#include <errno.h>
#include <sys/resource.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <algorithm>

#include "cyber/common/file.h"
#include "cyber/record/file/record_file_reader.h"
#include "cyber/record/file/record_file_writer.h"
#include "cyber/record/header_builder.h"

namespace apollo {
namespace cyber {
namespace record {

using apollo::cyber::proto::Channel;
using apollo::cyber::proto::ChunkBody;
using apollo::cyber::proto::ChunkHeader;
using apollo::cyber::proto::SectionType;
using apollo::cyber::proto::SingleMessage;

struct IoStats {
  uint64_t rchar = 0;
  uint64_t wchar = 0;
  uint64_t read_bytes = 0;
  uint64_t write_bytes = 0;
};

struct UsageSnapshot {
  double user_cpu_ms = 0.0;
  double sys_cpu_ms = 0.0;
  long max_rss_kb = 0;
  IoStats io;
};

struct PerfResult {
  double wall_ms = 0.0;
  double throughput_mib_s = 0.0;
  double user_cpu_ms = 0.0;
  double sys_cpu_ms = 0.0;
  long max_rss_kb = 0;
  uint64_t io_rchar = 0;
  uint64_t io_wchar = 0;
  uint64_t io_read_bytes = 0;
  uint64_t io_write_bytes = 0;
  uint64_t payload_bytes = 0;
};

uint64_t ParseIoValue(const std::string& line) {
  const auto pos = line.find(':');
  if (pos == std::string::npos) {
    return 0;
  }
  return static_cast<uint64_t>(std::strtoull(line.c_str() + pos + 1, nullptr, 10));
}

IoStats ReadProcIo() {
  IoStats stats;
  std::ifstream input("/proc/self/io");
  if (!input.is_open()) {
    return stats;
  }
  std::string line;
  while (std::getline(input, line)) {
    if (line.rfind("rchar:", 0) == 0) {
      stats.rchar = ParseIoValue(line);
    } else if (line.rfind("wchar:", 0) == 0) {
      stats.wchar = ParseIoValue(line);
    } else if (line.rfind("read_bytes:", 0) == 0) {
      stats.read_bytes = ParseIoValue(line);
    } else if (line.rfind("write_bytes:", 0) == 0) {
      stats.write_bytes = ParseIoValue(line);
    }
  }
  return stats;
}

UsageSnapshot CaptureUsage() {
  UsageSnapshot snapshot;
  struct rusage usage = {};
  if (getrusage(RUSAGE_SELF, &usage) == 0) {
    snapshot.user_cpu_ms =
        usage.ru_utime.tv_sec * 1000.0 + usage.ru_utime.tv_usec / 1000.0;
    snapshot.sys_cpu_ms =
        usage.ru_stime.tv_sec * 1000.0 + usage.ru_stime.tv_usec / 1000.0;
    snapshot.max_rss_kb = usage.ru_maxrss;
  }
  snapshot.io = ReadProcIo();
  return snapshot;
}

PerfResult DiffResult(const UsageSnapshot& before, const UsageSnapshot& after,
                      double wall_ms, uint64_t payload_bytes) {
  PerfResult result;
  result.wall_ms = wall_ms;
  result.payload_bytes = payload_bytes;
  result.throughput_mib_s =
      wall_ms > 0 ? (payload_bytes / (1024.0 * 1024.0)) / (wall_ms / 1000.0) : 0;
  result.user_cpu_ms = after.user_cpu_ms - before.user_cpu_ms;
  result.sys_cpu_ms = after.sys_cpu_ms - before.sys_cpu_ms;
  result.max_rss_kb = after.max_rss_kb;
  result.io_rchar = after.io.rchar - before.io.rchar;
  result.io_wchar = after.io.wchar - before.io.wchar;
  result.io_read_bytes = after.io.read_bytes - before.io.read_bytes;
  result.io_write_bytes = after.io.write_bytes - before.io.write_bytes;
  return result;
}

bool WriteRecordFile(const std::string& path, int message_count,
                     int payload_size_bytes, int chunk_raw_size_bytes,
                     PerfResult* result) {
  RecordFileWriter writer;
  if (!writer.Open(path)) {
    std::cerr << "write_open_failed path=" << path << " errno=" << errno << "\n";
    return false;
  }

  auto header = HeaderBuilder::GetHeaderWithChunkParams(0, 0);
  header.set_segment_interval(0);
  header.set_segment_raw_size(0);
  header.set_chunk_interval(0);
  header.set_chunk_raw_size(chunk_raw_size_bytes);
  if (!writer.WriteHeader(header)) {
    std::cerr << "write_header_failed\n";
    return false;
  }

  Channel channel;
  channel.set_name("/perf/channel");
  channel.set_message_type("apollo.cyber.proto.Perf");
  channel.set_proto_desc("record_io_perf");
  if (!writer.WriteChannel(channel)) {
    std::cerr << "write_channel_failed\n";
    return false;
  }

  const std::string payload(payload_size_bytes, 'x');
  UsageSnapshot before = CaptureUsage();
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < message_count; ++i) {
    SingleMessage message;
    message.set_channel_name(channel.name());
    message.set_time(i + 1);
    message.set_content(payload);
    if (!writer.WriteMessage(message)) {
      std::cerr << "write_message_failed index=" << i << "\n";
      return false;
    }
  }
  writer.Close();
  const auto end = std::chrono::steady_clock::now();
  UsageSnapshot after = CaptureUsage();
  const double wall_ms =
      std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end -
                                                                             start)
          .count();
  *result = DiffResult(before, after, wall_ms,
                       static_cast<uint64_t>(message_count) * payload_size_bytes);
  return true;
}

bool ReadRecordFile(const std::string& path, PerfResult* result) {
  RecordFileReader reader;
  if (!reader.Open(path)) {
    std::cerr << "read_open_failed path=" << path << " errno=" << errno << "\n";
    return false;
  }

  UsageSnapshot before = CaptureUsage();
  const auto start = std::chrono::steady_clock::now();
  Section section;
  uint64_t payload_bytes = 0;
  while (reader.ReadSection(&section)) {
    switch (section.type) {
      case SectionType::SECTION_CHANNEL: {
        proto::Channel channel;
        if (!reader.ReadSection<proto::Channel>(section.size, &channel)) {
          return false;
        }
        break;
      }
      case SectionType::SECTION_CHUNK_HEADER: {
        ChunkHeader chunk_header;
        if (!reader.ReadSection<ChunkHeader>(section.size, &chunk_header)) {
          return false;
        }
        break;
      }
      case SectionType::SECTION_CHUNK_BODY: {
        ChunkBody chunk_body;
        if (!reader.ReadSection<ChunkBody>(section.size, &chunk_body)) {
          return false;
        }
        for (const auto& message : chunk_body.messages()) {
          payload_bytes += static_cast<uint64_t>(message.content().size());
        }
        break;
      }
      case SectionType::SECTION_INDEX: {
        proto::Index index;
        if (!reader.ReadSection<proto::Index>(section.size, &index)) {
          return false;
        }
        break;
      }
      default: {
        if (!reader.SkipSection(section.size)) {
          return false;
        }
        break;
      }
    }
  }
  const auto end = std::chrono::steady_clock::now();
  UsageSnapshot after = CaptureUsage();
  const double wall_ms =
      std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end -
                                                                             start)
          .count();
  *result = DiffResult(before, after, wall_ms, payload_bytes);
  reader.Close();
  return true;
}

std::string ToJson(const PerfResult& write_result, const PerfResult& read_result,
                   int message_count, int payload_size_bytes,
                   int chunk_raw_size_bytes) {
  std::ostringstream output;
  output << "{";
  output << "\"message_count\":" << message_count << ",";
  output << "\"payload_size_bytes\":" << payload_size_bytes << ",";
  output << "\"chunk_raw_size_bytes\":" << chunk_raw_size_bytes << ",";
  output << "\"total_payload_bytes\":"
         << (static_cast<uint64_t>(message_count) * payload_size_bytes) << ",";
  output << "\"write\":{"
         << "\"wall_ms\":" << write_result.wall_ms << ","
         << "\"throughput_mib_s\":" << write_result.throughput_mib_s << ","
         << "\"user_cpu_ms\":" << write_result.user_cpu_ms << ","
         << "\"sys_cpu_ms\":" << write_result.sys_cpu_ms << ","
         << "\"max_rss_kb\":" << write_result.max_rss_kb << ","
         << "\"io_rchar\":" << write_result.io_rchar << ","
         << "\"io_wchar\":" << write_result.io_wchar << ","
         << "\"io_read_bytes\":" << write_result.io_read_bytes << ","
         << "\"io_write_bytes\":" << write_result.io_write_bytes << "},";
  output << "\"read\":{"
         << "\"wall_ms\":" << read_result.wall_ms << ","
         << "\"throughput_mib_s\":" << read_result.throughput_mib_s << ","
         << "\"user_cpu_ms\":" << read_result.user_cpu_ms << ","
         << "\"sys_cpu_ms\":" << read_result.sys_cpu_ms << ","
         << "\"max_rss_kb\":" << read_result.max_rss_kb << ","
         << "\"io_rchar\":" << read_result.io_rchar << ","
         << "\"io_wchar\":" << read_result.io_wchar << ","
         << "\"io_read_bytes\":" << read_result.io_read_bytes << ","
         << "\"io_write_bytes\":" << read_result.io_write_bytes << "}";
  output << "}";
  return output.str();
}

}  // namespace record
}  // namespace cyber
}  // namespace apollo

int main(int argc, char** argv) {
  int message_count = 20000;
  int payload_size_bytes = 4096;
  int chunk_raw_size_bytes = 4 * 1024 * 1024;
  if (argc > 1) {
    message_count = std::max(1, std::atoi(argv[1]));
  }
  if (argc > 2) {
    payload_size_bytes = std::max(64, std::atoi(argv[2]));
  }
  if (argc > 3) {
    chunk_raw_size_bytes = std::max(64 * 1024, std::atoi(argv[3]));
  }

  const std::string path = "record_io_perf.record";
  apollo::cyber::record::PerfResult write_result;
  apollo::cyber::record::PerfResult read_result;
  if (!apollo::cyber::record::WriteRecordFile(path, message_count,
                                               payload_size_bytes,
                                               chunk_raw_size_bytes,
                                               &write_result)) {
    return 1;
  }
  if (!apollo::cyber::record::ReadRecordFile(path, &read_result)) {
    return 1;
  }
  std::cout << apollo::cyber::record::ToJson(write_result, read_result,
                                             message_count, payload_size_bytes,
                                             chunk_raw_size_bytes)
            << "\n";
  apollo::cyber::common::Remove(path);
  return 0;
}
