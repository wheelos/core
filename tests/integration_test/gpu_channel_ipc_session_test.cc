/******************************************************************************
 * Copyright 2026 WheelOS. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *****************************************************************************/

#include <sys/stat.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "cyber/transport/nvsci/gpu_channel_manager.h"
#include "cyber/transport/nvsci/gpu_control_protocol.h"

namespace apollo {
namespace cyber {
namespace transport {
namespace {

TEST(GpuChannelIpcSessionTest,
     RejectsNonIpcDescriptorWithoutInstallingSession) {
  auto* manager = GpuChannelManager::Instance();
  manager->Clear();

  NvSciBufPoolConfig config;
  config.slot_count = 1;
  config.slot_size = 4096;

  GpuBufferDescriptor descriptor;
  descriptor.slot_id = 0;
  descriptor.capacity = config.slot_size;
  descriptor.nvsci_buf_ipc_desc.resize(20, 0);
  const uint64_t simulated_magic = 0x5343494255463031ULL;
  std::memcpy(descriptor.nvsci_buf_ipc_desc.data() + 12, &simulated_magic,
              sizeof(simulated_magic));

  EXPECT_EQ(manager->ImportSession("ipc-reject", config, {descriptor}),
            GpuChannelIpcStatus::kUnsupported);
  EXPECT_EQ(manager->GetSession("ipc-reject"), nullptr);
  manager->Clear();
}

TEST(GpuChannelIpcSessionTest, ExportRequiresRealCrossProcessHandles) {
  auto* manager = GpuChannelManager::Instance();
  manager->Clear();

  NvSciBufPoolConfig config;
  config.slot_count = 1;
  config.slot_size = 4096;
  ASSERT_NE(manager->GetOrCreateSession("ipc-export", config), nullptr);

  std::vector<GpuBufferDescriptor> descriptors;
  const GpuChannelIpcStatus status =
      manager->ExportSession("ipc-export", &descriptors);
  EXPECT_TRUE(status == GpuChannelIpcStatus::kSuccess ||
              status == GpuChannelIpcStatus::kUnsupported);
  if (status == GpuChannelIpcStatus::kUnsupported) {
    EXPECT_TRUE(descriptors.empty());
  } else {
    ASSERT_EQ(descriptors.size(), 1U);
    EXPECT_EQ(descriptors[0].slot_id, 0U);
    EXPECT_EQ(descriptors[0].capacity, config.slot_size);
  }
  manager->Clear();
}

TEST(GpuChannelIpcSessionTest, FullSessionDescriptorIsVersionedAndComplete) {
  auto* manager = GpuChannelManager::Instance();
  manager->Clear();
  NvSciBufPoolConfig config;
  config.slot_count = 2;
  config.slot_size = 4096;
  config.alignment = 256;
  ASSERT_NE(manager->GetOrCreateSession("ipc-full-export", config), nullptr);

  GpuSessionDescriptor descriptor;
  const auto status = manager->ExportSession("ipc-full-export", &descriptor);
  if (status == GpuChannelIpcStatus::kUnsupported) {
    manager->Clear();
    GTEST_SKIP() << "CUDA IPC session export is unavailable on this host.";
  }
  ASSERT_EQ(status, GpuChannelIpcStatus::kSuccess);
  EXPECT_NE(descriptor.channel_id, 0);
  EXPECT_NE(descriptor.session_id, 0);
  EXPECT_EQ(descriptor.buffers.size(), config.slot_count);
  EXPECT_FALSE(descriptor.producer_sync_desc.empty());

  const std::string wire = EncodeGpuSessionDescriptor(descriptor);
  GpuSessionDescriptor decoded;
  ASSERT_TRUE(DecodeGpuSessionDescriptor(wire, &decoded));
  EXPECT_EQ(decoded.channel_id, descriptor.channel_id);
  EXPECT_EQ(decoded.session_id, descriptor.session_id);
  EXPECT_EQ(decoded.config.alignment, config.alignment);
  EXPECT_EQ(decoded.producer_sync_desc, descriptor.producer_sync_desc);
  manager->Clear();
}

TEST(GpuChannelIpcSessionTest, WriterOwnershipIsExclusiveAndIdentityScoped) {
  const char* test_tmpdir = std::getenv("TEST_TMPDIR");
  if (test_tmpdir != nullptr) {
    const std::string lock_base = std::string(test_tmpdir) + "/gpu_writer_test";
    ASSERT_TRUE(::mkdir(lock_base.c_str(), 0700) == 0 || errno == EEXIST);
    ASSERT_EQ(::setenv("XDG_RUNTIME_DIR", lock_base.c_str(), 1), 0);
  }
  auto* manager = GpuChannelManager::Instance();
  manager->Clear();
  NvSciBufPoolConfig config;
  config.slot_count = 2;
  config.slot_size = 4096;
  auto session = manager->GetOrCreateSession("single-writer", config);
  ASSERT_NE(session, nullptr);

  EXPECT_TRUE(manager->ClaimWriterSession("single-writer", session));
  EXPECT_FALSE(manager->ClaimWriterSession("single-writer", session));
  manager->ReleaseWriterSession("single-writer", session->session_id() + 1);
  EXPECT_EQ(manager->GetSession("single-writer"), session);
  manager->ReleaseWriterSession("single-writer", session->session_id());
  EXPECT_EQ(manager->GetSession("single-writer"), nullptr);
  manager->Clear();
}

}  // namespace
}  // namespace transport
}  // namespace cyber
}  // namespace apollo
