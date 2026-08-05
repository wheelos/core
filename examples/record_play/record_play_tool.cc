// Copyright 2026 WheelOS. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <iostream>
#include <string>

#include "cyber/cyber.h"
#include "examples/record_play/record_play_tool.h"

namespace apollo {
namespace cyber {
namespace examples {
namespace record_play {

struct RecordPlayToolOptions {
  std::string mode = "convert";
  std::string source = kDefaultRecordPath;
  std::string output = "/tmp/record_play_pod.record";
  std::string manifest = "/tmp/record_play_pod.manifest.tsv";
  std::string dump_dir;
  std::size_t max_per_channel = 64;
};

RecordPlayToolOptions ParseOptions(int argc, char** argv) {
  RecordPlayToolOptions options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const auto eq = arg.find('=');
    const auto key = arg.substr(0, eq);
    const auto value = eq == std::string::npos ? std::string() : arg.substr(eq + 1);
    if (key == "--mode" && !value.empty()) {
      options.mode = value;
    } else if (key == "--source" && !value.empty()) {
      options.source = value;
    } else if (key == "--output" && !value.empty()) {
      options.output = value;
    } else if (key == "--manifest" && !value.empty()) {
      options.manifest = value;
    } else if (key == "--dump_dir" && !value.empty()) {
      options.dump_dir = value;
    } else if (key == "--max_per_channel" && !value.empty()) {
      options.max_per_channel = static_cast<std::size_t>(std::stoul(value));
    }
  }
  return options;
}

int RunConvert(const RecordPlayToolOptions& options) {
  ConvertedRecordResult result;
  if (!ConvertRecordToPod(options.source, options.output, options.manifest,
                          options.max_per_channel, &result)) {
    return 1;
  }
  std::cout << "converted " << result.messages << " messages across "
            << result.channels << " channels to " << options.output
            << std::endl;
  return 0;
}

int RunBenchmark(const RecordPlayToolOptions& options) {
  const auto protobuf_stats =
      BenchmarkProtobufDecode(options.source, options.max_per_channel);
  if (protobuf_stats.messages == 0) {
    std::cerr << "protobuf benchmark failed" << std::endl;
    return 1;
  }

  std::string converted = options.output;
  ConvertedRecordResult result;
  if (!ConvertRecordToPod(options.source, converted, options.manifest,
                          options.max_per_channel, &result)) {
    return 1;
  }

  const auto pod_stats = BenchmarkPodBorrow(converted);
  if (pod_stats.messages == 0) {
    std::cerr << "pod benchmark failed" << std::endl;
    return 1;
  }

  std::cout << "protobuf parse: "
            << protobuf_stats.throughput_mb_s << " MB/s, "
            << protobuf_stats.throughput_msg_s << " msg/s\n";
  std::cout << "pod borrow: " << pod_stats.throughput_mb_s << " MB/s, "
            << pod_stats.throughput_msg_s << " msg/s\n";
  std::cout << "speedup_mb=" << (pod_stats.throughput_mb_s /
                                 std::max(0.001, protobuf_stats.throughput_mb_s))
            << " speedup_msg="
            << (pod_stats.throughput_msg_s /
                std::max(0.001, protobuf_stats.throughput_msg_s))
            << std::endl;
  std::cout << "baseline\tprotobuf_messages=" << protobuf_stats.messages
            << "\tprotobuf_bytes=" << protobuf_stats.bytes
            << "\tprotobuf_MBps=" << protobuf_stats.throughput_mb_s
            << "\tprotobuf_msgps=" << protobuf_stats.throughput_msg_s
            << "\tpod_messages=" << pod_stats.messages
            << "\tpod_bytes=" << pod_stats.bytes
            << "\tpod_MBps=" << pod_stats.throughput_mb_s
            << "\tpod_msgps=" << pod_stats.throughput_msg_s
            << "\tspeedup_MBps="
            << (pod_stats.throughput_mb_s /
                std::max(0.001, protobuf_stats.throughput_mb_s))
            << "\tspeedup_msgps="
            << (pod_stats.throughput_msg_s /
                std::max(0.001, protobuf_stats.throughput_msg_s))
            << std::endl;
  return 0;
}

int RunDump(const RecordPlayToolOptions& options) {
  return DumpConvertedRecord(options.source, options.dump_dir,
                             options.max_per_channel)
             ? 0
             : 1;
}
}  // namespace record_play
}  // namespace examples
}  // namespace cyber
}  // namespace apollo

int main(int argc, char** argv) {
  apollo::cyber::Init(argv[0]);
  const auto options =
      apollo::cyber::examples::record_play::ParseOptions(argc, argv);

  int ret = 0;
  if (options.mode == "convert") {
    ret = apollo::cyber::examples::record_play::RunConvert(options);
  } else if (options.mode == "benchmark") {
    ret = apollo::cyber::examples::record_play::RunBenchmark(options);
  } else if (options.mode == "dump") {
    ret = apollo::cyber::examples::record_play::RunDump(options);
  } else {
    std::cerr << "unknown mode: " << options.mode << std::endl;
    ret = 1;
  }

  apollo::cyber::Clear();
  return ret;
}
