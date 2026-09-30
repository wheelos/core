#ifndef CYBER_METRICS_METRICS_H_
#define CYBER_METRICS_METRICS_H_

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace apollo {
namespace cyber {
namespace metrics {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

enum class Mode { Off, Basic, Detailed };
enum class Kind { Writer, Receiver, Consumer, Task };
enum class DropReason {
  OverflowOverwrite,
  OverflowSkipToLatest,
  InitialSkipToLatest,
};

struct Distribution {
  uint64_t count = 0;
  std::optional<uint64_t> p50_ns;
  std::optional<uint64_t> p90_ns;
  std::optional<uint64_t> p99_ns;
  std::optional<uint64_t> p999_ns;
  std::optional<uint64_t> max_ns;
  // Values outside [1 us, 60 s] have a range-limited percentile.
  uint64_t underflow_count = 0;
  uint64_t overflow_count = 0;
  uint64_t window_seconds = 60;
  uint64_t window_start_mono_ns = 0;
  uint64_t window_end_mono_ns = 0;
};

struct EndpointSnapshot {
  Kind kind;
  std::string channel;
  std::string node;
  std::string consumer;
  std::string task;
  bool retired = false;
  std::optional<uint64_t> retired_at_unix_ns;
  uint64_t publish_attempt_count = 0;
  uint64_t publish_count = 0;
  uint64_t publish_error_count = 0;
  uint64_t receive_count = 0;
  uint64_t enqueue_count = 0;
  uint64_t dequeue_count = 0;
  std::array<uint64_t, 3> drop_count{};
  uint64_t queue_depth = 0;
  uint64_t queue_high_watermark = 0;
  uint64_t queue_window_high_watermark = 0;
  uint64_t callback_count = 0;
  uint64_t callback_completed_count = 0;
  uint64_t callback_error_count = 0;
  uint64_t callback_inflight = 0;
  uint64_t scheduling_count = 0;
  std::optional<uint64_t> shutdown_discard_count;
  double publish_rate = 0;
  double receive_rate = 0;
  uint64_t rate_window_seconds = 60;
  double rate_elapsed_seconds = 0;
  Distribution queue_latency;
  Distribution callback_latency;
  Distribution publish_latency;
  Distribution scheduling_latency;
};

struct ProcessSnapshot {
  std::string process_instance;
  uint64_t generated_at_unix_ns = 0;
  Mode mode = Mode::Off;
  uint64_t rejected_registrations = 0;
  uint64_t retired_snapshots_dropped = 0;
  size_t live_series = 0;
  std::vector<EndpointSnapshot> endpoints;
  std::string ToJson() const;
};

class Histogram {
 public:
  void Observe(std::chrono::nanoseconds duration, TimePoint now = Clock::now());
  Distribution Snapshot(TimePoint now = Clock::now()) const;

 private:
  // 32 logarithmic subdivisions per octave: maximum bucket error < 1.1%.
  static constexpr size_t kBuckets = 26 * 32 + 2;
  mutable std::mutex mutex_;
  std::array<uint64_t, kBuckets> buckets_{};
  uint64_t epoch_ = 0;
  uint64_t count_ = 0;
  uint64_t max_ns_ = 0;
};

class Endpoint {
 public:
  Endpoint(Kind kind, std::string channel, std::string node,
           std::string consumer);

  // All duration observations accept an explicit monotonic sample time for
  // deterministic testing. Calling these methods never searches the registry.
  void RecordPublish(bool success, TimePoint now = Clock::now());
  void RecordPublish(bool success, std::chrono::nanoseconds duration,
                     TimePoint now = Clock::now());
  void RecordReceive(TimePoint now = Clock::now());
  void RecordEnqueue(uint64_t count = 1);
  void RecordDequeue(uint64_t count = 1);
  void RecordDrop(DropReason reason, uint64_t count = 1);
  void RecordShutdownDiscard(uint64_t count);
  void SetQueueDepth(uint64_t depth, TimePoint now = Clock::now());
  void CallbackStart();
  void CallbackComplete(bool error = false);
  void ObserveQueueLatency(std::chrono::nanoseconds duration,
                           TimePoint now = Clock::now());
  void ObserveCallbackLatency(std::chrono::nanoseconds duration,
                              TimePoint now = Clock::now());
  void ObserveSchedulingLatency(std::chrono::nanoseconds duration,
                                TimePoint now = Clock::now());
  EndpointSnapshot Snapshot(TimePoint now = Clock::now()) const;

 private:
  struct RateBucket {
    uint64_t second = 0;
    uint64_t count = 0;
  };
  static constexpr size_t kRateSeconds = 60;
  void RecordRate(std::array<RateBucket, kRateSeconds>* buckets, TimePoint now);
  double Rate(const std::array<RateBucket, kRateSeconds>& buckets,
              TimePoint now) const;

  Kind kind_;
  std::string channel_;
  std::string node_;
  std::string consumer_;
  std::string task_;
  mutable std::mutex mutex_;
  EndpointSnapshot counters_;
  uint64_t epoch_ = 0;
  std::array<RateBucket, kRateSeconds> publish_rate_buckets_{};
  std::array<RateBucket, kRateSeconds> receive_rate_buckets_{};
  std::unique_ptr<Histogram> queue_latency_;
  std::unique_ptr<Histogram> callback_latency_;
  std::unique_ptr<Histogram> publish_latency_;
  std::unique_ptr<Histogram> scheduling_latency_;
  std::atomic<bool> retired_{false};

  friend class Registry;
};

class Registry {
 public:
  static Registry& Instance();
  ~Registry();
  static constexpr size_t kMaxSeries = 4096;
  static constexpr size_t kMaxRetiredSnapshots = kMaxSeries;

  void Configure(Mode mode);
  bool StartExport(const std::string& absolute_path);
  void StopExport();
  Mode mode() const { return mode_.load(std::memory_order_relaxed); }
  std::shared_ptr<Endpoint> RegisterWriter(const std::string& channel,
                                           const std::string& node);
  std::shared_ptr<Endpoint> RegisterReceiver(const std::string& channel);
  std::shared_ptr<Endpoint> RegisterConsumer(const std::string& channel,
                                             const std::string& consumer);
  std::shared_ptr<Endpoint> RegisterTask(const std::string& task);
  // Preserve a bounded final summary without retaining hot-path endpoint state.
  void Retire(const std::shared_ptr<Endpoint>& endpoint);
  ProcessSnapshot Snapshot(TimePoint now = Clock::now()) const;

 private:
  Registry();
  std::shared_ptr<Endpoint> Register(Kind kind, const std::string& channel,
                                     const std::string& node,
                                     const std::string& consumer);
  std::atomic<Mode> mode_{Mode::Off};
  mutable std::mutex mutex_;
  std::vector<std::weak_ptr<Endpoint>> endpoints_;
  std::deque<EndpointSnapshot> retired_endpoints_;
  std::string process_instance_;
  uint64_t rejected_registrations_ = 0;
  uint64_t retired_snapshots_dropped_ = 0;
  std::thread exporter_;
  std::mutex export_mutex_;
  std::condition_variable export_cv_;
  std::string export_path_;
  bool stop_export_ = false;
  void ExportLoop();
  bool WriteSnapshot() const;
};

template <typename F>
auto MeasureCallback(const std::shared_ptr<Endpoint>& endpoint, F&& callback)
    -> decltype(callback()) {
  if (!endpoint) {
    return callback();
  }
  endpoint->CallbackStart();
  const auto start = Clock::now();
  try {
    if constexpr (std::is_same_v<decltype(callback()), bool>) {
      bool success = callback();
      endpoint->ObserveCallbackLatency(
          std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
                                                               start));
      endpoint->CallbackComplete(!success);
      return success;
    } else {
      callback();
      endpoint->ObserveCallbackLatency(
          std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
                                                               start));
      endpoint->CallbackComplete();
    }
  } catch (...) {
    endpoint->ObserveCallbackLatency(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
                                                             start));
    endpoint->CallbackComplete(true);
    throw;
  }
}

}  // namespace metrics
}  // namespace cyber
}  // namespace apollo

#endif  // CYBER_METRICS_METRICS_H_
