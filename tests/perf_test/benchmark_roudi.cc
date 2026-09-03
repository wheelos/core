// Copyright 2026 WheelOS. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <utility>

#include "iceoryx_posh/iceoryx_posh_config.hpp"
#include "iceoryx_posh/internal/roudi/roudi.hpp"
#include "iceoryx_posh/roudi/iceoryx_roudi_components.hpp"
#include "iox/signal_watcher.hpp"

namespace {

bool ParseArgs(int argc, char** argv,
               std::unordered_map<std::string, std::string>* args) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg(argv[i]);
    const auto separator = arg.find('=');
    if (separator == std::string::npos) {
      return false;
    }
    (*args)[arg.substr(0, separator)] = arg.substr(separator + 1);
  }
  return true;
}

uint64_t ParseUint64(const std::unordered_map<std::string, std::string>& args,
                     const std::string& key, uint64_t fallback) {
  const auto it = args.find(key);
  if (it == args.end() || it->second.empty()) {
    return fallback;
  }
  errno = 0;
  char* end = nullptr;
  const auto value = std::strtoull(it->second.c_str(), &end, 10);
  if (errno != 0 || end == it->second.c_str() || *end != '\0') {
    return fallback;
  }
  return value;
}

std::string GetArg(const std::unordered_map<std::string, std::string>& args,
                   const std::string& key) {
  const auto it = args.find(key);
  return it == args.end() ? std::string() : it->second;
}

bool WriteResult(const std::string& path,
                 const std::initializer_list<
                     std::pair<std::string, std::string>>& values) {
  std::ofstream output(path, std::ios::out | std::ios::trunc);
  if (!output.is_open()) {
    return false;
  }
  for (const auto& value : values) {
    output << value.first << "=" << value.second << "\n";
  }
  output.flush();
  return !output.fail();
}

}  // namespace

int main(int argc, char** argv) {
  std::unordered_map<std::string, std::string> args;
  if (!ParseArgs(argc, argv, &args)) {
    return 2;
  }
  const std::string ready_path = GetArg(args, "--ready_path");
  const std::string result_path = GetArg(args, "--result_path");
  const uint64_t chunk_size =
      ParseUint64(args, "--chunk_size", 8ULL * 1024ULL * 1024ULL);
  const uint64_t chunk_count = ParseUint64(args, "--chunk_count", 16);
  if (ready_path.empty() || result_path.empty() || chunk_size == 0 ||
      chunk_count == 0 || chunk_count > UINT32_MAX) {
    return 2;
  }

  {
    iox::IceoryxConfig config = iox::IceoryxConfig().setDefaults();
    config.sharesAddressSpaceWithApplications = false;
    for (auto& segment : config.m_sharedMemorySegments) {
      segment.m_mempoolConfig.addMemPool(
          {chunk_size, static_cast<uint32_t>(chunk_count)});
    }

    auto& signal_watcher = iox::SignalWatcher::getInstance();
    iox::roudi::IceOryxRouDiComponents components(config);
    iox::roudi::RouDi roudi(components.rouDiMemoryManager,
                            components.portManager, config);
    if (!WriteResult(ready_path,
                     {{"ready", "1"},
                      {"chunk_size", std::to_string(chunk_size)},
                      {"chunk_count", std::to_string(chunk_count)}})) {
      return 1;
    }
    signal_watcher.waitForSignal();
  }

  return WriteResult(result_path,
                     {{"shutdown_complete", "1"},
                      {"chunk_size", std::to_string(chunk_size)},
                      {"chunk_count", std::to_string(chunk_count)}})
             ? 0
             : 1;
}
