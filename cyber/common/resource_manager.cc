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

#include "cyber/common/resource_manager.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <system_error>

#include "cyber/common/log.h"

namespace apollo {
namespace cyber {
namespace common {

namespace fs = std::filesystem;

namespace {

struct ConfigRootState {
  std::mutex mutex;
  bool initialized = false;
  bool configured = false;
  fs::path root;
};

ConfigRootState& GetConfigRootState() {
  static ConfigRootState state;
  return state;
}

bool IsAncestorOrSamePath(const fs::path& ancestor, const fs::path& child) {
  auto ancestor_it = ancestor.begin();
  auto child_it = child.begin();
  for (; ancestor_it != ancestor.end() && child_it != child.end();
       ++ancestor_it, ++child_it) {
    if (*ancestor_it != *child_it) {
      return false;
    }
  }
  return ancestor_it == ancestor.end();
}

bool IsSafeRelativeKey(const fs::path& key) {
  if (key.empty() || key.is_absolute()) {
    return false;
  }
  for (const fs::path& part : key) {
    if (part == "." || part == "..") {
      return false;
    }
  }
  return true;
}

bool HasParentTraversal(const fs::path& path) {
  for (const fs::path& part : path) {
    if (part == "..") {
      return true;
    }
  }
  return false;
}

bool IsReadableResource(const fs::path& path, std::error_code* ec) {
  const fs::file_status status = fs::status(path, *ec);
  if (*ec) {
    return false;
  }
  if (fs::is_regular_file(status)) {
    std::ifstream file(path, std::ios::in | std::ios::binary);
    return file.good();
  }
  if (fs::is_directory(status)) {
    fs::directory_iterator iterator(path, *ec);
    static_cast<void>(iterator);
    return !*ec;
  }
  return false;
}

bool IsReadableConfigFile(const fs::path& path, fs::path* canonical_path) {
  std::error_code ec;
  *canonical_path = fs::canonical(path, ec);
  return !ec && IsReadableResource(*canonical_path, &ec) &&
         fs::is_regular_file(*canonical_path, ec) && !ec;
}

bool GetConfigKey(const fs::path& source_path, const fs::path& software_root,
                  fs::path* config_key) {
  if (source_path.empty() || config_key == nullptr) {
    return false;
  }
  if (source_path.is_relative()) {
    *config_key = source_path;
    return true;
  }

  const fs::path normalized_source = source_path.lexically_normal();
  for (const fs::path& root : {software_root, fs::path("/apollo")}) {
    if (IsAncestorOrSamePath(root, normalized_source) &&
        normalized_source != root) {
      *config_key = normalized_source.lexically_relative(root);
      return true;
    }
  }
  return false;
}

fs::path SoftwareRoot() {
  const char* root_value = std::getenv("APOLLO_ROOT_DIR");
  if (root_value != nullptr && *root_value != '\0') {
    return fs::path(root_value);
  }
  return fs::path("/apollo");
}

bool ResolveSoftwareRoot(fs::path* software_root) {
  const fs::path configured_root = SoftwareRoot();
  if (!configured_root.is_absolute()) {
    AERROR << "APOLLO_ROOT_DIR must be an absolute directory: "
           << configured_root;
    return false;
  }

  std::error_code ec;
  *software_root = fs::canonical(configured_root, ec);
  if (ec || !fs::is_directory(*software_root, ec) || ec) {
    AERROR << "Software root is not an accessible directory: "
           << configured_root;
    return false;
  }
  return true;
}

bool ResolveAssetRoot(fs::path* asset_root) {
  const char* configured_root = std::getenv("WHEELOS_ASSET_ROOT");
  if (configured_root != nullptr) {
    const fs::path root(configured_root);
    if (configured_root[0] == '\0' || !root.is_absolute()) {
      AERROR << "WHEELOS_ASSET_ROOT must be a non-empty absolute directory: "
             << configured_root;
      return false;
    }
    std::error_code ec;
    *asset_root = fs::canonical(root, ec);
    if (ec || !fs::is_directory(*asset_root, ec) || ec) {
      AERROR << "WHEELOS_ASSET_ROOT is not an accessible directory: " << root;
      return false;
    }
    return true;
  }

  const fs::path default_root = SoftwareRoot() / "assets";
  if (!default_root.is_absolute()) {
    AERROR << "APOLLO_ROOT_DIR must be absolute to resolve the default asset "
              "root: "
           << default_root;
    return false;
  }
  std::error_code ec;
  *asset_root = fs::canonical(default_root, ec);
  if (ec || !fs::is_directory(*asset_root, ec) || ec) {
    AERROR << "WHEELOS_ASSET_ROOT is unset and the default asset root is "
              "unavailable: "
           << default_root;
    return false;
  }
  return true;
}

bool ResolveConfigRoot(const char* configured_root, fs::path* config_root,
                       bool* is_configured) {
  *is_configured = configured_root != nullptr;
  if (!*is_configured) {
    return true;
  }
  const fs::path root(configured_root);
  if (configured_root[0] == '\0' || !root.is_absolute()) {
    AERROR << "WHEELOS_CONFIG_ROOT must be a non-empty absolute directory: "
           << configured_root;
    return false;
  }

  std::error_code ec;
  *config_root = fs::canonical(root, ec);
  if (ec || !fs::is_directory(*config_root, ec) || ec) {
    AERROR << "WHEELOS_CONFIG_ROOT is not an accessible directory: " << root;
    return false;
  }
  return true;
}

bool GetConfigRoot(fs::path* config_root, bool* is_configured) {
  ConfigRootState& state = GetConfigRootState();
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.initialized) {
      if (state.configured) {
        std::error_code ec;
        if (!IsReadableResource(state.root, &ec) ||
            !fs::is_directory(state.root, ec) || ec) {
          AERROR << "Initialized WHEELOS_CONFIG_ROOT is no longer accessible: "
                 << state.root;
          return false;
        }
      }
      *config_root = state.root;
      *is_configured = state.configured;
      return true;
    }
  }
  if (!ResolveConfigRoot(std::getenv("WHEELOS_CONFIG_ROOT"), config_root,
                         is_configured)) {
    return false;
  }
  return true;
}

}  // namespace

bool ResourceManager::InitializeConfigRoot() {
  ConfigRootState& state = GetConfigRootState();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (state.initialized) {
    return true;
  }

  fs::path config_root;
  bool is_configured = false;
  if (!ResolveConfigRoot(std::getenv("WHEELOS_CONFIG_ROOT"), &config_root,
                         &is_configured)) {
    return false;
  }
  state.root = std::move(config_root);
  state.configured = is_configured;
  state.initialized = true;
  return true;
}

bool ResourceManager::ResolveConfigPath(const std::string& default_path,
                                        std::string* selected_path) {
  if (selected_path == nullptr) {
    AERROR << "Cannot resolve config path: output is null.";
    return false;
  }
  selected_path->clear();
  if (default_path.empty()) {
    AERROR << "Cannot resolve config path: default path is empty.";
    return false;
  }
  fs::path config_root;
  bool has_config_root = false;
  if (!GetConfigRoot(&config_root, &has_config_root)) {
    return false;
  }
  const fs::path source_path(default_path);
  if (HasParentTraversal(source_path)) {
    AERROR << "Config path must not contain parent traversal: " << source_path;
    return false;
  }

  fs::path software_root;
  if (!ResolveSoftwareRoot(&software_root)) {
    return false;
  }
  fs::path config_key;
  if (!GetConfigKey(source_path, software_root, &config_key)) {
    AERROR << "Absolute config path is outside the software root: "
           << source_path;
    return false;
  }
  if (!IsSafeRelativeKey(config_key)) {
    AERROR << "Config key must be relative and must not escape its root: "
           << config_key;
    return false;
  }

  const fs::path resolved_default = software_root / config_key;
  if (!has_config_root) {
    fs::path canonical_default;
    if (!IsReadableConfigFile(resolved_default, &canonical_default) ||
        !IsAncestorOrSamePath(software_root, canonical_default)) {
      AERROR << "Default config is not a readable regular file within the "
                "software root: "
             << resolved_default;
      return false;
    }
    *selected_path = canonical_default.string();
    AINFO << "[CONFIG] " << config_key << ": default " << *selected_path;
    return true;
  }

  const fs::path override_path = config_root / config_key;
  std::error_code ec;
  bool override_missing = false;
  fs::path current_path = config_root;
  for (const fs::path& part : config_key) {
    current_path /= part;
    const fs::file_status status = fs::symlink_status(current_path, ec);
    if (ec == std::errc::no_such_file_or_directory ||
        (!ec && status.type() == fs::file_type::not_found)) {
      ec.clear();
      override_missing = true;
      break;
    }
    if (ec) {
      AERROR << "Cannot inspect config override " << current_path << ": "
             << ec.message();
      return false;
    }
    if (fs::is_symlink(status)) {
      const fs::path canonical_prefix = fs::canonical(current_path, ec);
      if (ec || !IsAncestorOrSamePath(config_root, canonical_prefix)) {
        AERROR << "Config override contains a broken or escaping symlink: "
               << current_path;
        return false;
      }
    }
  }

  if (!override_missing) {
    fs::path canonical_override;
    if (!IsReadableConfigFile(override_path, &canonical_override) ||
        !IsAncestorOrSamePath(config_root, canonical_override)) {
      AERROR << "Config override is not a readable regular file within "
                "WHEELOS_CONFIG_ROOT: "
             << override_path;
      return false;
    }
    *selected_path = canonical_override.string();
    AINFO << "[CONFIG] " << config_key << ": override " << *selected_path;
    return true;
  }

  fs::path canonical_default;
  if (!IsReadableConfigFile(resolved_default, &canonical_default) ||
      !IsAncestorOrSamePath(software_root, canonical_default)) {
    AERROR << "Neither a usable override nor a readable default config exists "
              "for "
           << config_key << "; default path: " << resolved_default;
    return false;
  }
  *selected_path = canonical_default.string();
  AINFO << "[CONFIG] " << config_key << ": default " << *selected_path;
  return true;
}

bool ResourceManager::ResolveAssetPath(const std::string& asset_key,
                                       std::string* resolved_path) {
  if (resolved_path == nullptr) {
    AERROR << "Cannot resolve asset path: output is null.";
    return false;
  }
  resolved_path->clear();

  const fs::path key(asset_key);
  if (!IsSafeRelativeKey(key)) {
    AERROR << "Asset key must be a non-empty relative path without '..': "
           << asset_key;
    return false;
  }

  fs::path asset_root;
  if (!ResolveAssetRoot(&asset_root)) {
    return false;
  }

  std::error_code ec;
  const fs::path candidate = asset_root / key;
  const fs::path canonical_path = fs::canonical(candidate, ec);
  if (ec || !IsAncestorOrSamePath(asset_root, canonical_path)) {
    AERROR << "Asset does not exist or escapes WHEELOS_ASSET_ROOT: "
           << candidate;
    return false;
  }
  if (!IsReadableResource(canonical_path, &ec)) {
    AERROR << "Asset is not a readable file or directory: " << canonical_path;
    return false;
  }

  *resolved_path = canonical_path.string();
  AINFO << "[ASSET] " << asset_key << ": " << *resolved_path;
  return true;
}

}  // namespace common
}  // namespace cyber
}  // namespace apollo
