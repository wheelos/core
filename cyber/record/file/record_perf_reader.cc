#include <errno.h>
#include <fcntl.h>
#include <liburing.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if __has_include(<valgrind/memcheck.h>)
#include <valgrind/memcheck.h>
#define WHEELOS_RECORD_PERF_HAS_VALGRIND 1
#endif

#include "google/protobuf/io/zero_copy_stream_impl_lite.h"
#include "google/protobuf/wire_format_lite.h"

#include "cyber/proto/record.pb.h"
#include "cyber/record/file/section.h"

namespace apollo {
namespace cyber {
namespace record {

struct ChunkMeta {
  uint64_t section_offset = 0;
  uint64_t payload_offset = 0;
  uint32_t payload_size = 0;
};

struct UsageSnapshot {
  double user_cpu_ms = 0.0;
  double sys_cpu_ms = 0.0;
  long max_rss_kb = 0;
};

struct RunningStats {
  uint64_t count = 0;
  double mean = 0.0;
  double m2 = 0.0;

  void Add(double x) {
    ++count;
    const double delta = x - mean;
    mean += delta / static_cast<double>(count);
    const double delta2 = x - mean;
    m2 += delta * delta2;
  }

  double StdDev() const {
    if (count < 2) {
      return 0.0;
    }
    return std::sqrt(m2 / static_cast<double>(count - 1));
  }
};

struct PerfResult {
  std::string mode;
  uint64_t chunks = 0;
  uint64_t messages = 0;
  uint64_t bytes = 0;
  double wall_ms = 0.0;
  double throughput_mib_s = 0.0;
  double user_cpu_ms = 0.0;
  double sys_cpu_ms = 0.0;
  long max_rss_kb = 0;
  double per_message_jitter_us_stddev = 0.0;
  uint64_t pause_count = 0;
  double pause_duration_ms = 0.0;
  uint64_t queue_peak_bytes = 0;
  double publish_lag_us_stddev = 0.0;
};

struct HysteresisConfig {
  uint64_t high_watermark_bytes = 500ULL * 1024 * 1024;
  uint64_t low_watermark_bytes = 100ULL * 1024 * 1024;
  double replay_rate = 200.0;
};

struct QueuedMessage {
  proto::SingleMessage message;
  uint64_t byte_size = 0;
};

void MarkReadBufferDefined(const char* payload, size_t size) {
#if WHEELOS_RECORD_PERF_HAS_VALGRIND
  VALGRIND_MAKE_MEM_DEFINED(payload, size);
#else
  (void)payload;
  (void)size;
#endif
}

class ReservoirQueue {
 public:
  ReservoirQueue() = default;

  bool Push(QueuedMessage&& item) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (closed_) {
      return false;
    }
    bytes_in_queue_ += item.byte_size;
    peak_bytes_ = std::max(peak_bytes_, bytes_in_queue_);
    queue_.push_back(std::move(item));
    cv_not_empty_.notify_one();
    return true;
  }

  bool Pop(QueuedMessage* out) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_not_empty_.wait(lock, [this] { return closed_ || !queue_.empty(); });
    if (queue_.empty()) {
      return false;
    }
    *out = std::move(queue_.front());
    queue_.pop_front();
    bytes_in_queue_ -= out->byte_size;
    cv_watermark_.notify_all();
    return true;
  }

  void WaitUntilBelow(uint64_t lwm, bool* was_waiting, double* wait_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (bytes_in_queue_ <= lwm || closed_) {
      *was_waiting = false;
      *wait_ms = 0;
      return;
    }
    *was_waiting = true;
    const auto wait_start = std::chrono::steady_clock::now();
    cv_watermark_.wait(lock, [this, lwm] { return closed_ || bytes_in_queue_ <= lwm; });
    const auto wait_end = std::chrono::steady_clock::now();
    *wait_ms = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
                   wait_end - wait_start)
                   .count();
  }

  uint64_t BytesInQueue() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bytes_in_queue_;
  }

  uint64_t PeakBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return peak_bytes_;
  }

  void Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    cv_not_empty_.notify_all();
    cv_watermark_.notify_all();
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_not_empty_;
  std::condition_variable cv_watermark_;
  std::deque<QueuedMessage> queue_;
  uint64_t bytes_in_queue_ = 0;
  uint64_t peak_bytes_ = 0;
  bool closed_ = false;
};

bool PReadAll(int fd, void* output, size_t size, uint64_t offset) {
  char* out = reinterpret_cast<char*>(output);
  size_t copied = 0;
  while (copied < size) {
    const ssize_t n =
        pread(fd, out + copied, size - copied, static_cast<off_t>(offset + copied));
    if (n < 0) {
      return false;
    }
    if (n == 0) {
      return false;
    }
    copied += static_cast<size_t>(n);
  }
  return true;
}

UsageSnapshot CaptureUsage() {
  UsageSnapshot usage_snapshot;
  struct rusage usage = {};
  if (getrusage(RUSAGE_SELF, &usage) == 0) {
    usage_snapshot.user_cpu_ms =
        usage.ru_utime.tv_sec * 1000.0 + usage.ru_utime.tv_usec / 1000.0;
    usage_snapshot.sys_cpu_ms =
        usage.ru_stime.tv_sec * 1000.0 + usage.ru_stime.tv_usec / 1000.0;
    usage_snapshot.max_rss_kb = usage.ru_maxrss;
  }
  return usage_snapshot;
}

bool LoadChunkIndex(const std::string& path, int* fd, proto::Header* header,
                    std::vector<ChunkMeta>* chunks, uint32_t* max_chunk_size) {
  *fd = open(path.c_str(), O_RDONLY);
  if (*fd < 0) {
    std::cerr << "open_failed errno=" << errno << "\n";
    return false;
  }

  Section head_section = {};
  if (!PReadAll(*fd, &head_section, sizeof(head_section), 0)) {
    std::cerr << "read_header_section_failed\n";
    return false;
  }
  if (head_section.type != proto::SectionType::SECTION_HEADER || head_section.size <= 0) {
    std::cerr << "invalid_header_section\n";
    return false;
  }

  std::vector<char> header_buf(static_cast<size_t>(head_section.size));
  if (!PReadAll(*fd, header_buf.data(), header_buf.size(), sizeof(head_section))) {
    std::cerr << "read_header_payload_failed\n";
    return false;
  }
  if (!header->ParseFromArray(header_buf.data(), static_cast<int>(header_buf.size()))) {
    std::cerr << "parse_header_failed\n";
    return false;
  }
  if (!header->has_index_position()) {
    std::cerr << "header_has_no_index_position\n";
    return false;
  }

  Section index_section = {};
  if (!PReadAll(*fd, &index_section, sizeof(index_section), header->index_position())) {
    std::cerr << "read_index_section_failed\n";
    return false;
  }
  if (index_section.type != proto::SectionType::SECTION_INDEX || index_section.size <= 0) {
    std::cerr << "invalid_index_section\n";
    return false;
  }

  std::vector<char> index_buf(static_cast<size_t>(index_section.size));
  if (!PReadAll(*fd, index_buf.data(), index_buf.size(),
                header->index_position() + sizeof(index_section))) {
    std::cerr << "read_index_payload_failed\n";
    return false;
  }
  proto::Index index;
  if (!index.ParseFromArray(index_buf.data(), static_cast<int>(index_buf.size()))) {
    std::cerr << "parse_index_failed\n";
    return false;
  }

  chunks->clear();
  *max_chunk_size = 0;
  for (const auto& row : index.indexes()) {
    if (row.type() != proto::SectionType::SECTION_CHUNK_BODY) {
      continue;
    }
    Section chunk_section = {};
    if (!PReadAll(*fd, &chunk_section, sizeof(chunk_section), row.position())) {
      std::cerr << "read_chunk_section_failed offset=" << row.position() << "\n";
      return false;
    }
    if (chunk_section.type != proto::SectionType::SECTION_CHUNK_BODY || chunk_section.size <= 0) {
      continue;
    }
    ChunkMeta meta;
    meta.section_offset = row.position();
    meta.payload_offset = row.position() + sizeof(Section);
    meta.payload_size = static_cast<uint32_t>(chunk_section.size);
    chunks->push_back(meta);
    *max_chunk_size = std::max(*max_chunk_size, meta.payload_size);
  }

  if (chunks->empty()) {
    std::cerr << "no_chunk_body_found\n";
    return false;
  }
  return true;
}

void UpdateMessageJitter(const proto::ChunkBody& chunk_body, RunningStats* stats) {
  bool has_prev = false;
  std::chrono::steady_clock::time_point prev;
  for (int i = 0; i < chunk_body.messages_size(); ++i) {
    const auto now = std::chrono::steady_clock::now();
    if (has_prev) {
      const auto dt = std::chrono::duration_cast<std::chrono::duration<double, std::micro>>(
                          now - prev)
                          .count();
      stats->Add(dt);
    }
    prev = now;
    has_prev = true;
  }
}

bool ParseChunkBodyStreamingWithCallback(
    const char* payload, size_t size,
    const std::function<bool(const proto::SingleMessage&)>& on_message) {
  if (size > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return false;
  }
  google::protobuf::io::ArrayInputStream array_input(payload, static_cast<int>(size));
  google::protobuf::io::CodedInputStream coded_input(&array_input);
  proto::SingleMessage reusable_message;

  while (true) {
    const uint32_t tag = coded_input.ReadTag();
    if (tag == 0) {
      break;
    }
    if (google::protobuf::internal::WireFormatLite::GetTagFieldNumber(tag) != 1) {
      if (!google::protobuf::internal::WireFormatLite::SkipField(&coded_input, tag)) {
        return false;
      }
      continue;
    }
    if (google::protobuf::internal::WireFormatLite::GetTagWireType(tag) !=
        google::protobuf::internal::WireFormatLite::WIRETYPE_LENGTH_DELIMITED) {
      return false;
    }
    uint32_t len = 0;
    if (!coded_input.ReadVarint32(&len)) {
      return false;
    }
    const int bytes_until_limit = coded_input.BytesUntilLimit();
    if (bytes_until_limit >= 0 &&
        static_cast<uint64_t>(len) > static_cast<uint64_t>(bytes_until_limit)) {
      return false;
    }
    auto limit = coded_input.PushLimit(static_cast<int>(len));
    reusable_message.Clear();
    if (!reusable_message.ParseFromCodedStream(&coded_input)) {
      return false;
    }
    if (!coded_input.ConsumedEntireMessage()) {
      return false;
    }
    coded_input.PopLimit(limit);
    if (!on_message(reusable_message)) {
      return false;
    }
  }
  return true;
}

bool RunBaselinePRead(int fd, const std::vector<ChunkMeta>& chunks, uint64_t chunk_limit,
                      PerfResult* result) {
  proto::ChunkBody chunk_body;
  RunningStats jitter;
  UsageSnapshot before = CaptureUsage();
  const auto start = std::chrono::steady_clock::now();

  const uint64_t limit = chunk_limit == 0 ? static_cast<uint64_t>(chunks.size())
                                          : std::min<uint64_t>(chunk_limit, chunks.size());
  for (uint64_t i = 0; i < limit; ++i) {
    const auto& meta = chunks[i];
    std::string payload(meta.payload_size, '\0');
    if (!PReadAll(fd, &payload[0], payload.size(), meta.payload_offset)) {
      std::cerr << "baseline_pread_failed chunk=" << i << "\n";
      return false;
    }
    chunk_body.Clear();
    google::protobuf::io::ArrayInputStream stream(payload.data(),
                                                  static_cast<int>(payload.size()));
    if (!chunk_body.ParseFromZeroCopyStream(&stream)) {
      std::cerr << "baseline_parse_failed chunk=" << i << "\n";
      return false;
    }
    result->messages += static_cast<uint64_t>(chunk_body.messages_size());
    result->bytes += meta.payload_size;
    ++result->chunks;
    UpdateMessageJitter(chunk_body, &jitter);
  }

  const auto end = std::chrono::steady_clock::now();
  UsageSnapshot after = CaptureUsage();
  result->mode = "baseline_pread";
  result->wall_ms =
      std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - start)
          .count();
  result->throughput_mib_s =
      result->wall_ms > 0 ? (result->bytes / (1024.0 * 1024.0)) / (result->wall_ms / 1000.0)
                          : 0;
  result->user_cpu_ms = after.user_cpu_ms - before.user_cpu_ms;
  result->sys_cpu_ms = after.sys_cpu_ms - before.sys_cpu_ms;
  result->max_rss_kb = after.max_rss_kb;
  result->per_message_jitter_us_stddev = jitter.StdDev();
  return true;
}

bool SubmitReadFixed(io_uring* ring, int fd, void* buffer, uint32_t size, uint64_t offset,
                     int buf_index, uint64_t tag) {
  struct io_uring_sqe* sqe = io_uring_get_sqe(ring);
  if (sqe == nullptr) {
    return false;
  }
  io_uring_prep_read_fixed(sqe, fd, buffer, size, static_cast<off_t>(offset), buf_index);
  io_uring_sqe_set_data64(sqe, tag);
  return io_uring_submit(ring) >= 0;
}

bool WaitCompletion(io_uring* ring, struct io_uring_cqe** cqe) {
  if (io_uring_peek_cqe(ring, cqe) == 0) {
    return true;
  }
  return io_uring_wait_cqe(ring, cqe) == 0;
}

bool RunIoUringPingPong(int fd, const std::vector<ChunkMeta>& chunks, uint32_t slot_size,
                        uint64_t chunk_limit, bool stream_parse, PerfResult* result) {
  if (slot_size == 0 || slot_size > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
    return false;
  }

  io_uring ring = {};
  if (io_uring_queue_init(64, &ring, 0) < 0) {
    return false;
  }

  void* slot0 = nullptr;
  void* slot1 = nullptr;
  if (posix_memalign(&slot0, 4096, slot_size) != 0 ||
      posix_memalign(&slot1, 4096, slot_size) != 0) {
    if (slot0 != nullptr) free(slot0);
    if (slot1 != nullptr) free(slot1);
    io_uring_queue_exit(&ring);
    return false;
  }

  std::vector<struct iovec> iovecs(2);
  iovecs[0].iov_base = slot0;
  iovecs[0].iov_len = slot_size;
  iovecs[1].iov_base = slot1;
  iovecs[1].iov_len = slot_size;
  if (io_uring_register_buffers(&ring, iovecs.data(), iovecs.size()) < 0) {
    free(slot0);
    free(slot1);
    io_uring_queue_exit(&ring);
    return false;
  }

  RunningStats jitter;
  UsageSnapshot before = CaptureUsage();
  const auto start = std::chrono::steady_clock::now();
  const uint64_t limit = chunk_limit == 0 ? static_cast<uint64_t>(chunks.size())
                                          : std::min<uint64_t>(chunk_limit, chunks.size());
  if (!SubmitReadFixed(&ring, fd, slot0, chunks[0].payload_size, chunks[0].payload_offset, 0, 0)) {
    io_uring_unregister_buffers(&ring);
    free(slot0);
    free(slot1);
    io_uring_queue_exit(&ring);
    return false;
  }

  for (uint64_t i = 0; i < limit; ++i) {
    struct io_uring_cqe* cqe = nullptr;
    if (!WaitCompletion(&ring, &cqe)) {
      io_uring_unregister_buffers(&ring);
      free(slot0);
      free(slot1);
      io_uring_queue_exit(&ring);
      return false;
    }
    if (cqe->res < 0) {
      io_uring_cqe_seen(&ring, cqe);
      io_uring_unregister_buffers(&ring);
      free(slot0);
      free(slot1);
      io_uring_queue_exit(&ring);
      return false;
    }
    const auto& meta = chunks[i];
    if (cqe->res != static_cast<int>(meta.payload_size)) {
      io_uring_cqe_seen(&ring, cqe);
      io_uring_unregister_buffers(&ring);
      free(slot0);
      free(slot1);
      io_uring_queue_exit(&ring);
      return false;
    }
    const int slot_index = static_cast<int>(io_uring_cqe_get_data64(cqe) & 0x1ULL);
    io_uring_cqe_seen(&ring, cqe);

    if (i + 1 < limit) {
      const int next_slot = slot_index == 0 ? 1 : 0;
      const auto& next_meta = chunks[i + 1];
      if (!SubmitReadFixed(&ring, fd, next_slot == 0 ? slot0 : slot1, next_meta.payload_size,
                           next_meta.payload_offset, next_slot, i + 1)) {
        io_uring_unregister_buffers(&ring);
        free(slot0);
        free(slot1);
        io_uring_queue_exit(&ring);
        return false;
      }
    }

    const char* payload = reinterpret_cast<const char*>(slot_index == 0 ? slot0 : slot1);
    MarkReadBufferDefined(payload, meta.payload_size);
    if (stream_parse) {
      bool ok = ParseChunkBodyStreamingWithCallback(
          payload, meta.payload_size, [&](const proto::SingleMessage&) {
            ++result->messages;
            return true;
          });
      if (!ok) {
        io_uring_unregister_buffers(&ring);
        free(slot0);
        free(slot1);
        io_uring_queue_exit(&ring);
        return false;
      }
    } else {
      proto::ChunkBody chunk_body;
      google::protobuf::io::ArrayInputStream stream(payload, static_cast<int>(meta.payload_size));
      if (!chunk_body.ParseFromZeroCopyStream(&stream)) {
        io_uring_unregister_buffers(&ring);
        free(slot0);
        free(slot1);
        io_uring_queue_exit(&ring);
        return false;
      }
      result->messages += static_cast<uint64_t>(chunk_body.messages_size());
      UpdateMessageJitter(chunk_body, &jitter);
    }
    result->bytes += meta.payload_size;
    ++result->chunks;
  }

  const auto end = std::chrono::steady_clock::now();
  UsageSnapshot after = CaptureUsage();
  result->mode = stream_parse ? "io_uring_pingpong_stream_parse" : "io_uring_pingpong";
  result->wall_ms =
      std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - start)
          .count();
  result->throughput_mib_s =
      result->wall_ms > 0 ? (result->bytes / (1024.0 * 1024.0)) / (result->wall_ms / 1000.0)
                          : 0;
  result->user_cpu_ms = after.user_cpu_ms - before.user_cpu_ms;
  result->sys_cpu_ms = after.sys_cpu_ms - before.sys_cpu_ms;
  result->max_rss_kb = after.max_rss_kb;
  result->per_message_jitter_us_stddev = jitter.StdDev();

  io_uring_unregister_buffers(&ring);
  free(slot0);
  free(slot1);
  io_uring_queue_exit(&ring);
  return true;
}

bool RunIoUringHysteresisReplay(int fd, const std::vector<ChunkMeta>& chunks, uint32_t slot_size,
                                uint64_t chunk_limit, const HysteresisConfig& config,
                                PerfResult* result) {
  if (slot_size == 0 || slot_size > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
    return false;
  }
  if (config.low_watermark_bytes >= config.high_watermark_bytes) {
    std::cerr << "invalid_hysteresis_watermark\n";
    return false;
  }
  const uint64_t limit = chunk_limit == 0 ? static_cast<uint64_t>(chunks.size())
                                          : std::min<uint64_t>(chunk_limit, chunks.size());
  io_uring ring = {};
  if (io_uring_queue_init(64, &ring, 0) < 0) {
    return false;
  }
  void* slot0 = nullptr;
  void* slot1 = nullptr;
  if (posix_memalign(&slot0, 4096, slot_size) != 0 ||
      posix_memalign(&slot1, 4096, slot_size) != 0) {
    if (slot0 != nullptr) free(slot0);
    if (slot1 != nullptr) free(slot1);
    io_uring_queue_exit(&ring);
    return false;
  }
  std::vector<struct iovec> iovecs(2);
  iovecs[0].iov_base = slot0;
  iovecs[0].iov_len = slot_size;
  iovecs[1].iov_base = slot1;
  iovecs[1].iov_len = slot_size;
  if (io_uring_register_buffers(&ring, iovecs.data(), iovecs.size()) < 0) {
    free(slot0);
    free(slot1);
    io_uring_queue_exit(&ring);
    return false;
  }

  ReservoirQueue queue;
  std::atomic<bool> producer_done{false};
  std::atomic<bool> failed{false};
  RunningStats publish_jitter;
  RunningStats publish_lag;
  RunningStats parse_gap;

  UsageSnapshot before = CaptureUsage();
  const auto start = std::chrono::steady_clock::now();

  std::thread publisher([&]() {
    bool have_base = false;
    uint64_t base_msg_time_ns = 0;
    std::chrono::steady_clock::time_point base_wall;
    std::chrono::steady_clock::time_point prev_publish;
    while (true) {
      QueuedMessage item;
      if (!queue.Pop(&item)) {
        break;
      }
      const auto now = std::chrono::steady_clock::now();
      if (!have_base) {
        have_base = true;
        base_msg_time_ns = item.message.time();
        base_wall = now;
        prev_publish = now;
      }
      const uint64_t logical_ns = item.message.time() - base_msg_time_ns;
      const double scaled_ns = logical_ns / std::max(1.0, config.replay_rate);
      const auto target_time = base_wall + std::chrono::nanoseconds(static_cast<int64_t>(scaled_ns));
      if (target_time > now) {
        std::this_thread::sleep_until(target_time);
      }
      const auto publish_time = std::chrono::steady_clock::now();
      const auto gap_us =
          std::chrono::duration_cast<std::chrono::duration<double, std::micro>>(publish_time -
                                                                                 prev_publish)
              .count();
      publish_jitter.Add(gap_us);
      prev_publish = publish_time;
      const auto lag_us =
          std::chrono::duration_cast<std::chrono::duration<double, std::micro>>(publish_time -
                                                                                 target_time)
              .count();
      publish_lag.Add(lag_us);
    }
  });

  bool paused_by_hwm = false;
  auto pause_start = std::chrono::steady_clock::now();
  bool parse_have_prev = false;
  std::chrono::steady_clock::time_point parse_prev;

  if (!SubmitReadFixed(&ring, fd, slot0, chunks[0].payload_size, chunks[0].payload_offset, 0, 0)) {
    failed = true;
  }

  for (uint64_t i = 0; i < limit && !failed.load(); ++i) {
    struct io_uring_cqe* cqe = nullptr;
    if (!WaitCompletion(&ring, &cqe)) {
      failed = true;
      break;
    }
    if (cqe->res < 0) {
      io_uring_cqe_seen(&ring, cqe);
      failed = true;
      break;
    }
    const auto& meta = chunks[i];
    if (cqe->res != static_cast<int>(meta.payload_size)) {
      io_uring_cqe_seen(&ring, cqe);
      failed = true;
      break;
    }
    const int slot_index = static_cast<int>(io_uring_cqe_get_data64(cqe) & 0x1ULL);
    io_uring_cqe_seen(&ring, cqe);

    const char* payload = reinterpret_cast<const char*>(slot_index == 0 ? slot0 : slot1);
    MarkReadBufferDefined(payload, meta.payload_size);
    bool parse_ok = ParseChunkBodyStreamingWithCallback(
        payload, meta.payload_size, [&](const proto::SingleMessage& msg) {
          while (queue.BytesInQueue() >= config.high_watermark_bytes) {
            if (!paused_by_hwm) {
              paused_by_hwm = true;
              pause_start = std::chrono::steady_clock::now();
              ++result->pause_count;
            }
            bool waited = false;
            double wait_ms = 0.0;
            queue.WaitUntilBelow(config.low_watermark_bytes, &waited, &wait_ms);
            if (waited) {
              result->pause_duration_ms += wait_ms;
            }
            if (failed.load()) {
              return false;
            }
          }
          if (paused_by_hwm && queue.BytesInQueue() <= config.low_watermark_bytes) {
            paused_by_hwm = false;
          }

          QueuedMessage item;
          item.message = msg;
          item.byte_size = msg.content().size() + msg.channel_name().size() + 32;
          if (!queue.Push(std::move(item))) {
            return false;
          }
          const auto parse_now = std::chrono::steady_clock::now();
          if (parse_have_prev) {
            const auto dt = std::chrono::duration_cast<std::chrono::duration<double, std::micro>>(
                                parse_now - parse_prev)
                                .count();
            parse_gap.Add(dt);
          }
          parse_prev = parse_now;
          parse_have_prev = true;
          ++result->messages;
          return true;
        });
    if (!parse_ok) {
      failed = true;
      break;
    }

    result->bytes += meta.payload_size;
    ++result->chunks;
    if (i + 1 < limit && !failed.load()) {
      const int next_slot = slot_index == 0 ? 1 : 0;
      const auto& next_meta = chunks[i + 1];
      if (!SubmitReadFixed(&ring, fd, next_slot == 0 ? slot0 : slot1, next_meta.payload_size,
                           next_meta.payload_offset, next_slot, i + 1)) {
        failed = true;
        break;
      }
    }
  }

  producer_done = true;
  queue.Close();
  if (publisher.joinable()) {
    publisher.join();
  }
  const auto end = std::chrono::steady_clock::now();
  UsageSnapshot after = CaptureUsage();

  io_uring_unregister_buffers(&ring);
  free(slot0);
  free(slot1);
  io_uring_queue_exit(&ring);

  if (failed.load()) {
    return false;
  }

  result->mode = "io_uring_hysteresis";
  result->wall_ms =
      std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - start)
          .count();
  result->throughput_mib_s =
      result->wall_ms > 0 ? (result->bytes / (1024.0 * 1024.0)) / (result->wall_ms / 1000.0)
                          : 0;
  result->user_cpu_ms = after.user_cpu_ms - before.user_cpu_ms;
  result->sys_cpu_ms = after.sys_cpu_ms - before.sys_cpu_ms;
  result->max_rss_kb = after.max_rss_kb;
  result->per_message_jitter_us_stddev = publish_jitter.StdDev();
  result->queue_peak_bytes = queue.PeakBytes();
  result->publish_lag_us_stddev = publish_lag.StdDev();
  return true;
}

void PrintResult(const PerfResult& result, const std::string& path, uint32_t max_chunk,
                 const HysteresisConfig& config) {
  std::cout << "{";
  std::cout << "\"mode\":\"" << result.mode << "\",";
  std::cout << "\"file\":\"" << path << "\",";
  std::cout << "\"max_chunk_size_bytes\":" << max_chunk << ",";
  std::cout << "\"hwm_bytes\":" << config.high_watermark_bytes << ",";
  std::cout << "\"lwm_bytes\":" << config.low_watermark_bytes << ",";
  std::cout << "\"replay_rate\":" << config.replay_rate << ",";
  std::cout << "\"chunks\":" << result.chunks << ",";
  std::cout << "\"messages\":" << result.messages << ",";
  std::cout << "\"bytes\":" << result.bytes << ",";
  std::cout << "\"wall_ms\":" << result.wall_ms << ",";
  std::cout << "\"throughput_mib_s\":" << result.throughput_mib_s << ",";
  std::cout << "\"user_cpu_ms\":" << result.user_cpu_ms << ",";
  std::cout << "\"sys_cpu_ms\":" << result.sys_cpu_ms << ",";
  std::cout << "\"max_rss_kb\":" << result.max_rss_kb << ",";
  std::cout << "\"jitter_stddev_us\":" << result.per_message_jitter_us_stddev << ",";
  std::cout << "\"pause_count\":" << result.pause_count << ",";
  std::cout << "\"pause_duration_ms\":" << result.pause_duration_ms << ",";
  std::cout << "\"queue_peak_bytes\":" << result.queue_peak_bytes << ",";
  std::cout << "\"publish_lag_stddev_us\":" << result.publish_lag_us_stddev;
  std::cout << "}\n";
}

}  // namespace record
}  // namespace cyber
}  // namespace apollo

int main(int argc, char** argv) {
  std::string path = "/mnt/synology/apollo/sensor_rgb.record";
  std::string mode = "baseline";
  uint64_t chunk_limit = 0;
  apollo::cyber::record::HysteresisConfig config;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg.rfind("--file=", 0) == 0) {
      path = arg.substr(strlen("--file="));
    } else if (arg.rfind("--mode=", 0) == 0) {
      mode = arg.substr(strlen("--mode="));
    } else if (arg.rfind("--max_chunks=", 0) == 0) {
      chunk_limit = static_cast<uint64_t>(
          std::strtoull(arg.substr(strlen("--max_chunks=")).c_str(), nullptr, 10));
    } else if (arg.rfind("--hwm_mb=", 0) == 0) {
      config.high_watermark_bytes =
          static_cast<uint64_t>(std::strtoull(arg.substr(strlen("--hwm_mb=")).c_str(), nullptr,
                                              10)) *
          1024ULL * 1024ULL;
    } else if (arg.rfind("--lwm_mb=", 0) == 0) {
      config.low_watermark_bytes =
          static_cast<uint64_t>(std::strtoull(arg.substr(strlen("--lwm_mb=")).c_str(), nullptr,
                                              10)) *
          1024ULL * 1024ULL;
    } else if (arg.rfind("--replay_rate=", 0) == 0) {
      config.replay_rate = std::max(1.0, std::atof(arg.substr(strlen("--replay_rate=")).c_str()));
    }
  }

  int fd = -1;
  apollo::cyber::proto::Header header;
  std::vector<apollo::cyber::record::ChunkMeta> chunks;
  uint32_t max_chunk_size = 0;
  if (!apollo::cyber::record::LoadChunkIndex(path, &fd, &header, &chunks, &max_chunk_size)) {
    if (fd >= 0) {
      close(fd);
    }
    return 1;
  }
  if (fd < 0) {
    return 1;
  }

  apollo::cyber::record::PerfResult result;
  bool ok = false;
  if (mode == "baseline") {
    ok = apollo::cyber::record::RunBaselinePRead(fd, chunks, chunk_limit, &result);
  } else if (mode == "uring") {
    ok = apollo::cyber::record::RunIoUringPingPong(fd, chunks, max_chunk_size, chunk_limit, false,
                                                   &result);
  } else if (mode == "uring_stream") {
    ok = apollo::cyber::record::RunIoUringPingPong(fd, chunks, max_chunk_size, chunk_limit, true,
                                                   &result);
  } else if (mode == "uring_hysteresis") {
    ok = apollo::cyber::record::RunIoUringHysteresisReplay(fd, chunks, max_chunk_size, chunk_limit,
                                                           config, &result);
  } else {
    std::cerr << "unsupported_mode=" << mode << "\n";
  }
  close(fd);
  if (!ok) {
    return 1;
  }
  apollo::cyber::record::PrintResult(result, path, max_chunk_size, config);
  return 0;
}
