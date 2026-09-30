#include "cyber/metrics/metrics.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace apollo {
namespace cyber {
namespace metrics {
namespace {

constexpr uint64_t kWindowNs = 60000000000ULL;
constexpr uint64_t kMinNs = 1000;
constexpr uint64_t kMaxNs = kWindowNs;
constexpr size_t kSubdivisions = 32;

uint64_t Window(TimePoint now) {
  auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                now.time_since_epoch())
                .count();
  return ns < 0 ? 0 : static_cast<uint64_t>(ns) / kWindowNs;
}

uint64_t Second(TimePoint now) {
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      now.time_since_epoch())
                      .count();
  return ns < 0 ? 0 : static_cast<uint64_t>(ns) / 1000000000ULL;
}

size_t Bucket(uint64_t ns) {
  if (ns < kMinNs) return 0;
  if (ns > kMaxNs) return 26 * kSubdivisions + 1;
  return 1 + std::min<size_t>(26 * kSubdivisions - 1,
                              static_cast<size_t>(
                                  std::log2(static_cast<double>(ns) / kMinNs) *
                                  kSubdivisions));
}

uint64_t BucketValue(size_t bucket) {
  if (bucket == 0) return kMinNs;
  if (bucket > 26 * kSubdivisions) return kMaxNs;
  return static_cast<uint64_t>(
      kMinNs *
      std::exp2((static_cast<double>(bucket - 1) + 0.5) / kSubdivisions));
}

class JsonWriter {
 public:
  explicit JsonWriter(size_t endpoints) {
    output_.reserve(256 + endpoints * 2700);
  }

  JsonWriter& operator<<(std::string_view text) {
    output_.append(text);
    return *this;
  }

  JsonWriter& operator<<(uint64_t value) {
    char digits[20];
    const auto result = std::to_chars(digits, digits + sizeof(digits), value);
    if (result.ec != std::errc{}) {
      throw std::runtime_error("Runtime Metrics integer JSON encoding failed");
    }
    output_.append(digits, result.ptr);
    return *this;
  }

  JsonWriter& operator<<(double value) {
    char digits[64];
    const auto result = std::to_chars(digits, digits + sizeof(digits), value,
                                      std::chars_format::general, 6);
    if (result.ec != std::errc{}) {
      throw std::runtime_error("Runtime Metrics rate JSON encoding failed");
    }
    output_.append(digits, result.ptr);
    return *this;
  }

  void AppendEscaped(const std::string& value) {
    constexpr char kHex[] = "0123456789abcdef";
    for (unsigned char c : value) {
      switch (c) {
        case '"':
          output_ += "\\\"";
          break;
        case '\\':
          output_ += "\\\\";
          break;
        case '\b':
          output_ += "\\b";
          break;
        case '\f':
          output_ += "\\f";
          break;
        case '\n':
          output_ += "\\n";
          break;
        case '\r':
          output_ += "\\r";
          break;
        case '\t':
          output_ += "\\t";
          break;
        default:
          if (c < 0x20) {
            output_ += "\\u00";
            output_ += kHex[c >> 4];
            output_ += kHex[c & 0xf];
          } else {
            output_ += static_cast<char>(c);
          }
      }
    }
  }

  std::string Take() { return std::move(output_); }

 private:
  std::string output_;
};

void OptionalJson(JsonWriter& out, const std::optional<uint64_t>& n) {
  if (n)
    out << *n;
  else
    out << "null";
}

void DistributionJson(JsonWriter& out, const Distribution& d) {
  out << "{\"count\":" << d.count << ",\"window_seconds\":" << d.window_seconds
      << ",\"window_start_mono_ns\":" << d.window_start_mono_ns
      << ",\"window_end_mono_ns\":" << d.window_end_mono_ns << ",\"p50_ns\":";
  OptionalJson(out, d.p50_ns);
  out << ",\"p90_ns\":";
  OptionalJson(out, d.p90_ns);
  out << ",\"p99_ns\":";
  OptionalJson(out, d.p99_ns);
  out << ",\"p999_ns\":";
  OptionalJson(out, d.p999_ns);
  out << ",\"max_ns\":";
  OptionalJson(out, d.max_ns);
  out << ",\"underflow_count\":" << d.underflow_count
      << ",\"overflow_count\":" << d.overflow_count
      << ",\"sampled_count\":" << d.count
      << ",\"sampling_ratio\":1,\"insufficient_p999_samples\":"
      << (d.count < 1000 ? "true" : "false") << ",\"validity\":\""
      << (d.count == 0                            ? "no_samples"
          : d.underflow_count || d.overflow_count ? "range_limited"
                                                  : "valid")
      << "\"}";
}

void NotApplicableJson(JsonWriter& out) {
  out << "{\"count\":0,\"p50_ns\":null,\"p90_ns\":null,"
         "\"p99_ns\":null,\"p999_ns\":null,\"max_ns\":null,"
         "\"reason\":\"not_applicable\"}";
}

}  // namespace

void Histogram::Observe(std::chrono::nanoseconds duration, TimePoint now) {
  if (duration.count() < 0) return;
  std::lock_guard<std::mutex> lock(mutex_);
  const uint64_t current = Window(now);
  if (current < epoch_) return;
  if (current != epoch_) {
    buckets_.fill(0);
    count_ = 0;
    max_ns_ = 0;
    epoch_ = current;
  }
  const uint64_t ns = static_cast<uint64_t>(duration.count());
  ++buckets_[Bucket(ns)];
  ++count_;
  max_ns_ = std::max(max_ns_, ns);
}

Distribution Histogram::Snapshot(TimePoint now) const {
  std::lock_guard<std::mutex> lock(mutex_);
  Distribution result;
  result.window_start_mono_ns = Window(now) * kWindowNs;
  result.window_end_mono_ns = result.window_start_mono_ns + kWindowNs;
  if (Window(now) != epoch_ || count_ == 0) return result;
  result.count = count_;
  result.max_ns = max_ns_;
  result.underflow_count = buckets_[0];
  result.overflow_count = buckets_.back();
  const std::array<uint64_t, 4> ranks = {
      static_cast<uint64_t>(std::ceil(0.5 * count_)),
      static_cast<uint64_t>(std::ceil(0.9 * count_)),
      static_cast<uint64_t>(std::ceil(0.99 * count_)),
      static_cast<uint64_t>(std::ceil(0.999 * count_))};
  const std::array<std::optional<uint64_t>*, 4> percentiles = {
      &result.p50_ns, &result.p90_ns, &result.p99_ns, &result.p999_ns};
  size_t next = 0;
  uint64_t count = 0;
  for (size_t i = 0; i < buckets_.size(); ++i) {
    count += buckets_[i];
    if (count < ranks[next]) continue;
    const auto value = std::min(BucketValue(i), max_ns_);
    do {
      *percentiles[next++] = value;
    } while (next < ranks.size() && count >= ranks[next]);
    if (next == ranks.size()) break;
  }
  return result;
}

Endpoint::Endpoint(Kind kind, std::string channel, std::string node,
                   std::string consumer)
    : kind_(kind),
      channel_(std::move(channel)),
      node_(std::move(node)),
      consumer_(kind == Kind::Task ? "" : std::move(consumer)),
      task_(kind == Kind::Task ? std::move(consumer) : "") {
  if (kind == Kind::Writer) {
    publish_latency_ = std::make_unique<Histogram>();
  } else if (kind == Kind::Consumer) {
    queue_latency_ = std::make_unique<Histogram>();
    callback_latency_ = std::make_unique<Histogram>();
  } else if (kind == Kind::Task) {
    scheduling_latency_ = std::make_unique<Histogram>();
  }
}

void Endpoint::RecordRate(std::array<RateBucket, kRateSeconds>* buckets,
                          TimePoint now) {
  const auto second = Second(now);
  auto& bucket = (*buckets)[second % kRateSeconds];
  if (bucket.second > second) return;
  if (bucket.second != second) {
    bucket.second = second;
    bucket.count = 0;
  }
  ++bucket.count;
}

double Endpoint::Rate(const std::array<RateBucket, kRateSeconds>& buckets,
                      TimePoint now) const {
  const auto second = Second(now);
  uint64_t count = 0;
  for (const auto& bucket : buckets) {
    if (bucket.second <= second && second - bucket.second < kRateSeconds) {
      count += bucket.count;
    }
  }
  return count / static_cast<double>(kRateSeconds);
}

void Endpoint::RecordPublish(bool success, TimePoint now) {
  if (Registry::Instance().mode() == Mode::Off) return;
  std::lock_guard<std::mutex> lock(mutex_);
  const auto current = Window(now);
  if (current > epoch_) {
    counters_.queue_window_high_watermark = counters_.queue_depth;
    epoch_ = current;
  }
  ++counters_.publish_attempt_count;
  if (success) {
    ++counters_.publish_count;
    RecordRate(&publish_rate_buckets_, now);
  } else {
    ++counters_.publish_error_count;
  }
}

void Endpoint::RecordPublish(bool success, std::chrono::nanoseconds duration,
                             TimePoint now) {
  RecordPublish(success, now);
  if (publish_latency_ && Registry::Instance().mode() != Mode::Off) {
    publish_latency_->Observe(duration, now);
  }
}

void Endpoint::RecordReceive(TimePoint now) {
  if (Registry::Instance().mode() == Mode::Off) return;
  std::lock_guard<std::mutex> lock(mutex_);
  const auto current = Window(now);
  if (current > epoch_) {
    counters_.queue_window_high_watermark = counters_.queue_depth;
    epoch_ = current;
  }
  ++counters_.receive_count;
  RecordRate(&receive_rate_buckets_, now);
}

void Endpoint::RecordEnqueue(uint64_t count) {
  if (Registry::Instance().mode() == Mode::Off) return;
  std::lock_guard<std::mutex> lock(mutex_);
  counters_.enqueue_count += count;
}
void Endpoint::RecordDequeue(uint64_t count) {
  if (Registry::Instance().mode() == Mode::Off) return;
  std::lock_guard<std::mutex> lock(mutex_);
  counters_.dequeue_count += count;
}
void Endpoint::RecordDrop(DropReason reason, uint64_t count) {
  if (Registry::Instance().mode() == Mode::Off) return;
  std::lock_guard<std::mutex> lock(mutex_);
  counters_.drop_count[static_cast<size_t>(reason)] += count;
}
void Endpoint::RecordShutdownDiscard(uint64_t count) {
  if (Registry::Instance().mode() == Mode::Off) return;
  std::lock_guard<std::mutex> lock(mutex_);
  if (!counters_.shutdown_discard_count) {
    counters_.shutdown_discard_count = count;
  } else {
    *counters_.shutdown_discard_count += count;
  }
}
void Endpoint::SetQueueDepth(uint64_t depth, TimePoint now) {
  if (Registry::Instance().mode() == Mode::Off) return;
  std::lock_guard<std::mutex> lock(mutex_);
  const auto current = Window(now);
  if (current < epoch_) return;
  if (current > epoch_) {
    counters_.queue_window_high_watermark = counters_.queue_depth;
    epoch_ = current;
  }
  counters_.queue_depth = depth;
  counters_.queue_high_watermark =
      std::max(counters_.queue_high_watermark, depth);
  counters_.queue_window_high_watermark =
      std::max(counters_.queue_window_high_watermark, depth);
}
void Endpoint::CallbackStart() {
  if (Registry::Instance().mode() == Mode::Off) return;
  std::lock_guard<std::mutex> lock(mutex_);
  ++counters_.callback_count;
  ++counters_.callback_inflight;
}
void Endpoint::CallbackComplete(bool error) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!counters_.callback_inflight) return;
  --counters_.callback_inflight;
  ++counters_.callback_completed_count;
  if (error) ++counters_.callback_error_count;
}
void Endpoint::ObserveQueueLatency(std::chrono::nanoseconds duration,
                                   TimePoint now) {
  if (queue_latency_ && Registry::Instance().mode() != Mode::Off)
    queue_latency_->Observe(duration, now);
}
void Endpoint::ObserveCallbackLatency(std::chrono::nanoseconds duration,
                                      TimePoint now) {
  if (callback_latency_ && Registry::Instance().mode() != Mode::Off)
    callback_latency_->Observe(duration, now);
}
void Endpoint::ObserveSchedulingLatency(std::chrono::nanoseconds duration,
                                        TimePoint now) {
  if (scheduling_latency_ && duration.count() >= 0 &&
      Registry::Instance().mode() != Mode::Off) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++counters_.scheduling_count;
    }
    scheduling_latency_->Observe(duration, now);
  }
}
EndpointSnapshot Endpoint::Snapshot(TimePoint now) const {
  EndpointSnapshot result;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    result = counters_;
    result.kind = kind_;
    result.channel = channel_;
    result.node = node_;
    result.consumer = consumer_;
    result.task = task_;
    result.publish_rate = Rate(publish_rate_buckets_, now);
    result.receive_rate = Rate(receive_rate_buckets_, now);
    result.rate_elapsed_seconds = kRateSeconds;
    if (Window(now) != epoch_) {
      result.queue_window_high_watermark = result.queue_depth;
    }
  }
  if (queue_latency_) result.queue_latency = queue_latency_->Snapshot(now);
  if (callback_latency_)
    result.callback_latency = callback_latency_->Snapshot(now);
  if (publish_latency_)
    result.publish_latency = publish_latency_->Snapshot(now);
  if (scheduling_latency_)
    result.scheduling_latency = scheduling_latency_->Snapshot(now);
  return result;
}

Registry::Registry() {
  std::random_device random;
  std::ostringstream id;
  id << getpid() << "-" << Clock::now().time_since_epoch().count() << "-"
     << std::hex << random() << random();
  process_instance_ = id.str();
}

Registry::~Registry() { StopExport(); }

bool Registry::StartExport(const std::string& absolute_path) {
  if (absolute_path.empty() || absolute_path.front() != '/' ||
      absolute_path.back() == '/') {
    return false;
  }
  const auto parent = absolute_path.substr(0, absolute_path.find_last_of('/'));
  struct stat directory;
  if (stat(parent.empty() ? "/" : parent.c_str(), &directory) != 0 ||
      !S_ISDIR(directory.st_mode) || directory.st_uid != geteuid() ||
      (directory.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
    return false;
  }
  std::lock_guard<std::mutex> lock(export_mutex_);
  if (exporter_.joinable()) {
    return false;
  }
  export_path_ = absolute_path;
  stop_export_ = false;
  if (!WriteSnapshot()) {
    export_path_.clear();
    return false;
  }
  exporter_ = std::thread(&Registry::ExportLoop, this);
  return true;
}

void Registry::StopExport() {
  {
    std::lock_guard<std::mutex> lock(export_mutex_);
    stop_export_ = true;
  }
  export_cv_.notify_one();
  if (exporter_.joinable()) {
    exporter_.join();
  }
}

void Registry::ExportLoop() {
  std::unique_lock<std::mutex> lock(export_mutex_);
  while (!export_cv_.wait_for(lock, std::chrono::seconds(1),
                              [this] { return stop_export_; })) {
    if (!WriteSnapshot()) {
      std::cerr << "Runtime Metrics snapshot export failed: " << export_path_
                << std::endl;
    }
  }
  if (!WriteSnapshot()) {
    std::cerr << "Runtime Metrics final snapshot export failed: "
              << export_path_ << std::endl;
  }
}

bool Registry::WriteSnapshot() const {
  const std::string temp_path = export_path_ + "." + process_instance_ + ".tmp";
  const int fd = open(temp_path.c_str(),
                      O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                      S_IRUSR | S_IWUSR);
  if (fd < 0) {
    return false;
  }
  const std::string json = Snapshot().ToJson() + "\n";
  size_t written = 0;
  while (written < json.size()) {
    const ssize_t size =
        write(fd, json.data() + written, json.size() - written);
    if (size <= 0) {
      close(fd);
      unlink(temp_path.c_str());
      return false;
    }
    written += size;
  }
  const bool success =
      close(fd) == 0 && rename(temp_path.c_str(), export_path_.c_str()) == 0;
  if (!success) {
    unlink(temp_path.c_str());
  }
  return success;
}

Registry& Registry::Instance() {
  static Registry instance;
  return instance;
}
void Registry::Configure(Mode mode) {
  mode_.store(mode, std::memory_order_relaxed);
}
std::shared_ptr<Endpoint> Registry::RegisterWriter(const std::string& channel,
                                                   const std::string& node) {
  return Register(Kind::Writer, channel, node, "");
}
std::shared_ptr<Endpoint> Registry::RegisterReceiver(
    const std::string& channel) {
  return Register(Kind::Receiver, channel, "", "");
}
std::shared_ptr<Endpoint> Registry::RegisterConsumer(
    const std::string& channel, const std::string& consumer) {
  return Register(Kind::Consumer, channel, "", consumer);
}
std::shared_ptr<Endpoint> Registry::RegisterTask(const std::string& task) {
  return Register(Kind::Task, "", "", task);
}
void Registry::Retire(const std::shared_ptr<Endpoint>& endpoint) {
  if (!endpoint || mode() == Mode::Off) return;
  std::lock_guard<std::mutex> lock(mutex_);
  if (endpoint->retired_.exchange(true, std::memory_order_relaxed)) return;
  auto snapshot = endpoint->Snapshot();
  snapshot.retired = true;
  snapshot.retired_at_unix_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();
  if (retired_endpoints_.size() == kMaxRetiredSnapshots) {
    retired_endpoints_.pop_front();
    ++retired_snapshots_dropped_;
  }
  retired_endpoints_.push_back(snapshot);
}
std::shared_ptr<Endpoint> Registry::Register(Kind kind,
                                             const std::string& channel,
                                             const std::string& node,
                                             const std::string& consumer) {
  if (mode() == Mode::Off) return nullptr;
  std::lock_guard<std::mutex> lock(mutex_);
  if (channel.size() > 256 || node.size() > 256 || consumer.size() > 256) {
    ++rejected_registrations_;
    std::cerr << "Runtime Metrics registration rejected: label too long"
              << std::endl;
    return nullptr;
  }
  if (endpoints_.size() >= kMaxSeries) {
    endpoints_.erase(std::remove_if(endpoints_.begin(), endpoints_.end(),
                                    [](const auto& e) { return e.expired(); }),
                     endpoints_.end());
  }
  if (endpoints_.size() >= kMaxSeries) {
    ++rejected_registrations_;
    std::cerr << "Runtime Metrics registration rejected: series limit reached"
              << std::endl;
    return nullptr;
  }
  auto endpoint = std::make_shared<Endpoint>(kind, channel, node, consumer);
  endpoints_.push_back(endpoint);
  return endpoint;
}
ProcessSnapshot Registry::Snapshot(TimePoint now) const {
  ProcessSnapshot result;
  result.generated_at_unix_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();
  std::vector<std::shared_ptr<Endpoint>> endpoints;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    result.process_instance = process_instance_;
    for (const auto& weak : endpoints_) {
      auto endpoint = weak.lock();
      if (!endpoint || endpoint->retired_.load(std::memory_order_relaxed))
        continue;
      endpoints.push_back(std::move(endpoint));
    }
  }

  std::vector<EndpointSnapshot> endpoint_snapshots;
  endpoint_snapshots.reserve(endpoints.size());
  for (const auto& endpoint : endpoints) {
    endpoint_snapshots.push_back(endpoint->Snapshot(now));
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    result.mode = mode();
    result.rejected_registrations = rejected_registrations_;
    result.retired_snapshots_dropped = retired_snapshots_dropped_;
    result.endpoints.assign(retired_endpoints_.begin(),
                            retired_endpoints_.end());
    for (size_t i = 0; i < endpoints.size(); ++i) {
      if (endpoints[i]->retired_.load(std::memory_order_relaxed)) continue;
      result.endpoints.push_back(std::move(endpoint_snapshots[i]));
      ++result.live_series;
    }
  }
  return result;
}

std::string ProcessSnapshot::ToJson() const {
  JsonWriter out(endpoints.size());
  out << "{\"schema_version\":1,\"process_instance\":\"";
  out.AppendEscaped(process_instance);
  out << "\",\"generated_at_unix_ns\":" << generated_at_unix_ns
      << ",\"mode\":\""
      << (mode == Mode::Off     ? "off"
          : mode == Mode::Basic ? "basic"
                                : "detailed")
      << "\",\"live_series\":" << live_series
      << ",\"rejected_registrations\":" << rejected_registrations
      << ",\"retired_snapshots_dropped\":" << retired_snapshots_dropped
      << ",\"endpoints\":[";
  bool first = true;
  for (const auto& e : endpoints) {
    if (!first) out << ",";
    first = false;
    out << "{\"kind\":\""
        << (e.kind == Kind::Writer     ? "writer"
            : e.kind == Kind::Receiver ? "receiver"
            : e.kind == Kind::Consumer ? "consumer"
                                       : "task")
        << "\",\"channel\":\"";
    out.AppendEscaped(e.channel);
    out << "\",\"node\":\"";
    out.AppendEscaped(e.node);
    out << "\",\"consumer\":\"";
    out.AppendEscaped(e.consumer);
    out << "\",\"task\":\"";
    out.AppendEscaped(e.task);
    out << "\",\"retired\":" << (e.retired ? "true" : "false")
        << ",\"retired_at_unix_ns\":";
    OptionalJson(out, e.retired_at_unix_ns);
    out << ",\"publish_attempt_count\":" << e.publish_attempt_count
        << ",\"publish_count\":" << e.publish_count
        << ",\"publish_error_count\":" << e.publish_error_count
        << ",\"receive_count\":" << e.receive_count
        << ",\"enqueue_count\":" << e.enqueue_count
        << ",\"dequeue_count\":" << e.dequeue_count << ",\"drop_count\":{"
        << "\"overflow_overwrite\":" << e.drop_count[0]
        << ",\"overflow_skip_to_latest\":" << e.drop_count[1]
        << ",\"initial_skip_to_latest\":" << e.drop_count[2]
        << ",\"shutdown_discard\":";
    OptionalJson(out, e.shutdown_discard_count);
    out << "},\"queue_depth\":" << e.queue_depth
        << ",\"queue_high_watermark\":" << e.queue_high_watermark
        << ",\"queue_window_high_watermark\":" << e.queue_window_high_watermark
        << ",\"callback_count\":" << e.callback_count
        << ",\"callback_completed_count\":" << e.callback_completed_count
        << ",\"callback_error_count\":" << e.callback_error_count
        << ",\"callback_inflight\":" << e.callback_inflight
        << ",\"scheduling_count\":" << e.scheduling_count
        << ",\"publish_rate\":";
    if (e.kind == Kind::Writer) {
      out << e.publish_rate;
    } else {
      out << "null";
    }
    out << ",\"receive_rate\":";
    if (e.kind == Kind::Receiver) {
      out << e.receive_rate;
    } else {
      out << "null";
    }
    out << ",\"rate_window_seconds\":" << e.rate_window_seconds
        << ",\"rate_elapsed_seconds\":" << e.rate_elapsed_seconds
        << ",\"queue_latency\":";
    if (e.kind == Kind::Consumer) {
      DistributionJson(out, e.queue_latency);
    } else {
      NotApplicableJson(out);
    }
    out << ",\"callback_latency\":";
    if (e.kind == Kind::Consumer) {
      DistributionJson(out, e.callback_latency);
    } else {
      NotApplicableJson(out);
    }
    out << ",\"publish_latency\":";
    if (e.kind == Kind::Writer) {
      DistributionJson(out, e.publish_latency);
    } else {
      NotApplicableJson(out);
    }
    out << ",\"scheduling_latency\":";
    if (e.kind == Kind::Task) {
      DistributionJson(out, e.scheduling_latency);
    } else {
      NotApplicableJson(out);
    }
    for (const char* stage :
         {"publish_prepare_latency", "serialization_latency",
          "shm_acquire_latency", "shm_write_latency", "transport_latency",
          "dispatch_latency", "callback_dispatch_latency",
          "end_to_end_latency"}) {
      out << ",\"" << stage
          << "\":{\"count\":0,\"p50_ns\":null,\"p90_ns\":null,"
             "\"p99_ns\":null,\"p999_ns\":null,\"max_ns\":null,"
             "\"reason\":\"not_instrumented\"}";
    }
    out << "}";
  }
  out << "]}";
  return out.Take();
}

}  // namespace metrics
}  // namespace cyber
}  // namespace apollo
