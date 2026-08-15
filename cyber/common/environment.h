/******************************************************************************
 * Copyright 2018 The Apollo Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/

#ifndef CYBER_COMMON_ENVIRONMENT_H_
#define CYBER_COMMON_ENVIRONMENT_H_

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "cyber/common/log.h"

namespace apollo {
namespace cyber {
namespace common {

inline std::string GetEnv(const std::string& var_name,
                          const std::string& default_value = "") {
  const char* var = std::getenv(var_name.c_str());
  if (var == nullptr) {
    AWARN << "Environment variable [" << var_name << "] not set, fallback to "
          << default_value;
    return default_value;
  }
  return std::string(var);
}

inline const std::string WorkRoot() {
  std::string work_root = GetEnv("CYBER_PATH");
  if (work_root.empty()) {
    const char* runfiles_dir = std::getenv("RUNFILES_DIR");
    if (runfiles_dir != nullptr) {
      const std::filesystem::path runfiles_root(runfiles_dir);
      const auto config_relative_path =
          std::filesystem::path("cyber") / "conf" / "cyber.pb.conf";
      if (std::filesystem::is_directory(runfiles_root)) {
        const auto direct_config = runfiles_root / config_relative_path;
        if (std::filesystem::exists(direct_config)) {
          work_root = (runfiles_root / "cyber").string();
        } else {
          for (const auto& entry :
               std::filesystem::directory_iterator(runfiles_root)) {
            const auto candidate = entry.path() / config_relative_path;
            if (std::filesystem::exists(candidate)) {
              work_root = (entry.path() / "cyber").string();
              break;
            }
          }
        }
      }
    }
  }
  if (work_root.empty()) {
    const char* manifest_file = std::getenv("RUNFILES_MANIFEST_FILE");
    if (manifest_file != nullptr) {
      std::ifstream manifest(manifest_file);
      std::string logical_path;
      std::string resolved_path;
      while (manifest >> logical_path >> resolved_path) {
        const std::string suffix = "cyber/conf/cyber.pb.conf";
        if (logical_path.size() >= suffix.size() &&
            logical_path.compare(logical_path.size() - suffix.size(),
                                 suffix.size(), suffix) == 0) {
          work_root =
              std::filesystem::path(resolved_path).parent_path().parent_path();
          break;
        }
      }
    }
  }
  if (work_root.empty()) {
    const std::filesystem::path cwd = std::filesystem::current_path();
    for (auto path = cwd; !path.empty();) {
      const auto candidate = path / "cyber" / "conf" / "cyber.pb.conf";
      if (std::filesystem::exists(candidate)) {
        work_root = (path / "cyber").string();
        break;
      }
      const auto parent = path.parent_path();
      if (parent == path) {
        break;
      }
      path = parent;
    }
    if (work_root.empty()) {
      work_root = "/apollo/cyber";
    }
  }
  return work_root;
}

}  // namespace common
}  // namespace cyber
}  // namespace apollo

#endif  // CYBER_COMMON_ENVIRONMENT_H_
