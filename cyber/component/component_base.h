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

#ifndef CYBER_COMPONENT_COMPONENT_BASE_H_
#define CYBER_COMPONENT_COMPONENT_BASE_H_

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "gflags/gflags.h"

#include "cyber/proto/component_conf.pb.h"

#include "cyber/class_loader/class_loader.h"
#include "cyber/common/environment.h"
#include "cyber/common/file.h"
#include "cyber/common/log.h"
#include "cyber/common/resource_manager.h"
#include "cyber/metrics/metrics.h"
#include "cyber/node/node.h"
#include "cyber/scheduler/scheduler.h"

namespace apollo {
namespace cyber {

using apollo::cyber::proto::ComponentConfig;
using apollo::cyber::proto::TimerComponentConfig;

class ComponentBase : public std::enable_shared_from_this<ComponentBase> {
 public:
  template <typename M>
  using Reader = cyber::Reader<M>;

  virtual ~ComponentBase() {}

  virtual bool Initialize(const ComponentConfig& config) { return false; }
  virtual bool Initialize(const TimerComponentConfig& config) { return false; }
  virtual void Shutdown() {
    if (is_shutdown_.exchange(true)) {
      return;
    }

    Clear();
    for (auto& reader : readers_) {
      reader->Shutdown();
    }
    scheduler::Instance()->RemoveTask(node_->Name());
  }

  template <typename T>
  bool GetProtoConfig(T* config) const {
    return common::GetProtoFromFile(config_file_path_, config);
  }

  template <typename T>
  bool GetProtoConfigFromPath(const std::string& input_path,
                              T* config) const {
    std::string resolved_path;
    if (!ResolveComponentConfigPath(input_path, &resolved_path)) {
      return false;
    }
    return common::GetProtoFromFile(resolved_path, config);
  }

 protected:
  virtual bool Init() = 0;
  virtual void Clear() { return; }
  const std::string& ConfigFilePath() const { return config_file_path_; }

  bool LoadConfigFiles(const ComponentConfig& config) {
    return LoadConfigFiles(config.config_file_path(), config.flag_file_path());
  }

  bool LoadConfigFiles(const TimerComponentConfig& config) {
    return LoadConfigFiles(config.config_file_path(), config.flag_file_path());
  }

  bool ResolveComponentConfigPath(const std::string& input_path,
                                  std::string* resolved_path) const {
    if (input_path.empty()) {
      return true;
    }

    const std::string relative_asset_prefix = "assets/";
    const std::string absolute_asset_prefix = "/apollo/assets/";
    if (input_path.rfind(relative_asset_prefix, 0) == 0) {
      return common::ResourceManager::ResolveAssetPath(
          input_path.substr(relative_asset_prefix.size()), resolved_path);
    }
    if (input_path.rfind(absolute_asset_prefix, 0) == 0) {
      return common::ResourceManager::ResolveAssetPath(
          input_path.substr(absolute_asset_prefix.size()), resolved_path);
    }

    if (!common::ResourceManager::InitializeConfigRoot()) {
      AERROR << "Failed to initialize config root.";
      return false;
    }

    std::string source_path = input_path;
    if (source_path[0] != '/') {
      if (source_path.rfind("modules/", 0) == 0 ||
          source_path.rfind("cyber/", 0) == 0) {
        return common::ResourceManager::ResolveConfigPath(source_path,
                                                          resolved_path);
      } else {
        source_path = common::GetAbsolutePath(common::WorkRoot(), source_path);
      }
    }

    return common::ResourceManager::ResolveConfigPath(source_path,
                                                      resolved_path);
  }

  bool LoadConfigFiles(const std::string& config_path,
                       const std::string& flag_file_path) {
    if (!config_path.empty() &&
        !ResolveComponentConfigPath(config_path, &config_file_path_)) {
      return false;
    }
    if (!flag_file_path.empty()) {
      std::string resolved_flag_file_path;
      if (!ResolveComponentConfigPath(flag_file_path,
                                      &resolved_flag_file_path)) {
        return false;
      }
      if (google::SetCommandLineOption("flagfile",
                                       resolved_flag_file_path.c_str())
              .empty()) {
        AERROR << "Failed to set component flagfile: "
               << resolved_flag_file_path;
        return false;
      }
    }
    return true;
  }

  void RegisterComponentMetric(const ComponentConfig& config) {
    metric_ = metrics::Registry::Instance().RegisterConsumer(
        config.readers(0).channel(), config.name() + ":component");
  }

  std::atomic<bool> is_shutdown_ = {false};
  std::shared_ptr<Node> node_ = nullptr;
  std::string config_file_path_ = "";
  std::vector<std::shared_ptr<ReaderBase>> readers_;
  std::shared_ptr<metrics::Endpoint> metric_;
};

}  // namespace cyber
}  // namespace apollo

#endif  // CYBER_COMPONENT_COMPONENT_BASE_H_
