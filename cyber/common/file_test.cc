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

//  Created Date: 2025-10-25
//  Author: daohu527 <daohu527@gmail.com>

#include "cyber/common/file.h"
#include "cyber/common/resource_manager.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "cyber/proto/unit_test.pb.h"

namespace apollo {
namespace cyber {
namespace common {

namespace fs = std::filesystem;
using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::Not;
using ::testing::UnorderedElementsAre;

class FileTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Create unique, isolated sandbox directories for each test
    const auto* test_info =
        ::testing::UnitTest::GetInstance()->current_test_info();
    test_root_ =
        fs::temp_directory_path() / "file_test_root" / test_info->name();
    fs::remove_all(test_root_);
    fs::create_directories(test_root_);
    SaveEnvironmentVariable("WHEELOS_CONFIG_ROOT",
                            &original_config_root_is_set_,
                            &original_config_root_);
    SaveEnvironmentVariable("APOLLO_ROOT_DIR", &original_apollo_root_is_set_,
                            &original_apollo_root_);
    SaveEnvironmentVariable("WHEELOS_ASSET_ROOT",
                            &original_asset_root_is_set_, &original_asset_root_);
    unsetenv("WHEELOS_CONFIG_ROOT");
    unsetenv("APOLLO_ROOT_DIR");
    unsetenv("WHEELOS_ASSET_ROOT");
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(test_root_, ec);
    RestoreEnvironmentVariable("WHEELOS_CONFIG_ROOT",
                               original_config_root_is_set_,
                               original_config_root_);
    RestoreEnvironmentVariable("APOLLO_ROOT_DIR",
                               original_apollo_root_is_set_,
                               original_apollo_root_);
    RestoreEnvironmentVariable("WHEELOS_ASSET_ROOT",
                               original_asset_root_is_set_,
                               original_asset_root_);
    ASSERT_FALSE(ec) << "TearDown failed to clean up: " << test_root_.string();
  }

  // Helper to get full path within the test sandbox
  fs::path GetTestPath(const std::string& relative_path) const {
    return test_root_ / relative_path;
  }

  static void SaveEnvironmentVariable(const char* name, bool* was_set,
                                     std::string* value) {
    const char* original = std::getenv(name);
    *was_set = original != nullptr;
    *value = original == nullptr ? "" : original;
  }

  static void RestoreEnvironmentVariable(const char* name, bool was_set,
                                        const std::string& value) {
    if (was_set) {
      setenv(name, value.c_str(), 1);
    } else {
      unsetenv(name);
    }
  }

  fs::path test_root_;
  bool original_config_root_is_set_ = false;
  std::string original_config_root_;
  bool original_apollo_root_is_set_ = false;
  std::string original_apollo_root_;
  bool original_asset_root_is_set_ = false;
  std::string original_asset_root_;
};

TEST_F(FileTest, ProtoIOCombined) {
  proto::UnitTest message;
  message.set_class_name("ProtoTest");
  const auto ascii_path = GetTestPath("message.ascii");
  const auto bin_path = GetTestPath("message.bin");
  const auto json_path = GetTestPath("message.json");

  // Testing ASCII reading and writing
  ASSERT_TRUE(SetProtoToASCIIFile(message, ascii_path.string()));
  proto::UnitTest read_ascii;
  ASSERT_TRUE(GetProtoFromASCIIFile(ascii_path.string(), &read_ascii));
  EXPECT_EQ(read_ascii.class_name(), "ProtoTest");

  // Testing Binary reading and writing
  ASSERT_TRUE(SetProtoToBinaryFile(message, bin_path.string()));
  proto::UnitTest read_bin;
  ASSERT_TRUE(GetProtoFromBinaryFile(bin_path.string(), &read_bin));
  EXPECT_EQ(read_bin.class_name(), "ProtoTest");

  // Testing JSON reading and writing
  {
    std::ofstream ofs(json_path);
    ofs << R"({"className": "JsonTest"})";
  }
  proto::UnitTest read_json;
  ASSERT_TRUE(GetProtoFromJsonFile(json_path.string(), &read_json));
  EXPECT_EQ(read_json.class_name(), "JsonTest");

  // Testing malformed JSON handling
  {
    std::ofstream ofs(json_path);
    ofs << R"({"className": )";
  }
  EXPECT_FALSE(GetProtoFromJsonFile(json_path.string(), &read_json));
}

TEST_F(FileTest, ResolveConfigPathUsesWholeFileOverride) {
  const fs::path software_root = GetTestPath("software");
  const fs::path config_root = GetTestPath("assets/vehicles/cargo/config");
  const fs::path default_path =
      software_root / "modules/canbus/conf/canbus_conf.pb.txt";
  const fs::path override_path =
      config_root / "modules/canbus/conf/canbus_conf.pb.txt";
  fs::create_directories(default_path.parent_path());
  fs::create_directories(override_path.parent_path());
  {
    std::ofstream default_file(default_path);
    default_file << "default";
    std::ofstream override_file(override_path);
    override_file << "override";
  }

  setenv("APOLLO_ROOT_DIR", software_root.c_str(), 1);
  setenv("WHEELOS_CONFIG_ROOT", config_root.c_str(), 1);

  std::string selected_path;
  ASSERT_TRUE(ResourceManager::ResolveConfigPath(default_path.string(),
                                                 &selected_path));
  EXPECT_EQ(fs::canonical(override_path).string(), selected_path);
}

TEST_F(FileTest, ResolveConfigPathFallsBackToSoftwareDefault) {
  const fs::path software_root = GetTestPath("software");
  const fs::path config_root = GetTestPath("assets/vehicles/cargo/config");
  const fs::path default_path =
      software_root / "modules/canbus/conf/canbus_conf.pb.txt";
  fs::create_directories(default_path.parent_path());
  fs::create_directories(config_root);
  {
    std::ofstream default_file(default_path);
    default_file << "default";
  }

  setenv("APOLLO_ROOT_DIR", software_root.c_str(), 1);
  setenv("WHEELOS_CONFIG_ROOT", config_root.c_str(), 1);

  std::string selected_path;
  ASSERT_TRUE(ResourceManager::ResolveConfigPath(default_path.string(),
                                                 &selected_path));
  EXPECT_EQ(fs::canonical(default_path).string(), selected_path);
}

TEST_F(FileTest, ResolveAbsoluteApolloPathAgainstConfiguredSoftwareRoot) {
  const fs::path software_root = GetTestPath("software");
  const fs::path config_root = GetTestPath("assets/vehicles/cargo/config");
  const fs::path default_path =
      software_root / "modules/canbus/conf/canbus_conf.pb.txt";
  fs::create_directories(default_path.parent_path());
  fs::create_directories(config_root);
  {
    std::ofstream default_file(default_path);
    default_file << "default";
  }

  setenv("APOLLO_ROOT_DIR", software_root.c_str(), 1);
  setenv("WHEELOS_CONFIG_ROOT", config_root.c_str(), 1);

  std::string selected_path;
  ASSERT_TRUE(ResourceManager::ResolveConfigPath(
      "/apollo/modules/canbus/conf/canbus_conf.pb.txt", &selected_path));
  EXPECT_EQ(fs::canonical(default_path).string(), selected_path);
}

TEST_F(FileTest, ResolveConfigPathRejectsInvalidRootAndTraversal) {
  const fs::path software_root = GetTestPath("software");
  const fs::path default_path =
      software_root / "modules/canbus/conf/canbus_conf.pb.txt";
  const fs::path traversal_target = software_root / "outside.conf";
  fs::create_directories(default_path.parent_path());
  {
    std::ofstream default_file(default_path);
    default_file << "default";
    std::ofstream outside_file(traversal_target);
    outside_file << "outside";
  }
  setenv("APOLLO_ROOT_DIR", software_root.c_str(), 1);

  std::string selected_path;
  setenv("WHEELOS_CONFIG_ROOT", GetTestPath("missing-root").c_str(), 1);
  EXPECT_FALSE(ResourceManager::ResolveConfigPath(default_path.string(),
                                                  &selected_path));

  setenv("WHEELOS_CONFIG_ROOT", GetTestPath("assets").c_str(), 1);
  EXPECT_FALSE(
      ResourceManager::ResolveConfigPath("modules/../outside.conf",
                                         &selected_path));
  EXPECT_FALSE(ResourceManager::ResolveConfigPath(
      (software_root / "modules/../outside.conf").string(), &selected_path));
  EXPECT_FALSE(ResourceManager::ResolveConfigPath(
      (GetTestPath("outside.conf")).string(), &selected_path));
}

TEST_F(FileTest, ResolveConfigPathFailsWhenBothFilesAreMissing) {
  const fs::path software_root = GetTestPath("software");
  const fs::path config_root = GetTestPath("assets/vehicles/cargo/config");
  fs::create_directories(software_root);
  fs::create_directories(config_root);
  setenv("APOLLO_ROOT_DIR", software_root.c_str(), 1);
  setenv("WHEELOS_CONFIG_ROOT", config_root.c_str(), 1);

  std::string selected_path;
  EXPECT_FALSE(ResourceManager::ResolveConfigPath(
      (software_root / "modules/canbus/conf/missing.pb.txt").string(),
      &selected_path));
}

TEST_F(FileTest, ResolveConfigPathRejectsOverrideSymlinkEscape) {
  const fs::path software_root = GetTestPath("software");
  const fs::path config_root = GetTestPath("assets/vehicles/cargo/config");
  const fs::path default_path =
      software_root / "modules/canbus/conf/canbus_conf.pb.txt";
  const fs::path outside_path = GetTestPath("outside.conf");
  const fs::path override_path =
      config_root / "modules/canbus/conf/canbus_conf.pb.txt";
  fs::create_directories(default_path.parent_path());
  fs::create_directories(override_path.parent_path());
  {
    std::ofstream default_file(default_path);
    default_file << "default";
    std::ofstream outside_file(outside_path);
    outside_file << "outside";
  }
  fs::create_symlink(outside_path, override_path);
  setenv("APOLLO_ROOT_DIR", software_root.c_str(), 1);
  setenv("WHEELOS_CONFIG_ROOT", config_root.c_str(), 1);

  std::string selected_path;
  EXPECT_FALSE(ResourceManager::ResolveConfigPath(default_path.string(),
                                                  &selected_path));
}

TEST_F(FileTest, ResolveConfigPathRejectsBrokenOverrideSymlink) {
  const fs::path software_root = GetTestPath("software");
  const fs::path config_root = GetTestPath("assets/vehicles/cargo/config");
  const fs::path default_path =
      software_root / "modules/canbus/conf/canbus_conf.pb.txt";
  const fs::path override_path =
      config_root / "modules/canbus/conf/canbus_conf.pb.txt";
  fs::create_directories(default_path.parent_path());
  fs::create_directories(override_path.parent_path());
  {
    std::ofstream default_file(default_path);
    default_file << "default";
  }
  fs::create_symlink(GetTestPath("missing.conf"), override_path);
  setenv("APOLLO_ROOT_DIR", software_root.c_str(), 1);
  setenv("WHEELOS_CONFIG_ROOT", config_root.c_str(), 1);

  std::string selected_path;
  EXPECT_FALSE(ResourceManager::ResolveConfigPath(default_path.string(),
                                                  &selected_path));
}

TEST_F(FileTest, ResolveConfigPathRejectsDefaultSymlinkEscape) {
  const fs::path software_root = GetTestPath("software");
  const fs::path config_root = GetTestPath("assets/vehicles/cargo/config");
  const fs::path default_path =
      software_root / "modules/canbus/conf/canbus_conf.pb.txt";
  const fs::path outside_path = GetTestPath("outside.conf");
  fs::create_directories(default_path.parent_path());
  fs::create_directories(config_root);
  {
    std::ofstream outside_file(outside_path);
    outside_file << "outside";
  }
  fs::create_symlink(outside_path, default_path);
  setenv("APOLLO_ROOT_DIR", software_root.c_str(), 1);
  setenv("WHEELOS_CONFIG_ROOT", config_root.c_str(), 1);

  std::string selected_path;
  EXPECT_FALSE(ResourceManager::ResolveConfigPath(default_path.string(),
                                                  &selected_path));
}

TEST_F(FileTest, ResolveRelativeConfigPathUsesSoftwareRootWithoutOverride) {
  const fs::path software_root = GetTestPath("software");
  const fs::path default_path =
      software_root / "modules/canbus/conf/canbus_conf.pb.txt";
  fs::create_directories(default_path.parent_path());
  {
    std::ofstream default_file(default_path);
    default_file << "default";
  }
  setenv("APOLLO_ROOT_DIR", software_root.c_str(), 1);

  std::string selected_path;
  ASSERT_TRUE(ResourceManager::ResolveConfigPath(
      "modules/canbus/conf/canbus_conf.pb.txt", &selected_path));
  EXPECT_EQ((software_root / "modules/canbus/conf/canbus_conf.pb.txt")
                .lexically_normal()
                .string(),
            selected_path);
}

TEST_F(FileTest, ResolveConfigPathFailsWhenDefaultIsMissingWithoutOverride) {
  setenv("APOLLO_ROOT_DIR", GetTestPath("software").c_str(), 1);

  std::string selected_path;
  EXPECT_FALSE(ResourceManager::ResolveConfigPath(
      "modules/canbus/conf/missing.pb.txt", &selected_path));
}

TEST_F(FileTest, ResolveAssetPathUsesConfiguredAssetRoot) {
  const fs::path asset_root = GetTestPath("installed/assets");
  const fs::path map_bundle = asset_root / "sites/map/borregas_ave";
  fs::create_directories(map_bundle);
  {
    std::ofstream map_file(map_bundle / "base_map.bin", std::ios::binary);
    map_file << "map";
  }
  setenv("WHEELOS_ASSET_ROOT", asset_root.c_str(), 1);

  std::string resolved_path;
  ASSERT_TRUE(ResourceManager::ResolveAssetPath("sites/map/borregas_ave",
                                               &resolved_path));
  EXPECT_EQ(fs::canonical(map_bundle).string(), resolved_path);
}

TEST_F(FileTest, ResolveAssetPathAcceptsEmptyDirectory) {
  const fs::path asset_root = GetTestPath("installed/assets");
  const fs::path empty_bundle = asset_root / "runtime/models/empty";
  fs::create_directories(empty_bundle);
  setenv("WHEELOS_ASSET_ROOT", asset_root.c_str(), 1);

  std::string resolved_path;
  ASSERT_TRUE(
      ResourceManager::ResolveAssetPath("runtime/models/empty", &resolved_path));
  EXPECT_EQ(fs::canonical(empty_bundle).string(), resolved_path);
}

TEST_F(FileTest, ResolveAssetPathRejectsMissingAndEscapingKeys) {
  const fs::path asset_root = GetTestPath("installed/assets");
  fs::create_directories(asset_root / "vehicles/cargo/calibration");
  setenv("WHEELOS_ASSET_ROOT", asset_root.c_str(), 1);

  std::string resolved_path;
  EXPECT_FALSE(ResourceManager::ResolveAssetPath(".", &resolved_path));
  EXPECT_FALSE(
      ResourceManager::ResolveAssetPath("../outside", &resolved_path));
  EXPECT_FALSE(ResourceManager::ResolveAssetPath("runtime/models/missing",
                                                 &resolved_path));
}

TEST_F(FileTest, ContentAndExistence) {
  const auto dir_path = GetTestPath("a_dir");
  const auto file_path = GetTestPath("a_file.txt");
  fs::create_directory(dir_path);
  const std::string content_to_write = "Hello, Cyber!";
  {
    std::ofstream ofs(file_path);
    ofs << content_to_write;
  }

  // Testing PathExists and DirectoryExists
  EXPECT_TRUE(PathExists(dir_path.string()));
  EXPECT_TRUE(PathExists(file_path.string()));
  EXPECT_FALSE(PathExists(GetTestPath("non_existent").string()));
  EXPECT_TRUE(DirectoryExists(dir_path.string()));
  EXPECT_FALSE(DirectoryExists(file_path.string()));

  // Testing GetContent
  std::string read_content;
  ASSERT_TRUE(GetContent(file_path.string(), &read_content));
  EXPECT_EQ(read_content, content_to_write);
  EXPECT_FALSE(GetContent("non_existent_file", &read_content));
}

TEST_F(FileTest, DirectoryModification) {
  // Testing CreateDirectory
  const auto single_dir = GetTestPath("single_dir");
  EXPECT_TRUE(CreateDirectory(single_dir.string()));
  EXPECT_TRUE(fs::is_directory(single_dir));
  EXPECT_TRUE(CreateDirectory(single_dir.string()));  // 幂等性

  // Testing CreateDirectories
  const auto nested_dir = GetTestPath("a/b/c");
  EXPECT_TRUE(CreateDirectories(nested_dir.string()));
  EXPECT_TRUE(fs::is_directory(nested_dir));

  const auto conflicting_file = GetTestPath("conflicting_file");
  {
    std::ofstream ofs(conflicting_file);
  }
  EXPECT_FALSE(CreateDirectory(conflicting_file.string()));
  EXPECT_FALSE(CreateDirectories(conflicting_file.string()));

  // Testing Remove
  EXPECT_FALSE(Remove(GetTestPath("a").string()));
  EXPECT_TRUE(fs::exists(GetTestPath("a")));

  EXPECT_TRUE(Remove(conflicting_file.string()));
  EXPECT_FALSE(fs::exists(conflicting_file));
}

TEST_F(FileTest, RemoveAll_Functionality) {
  const auto non_empty_dir = GetTestPath("non_empty_dir");
  fs::create_directories(non_empty_dir / "sub/folder");
  {
    std::ofstream ofs(non_empty_dir / "sub/file.txt");
    ofs << "data";
  }

  // Successfully deleted a non-empty directory
  EXPECT_TRUE(RemoveAll(non_empty_dir.string()));
  EXPECT_FALSE(fs::exists(non_empty_dir));

  // Idempotence: Deleting a non-existent directory also returns true
  EXPECT_TRUE(RemoveAll(non_empty_dir.string()));
}

TEST_F(FileTest, ClearDirectory) {
  const auto root_dir = GetTestPath("clear_directory");
  const auto nested_dir = root_dir / "nested";
  const auto file_path = root_dir / "file.txt";
  const auto nested_file_path = nested_dir / "nested.txt";
  fs::create_directories(nested_dir);
  {
    std::ofstream ofs(file_path);
    ofs << "data";
  }
  {
    std::ofstream ofs(nested_file_path);
    ofs << "nested";
  }

  EXPECT_TRUE(ClearDirectory(root_dir.string()));
  EXPECT_FALSE(fs::exists(file_path));
  EXPECT_FALSE(fs::exists(nested_dir));
  EXPECT_TRUE(fs::exists(root_dir));
}

TEST_F(FileTest, RemoveAll_SafetyChecks) {
  // Basic illegal path
  EXPECT_FALSE(RemoveAll(""));
  EXPECT_FALSE(RemoveAll("/"));

  // Switch to the sandbox directory to test relative path protection
  const auto original_cwd = fs::current_path();
  fs::current_path(test_root_);

  // Verify protection against the current directory
  EXPECT_FALSE(RemoveAll("."));
  EXPECT_TRUE(fs::exists("."));  // Verify the directory was not deleted

  // Verify protection against the parent directory
  EXPECT_FALSE(RemoveAll(".."));
  EXPECT_TRUE(fs::exists(".."));  // Verify the directory was not deleted

  // Verify protection against an absolute ancestor path.
  const auto parent_abs = fs::canonical("..");
  EXPECT_FALSE(RemoveAll(parent_abs.string()));
  EXPECT_TRUE(fs::exists(parent_abs));
  EXPECT_TRUE(fs::exists(test_root_));

  // Switch back to the original directory
  fs::current_path(original_cwd);
}

TEST_F(FileTest, CopyFileAndDir) {
  const auto from_file = GetTestPath("from.txt");
  const auto to_file = GetTestPath("to.txt");

  {
    std::ofstream ofs(from_file);
    ofs << "data";
  }

  ASSERT_TRUE(CopyFile(from_file.string(), to_file.string()));
  ASSERT_TRUE(fs::exists(to_file));
  std::string content;
  ASSERT_TRUE(GetContent(to_file.string(), &content));
  EXPECT_EQ(content, "data");

  const auto from_dir = GetTestPath("from_dir");
  const auto to_dir = GetTestPath("to_dir");
  fs::create_directories(from_dir / "sub");
  {
    std::ofstream ofs(from_dir / "f.txt");
    ofs << "sub-data";
  }

  ASSERT_TRUE(CopyDir(from_dir.string(), to_dir.string()));
  EXPECT_TRUE(fs::is_directory(to_dir / "sub"));
  EXPECT_TRUE(fs::exists(to_dir / "f.txt"));
}

TEST_F(FileTest, Enumeration) {
  const auto dir1 = GetTestPath("dir1");
  const auto file1 = GetTestPath("file1.txt");
  const auto file2 = GetTestPath("file2.log");
  fs::create_directory(dir1);
  { std::ofstream ofs(file1); }
  { std::ofstream ofs(file2); }

  EXPECT_THAT(
      ListSubPaths(test_root_.string(), FileTypeFilter::All),
      UnorderedElementsAre(dir1, file1, file2));
  EXPECT_THAT(ListSubPaths(test_root_.string(), FileTypeFilter::Directories),
              ElementsAre(dir1));
  EXPECT_THAT(ListSubPaths(test_root_.string(), FileTypeFilter::Files),
              UnorderedElementsAre(file1, file2));

  // Glob
  EXPECT_THAT(Glob((test_root_ / "*.txt").string()),
              ElementsAre(file1.string()));
  EXPECT_THAT(Glob((test_root_ / "*.*").string()),
              UnorderedElementsAre(file1.string(), file2.string()));
  EXPECT_THAT(Glob((test_root_ / "file?.log").string()),
              ElementsAre(file2.string()));
}

TEST_F(FileTest, GetAbsolutePath) {
  // The relative path is already an absolute path
  EXPECT_EQ("/var/log", GetAbsolutePath("/home/work", "/var/log"));
  EXPECT_EQ("/var/log", GetAbsolutePath("/home/work", "/var/lib/../log"));

  // Basic concatenation
  // Note: weakly_canonical may not change the path without actually creating
  // the directory For test stability, we assume the path format is canonical
  EXPECT_EQ(fs::path("/home/work/data.txt").string(),
            GetAbsolutePath("/home/work", "data.txt"));

  // Empty prefix, based on the current working directory
  const std::string expected_path =
      fs::weakly_canonical(fs::current_path() / "xx.txt").string();
  EXPECT_EQ(expected_path, GetAbsolutePath("", "xx.txt"));
}

TEST_F(FileTest, GetProtoFromFileLoadsBinaryBinFile) {
  proto::UnitTest message;
  message.set_class_name("BinaryProto");
  const auto bin_path = GetTestPath("message.bin");

  ASSERT_TRUE(SetProtoToBinaryFile(message, bin_path.string()));

  proto::UnitTest read_message;
  ASSERT_TRUE(GetProtoFromFile(bin_path.string(), &read_message));
  EXPECT_EQ(read_message.class_name(), "BinaryProto");
}

TEST_F(FileTest, GetProtoFromFileLoadsJsonByExtension) {
  const auto json_path = GetTestPath("message.json");
  {
    std::ofstream ofs(json_path);
    ofs << R"({"className": "JsonProto"})";
  }

  proto::UnitTest read_message;
  ASSERT_TRUE(GetProtoFromFile(json_path.string(), &read_message));
  EXPECT_EQ(read_message.class_name(), "JsonProto");
}

TEST_F(FileTest, GetProtoFromFileHonorsExplicitTextFormat) {
  proto::UnitTest message;
  message.set_class_name("BinaryProto");
  const auto bin_path = GetTestPath("message.bin");

  ASSERT_TRUE(SetProtoToBinaryFile(message, bin_path.string()));

  proto::UnitTest read_message;
  read_message.set_class_name("unchanged");
  EXPECT_FALSE(
      GetProtoFromFile(bin_path.string(), &read_message, ProtoFileFormat::Text));
  EXPECT_EQ(read_message.class_name(), "unchanged");
}

TEST_F(FileTest, GetProtoFromFileHonorsExplicitBinaryFormat) {
  proto::UnitTest message;
  message.set_class_name("TextProto");
  const auto text_path = GetTestPath("message.pb.txt");

  ASSERT_TRUE(SetProtoToASCIIFile(message, text_path.string()));

  proto::UnitTest read_message;
  read_message.set_class_name("unchanged");
  EXPECT_FALSE(GetProtoFromFile(text_path.string(), &read_message,
                                ProtoFileFormat::Binary));
  EXPECT_EQ(read_message.class_name(), "unchanged");
}

TEST_F(FileTest, ConfigRootRemainsFixedAfterInitialization) {
  const fs::path software_root = GetTestPath("software");
  const fs::path config_root = GetTestPath("assets/vehicles/cargo/config");
  const fs::path other_config_root = GetTestPath("other-config");
  const fs::path default_path =
      software_root / "modules/canbus/conf/canbus_conf.pb.txt";
  const fs::path override_path =
      config_root / "modules/canbus/conf/canbus_conf.pb.txt";
  fs::create_directories(default_path.parent_path());
  fs::create_directories(override_path.parent_path());
  fs::create_directories(other_config_root /
                         "modules/canbus/conf");
  {
    std::ofstream default_file(default_path);
    default_file << "default";
    std::ofstream override_file(override_path);
    override_file << "first";
    std::ofstream other_override_file(
        other_config_root / "modules/canbus/conf/canbus_conf.pb.txt");
    other_override_file << "second";
  }

  setenv("APOLLO_ROOT_DIR", software_root.c_str(), 1);
  setenv("WHEELOS_CONFIG_ROOT", config_root.c_str(), 1);
  ASSERT_TRUE(ResourceManager::InitializeConfigRoot());

  setenv("WHEELOS_CONFIG_ROOT", other_config_root.c_str(), 1);
  ASSERT_TRUE(ResourceManager::InitializeConfigRoot());

  std::string selected_path;
  ASSERT_TRUE(ResourceManager::ResolveConfigPath(default_path.string(),
                                                 &selected_path));
  EXPECT_EQ(fs::canonical(override_path).string(), selected_path);
}

}  // namespace common
}  // namespace cyber
}  // namespace apollo
