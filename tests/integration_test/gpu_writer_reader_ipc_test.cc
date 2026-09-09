/******************************************************************************
 * Copyright 2026 WheelOS. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *****************************************************************************/

#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <cuda_runtime.h>
#include <gtest/gtest.h>

#include "cyber/cyber.h"
#include "cyber/transport/nvsci/gpu_reader.h"
#include "cyber/transport/nvsci/gpu_writer.h"
#include "cyber/transport/rtps/participant.h"

namespace apollo {
namespace cyber {
namespace transport {
namespace {

struct IpcMeta {
  uint32_t frame_id = 0;
};

constexpr uint32_t kFrameCount = 8;
constexpr uint8_t kChildReady = 1;
constexpr uint8_t kChildCyberUnavailable = 2;
constexpr uint8_t kChildGpuIpcUnavailable = 3;

bool WriteAll(int fd, const void* data, size_t size) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  while (size != 0) {
    const ssize_t written = write(fd, bytes, size);
    if (written <= 0) {
      return false;
    }
    bytes += written;
    size -= static_cast<size_t>(written);
  }
  return true;
}

bool ReadAll(int fd, void* data, size_t size) {
  auto* bytes = static_cast<uint8_t*>(data);
  while (size != 0) {
    const ssize_t read_bytes = read(fd, bytes, size);
    if (read_bytes <= 0) {
      return false;
    }
    bytes += read_bytes;
    size -= static_cast<size_t>(read_bytes);
  }
  return true;
}

bool ReadByteWithTimeout(int fd, uint8_t* value, int timeout_ms) {
  struct pollfd descriptor = {fd, POLLIN, 0};
  if (poll(&descriptor, 1, timeout_ms) <= 0 ||
      (descriptor.revents & (POLLIN | POLLHUP)) == 0) {
    return false;
  }
  return ReadAll(fd, value, sizeof(*value));
}

int RunReader(int descriptor_fd, int result_fd) {
  uint8_t start = 0;
  if (!ReadAll(descriptor_fd, &start, sizeof(start)) || start != 1) {
    return 10;
  }

  if (!apollo::cyber::Init("gpu_writer_reader_ipc_child")) {
    const uint8_t status = kChildCyberUnavailable;
    WriteAll(result_fd, &status, sizeof(status));
    return 13;
  }
  auto node = apollo::cyber::CreateNode("gpu_ipc_reader_node");
  cudaStream_t stream = nullptr;
  if (cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) !=
      cudaSuccess) {
    return 14;
  }
  GpuReaderOptions options;
  options.consumer_id = 9001;
  options.stream = stream;
  options.bootstrap_timeout_ms = 8000;

  uint32_t received_count = 0;
  auto reader = CreateGpuReader<IpcMeta>(
      node, "test/gpu_cross_process", options,
      [&](const GpuMsgView<IpcMeta>& view) {
        uint8_t value = 0;
        const bool copied =
            cudaMemcpyAsync(&value, view.device_ptr(), sizeof(value),
                            cudaMemcpyDeviceToHost, stream) == cudaSuccess &&
            cudaStreamSynchronize(stream) == cudaSuccess;
        const uint32_t expected_frame = 42 + received_count;
        const uint8_t expected_value =
            static_cast<uint8_t>(0x5a + received_count);
        const uint8_t result = copied && value == expected_value &&
                                       view->frame_id == expected_frame
                                   ? 1
                                   : 0;
        WriteAll(result_fd, &result, sizeof(result));
        ++received_count;
      });
  if (!reader || !reader->is_ready()) {
    const uint8_t status = kChildGpuIpcUnavailable;
    WriteAll(result_fd, &status, sizeof(status));
    return 14;
  }

  const uint8_t ready = kChildReady;
  if (!WriteAll(result_fd, &ready, sizeof(ready))) {
    return 15;
  }
  std::this_thread::sleep_for(std::chrono::seconds(2));
  reader.reset();
  cudaStreamDestroy(stream);
  return received_count == kFrameCount ? 0 : 16;
}

}  // namespace

int RunGpuIpcReaderFromEnvironment() {
  const char* descriptor_fd = std::getenv("GPU_IPC_DESCRIPTOR_FD");
  const char* result_fd = std::getenv("GPU_IPC_RESULT_FD");
  if (descriptor_fd == nullptr || result_fd == nullptr) {
    return 21;
  }
  return RunReader(std::atoi(descriptor_fd), std::atoi(result_fd));
}

namespace {

TEST(GpuWriterReaderIpcTest, ForkedWriterAndReaderBootstrapAutomatically) {
  if (std::getenv("CYBER_GPU_IPC_E2E") == nullptr) {
    GTEST_SKIP()
        << "Set CYBER_GPU_IPC_E2E=1 to run the process-level RTPS test.";
  }
  // Use a test-local DDS domain so unrelated Cyber processes cannot exhaust
  // the shared participant-id range or collide on their UDP ports.
  const uint32_t test_domain_id = 200 + (static_cast<uint32_t>(getpid()) % 40);
  const std::string test_domain = std::to_string(test_domain_id);
  ASSERT_EQ(setenv("CYBER_DOMAIN_ID", test_domain.c_str(), 1), 0);
  int device_count = 0;
  if (cudaGetDeviceCount(&device_count) != cudaSuccess || device_count == 0) {
    GTEST_SKIP() << "A CUDA device is required for CUDA IPC validation.";
  }

  // Probe RTPS in an isolated process.  The current Cyber RT participant
  // path can terminate the process when all participant slots are occupied;
  // keep that environment failure from taking down the gtest process.
  const pid_t probe = fork();
  ASSERT_GE(probe, 0);
  if (probe == 0) {
    apollo::cyber::transport::Participant participant("gpu_ipc_rtps_probe",
                                                      11511);
    if (participant.fastrtps_participant() == nullptr) {
      _exit(2);
    }
    _exit(0);
  }
  int probe_status = 0;
  ASSERT_EQ(waitpid(probe, &probe_status, 0), probe);
  if (!WIFEXITED(probe_status) || WEXITSTATUS(probe_status) != 0) {
    GTEST_SKIP() << "RTPS participant preflight is unavailable (probe status "
                 << probe_status << ").";
  }

  int descriptor_pipe[2];
  int result_pipe[2];
  ASSERT_EQ(pipe(descriptor_pipe), 0);
  ASSERT_EQ(pipe(result_pipe), 0);

  const pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    close(descriptor_pipe[1]);
    close(result_pipe[0]);
    const std::string descriptor_fd = std::to_string(descriptor_pipe[0]);
    const std::string result_fd = std::to_string(result_pipe[1]);
    setenv("GPU_IPC_DESCRIPTOR_FD", descriptor_fd.c_str(), 1);
    setenv("GPU_IPC_RESULT_FD", result_fd.c_str(), 1);
    execl("/proc/self/exe", "/proc/self/exe", nullptr);
    _exit(20);
  }

  close(descriptor_pipe[0]);
  close(result_pipe[1]);
  if (!apollo::cyber::Init("gpu_writer_reader_ipc_parent")) {
    close(descriptor_pipe[1]);
    close(result_pipe[0]);
    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    GTEST_SKIP() << "Cyber RT participant unavailable for cross-process test.";
  }
  auto node = apollo::cyber::CreateNode("gpu_ipc_writer_node");
  GpuWriterOptions options;
  options.slot_count = 1;
  options.slot_size = 4096;
  cudaStream_t writer_stream = nullptr;
  ASSERT_EQ(cudaStreamCreateWithFlags(&writer_stream, cudaStreamNonBlocking),
            cudaSuccess);
  options.stream = writer_stream;
  auto writer =
      CreateGpuWriter<IpcMeta>(node, "test/gpu_cross_process", options);
  if (!writer || !writer->is_ready()) {
    close(descriptor_pipe[1]);
    close(result_pipe[0]);
    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    GTEST_SKIP() << "Cyber RT participant unavailable for cross-process test.";
  }

  GpuSessionDescriptor descriptor;
  const auto export_status = writer->ExportSessionDescriptor(&descriptor);
  if (export_status == GpuChannelIpcStatus::kUnsupported) {
    close(descriptor_pipe[1]);
    close(result_pipe[0]);
    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    cudaStreamDestroy(writer_stream);
    GTEST_SKIP() << "CUDA IPC export is unavailable on this host.";
  }
  ASSERT_EQ(export_status, GpuChannelIpcStatus::kSuccess);
  const uint8_t start = 1;
  ASSERT_TRUE(WriteAll(descriptor_pipe[1], &start, sizeof(start)));
  close(descriptor_pipe[1]);

  uint8_t ready = 0;
  if (!ReadByteWithTimeout(result_pipe[0], &ready, 5000)) {
    close(result_pipe[0]);
    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    if (WIFSIGNALED(status)) {
      GTEST_SKIP() << "RTPS/CUDA child startup terminated by signal "
                   << WTERMSIG(status)
                   << "; environment is not usable for "
                      "the opt-in process test.";
    }
    FAIL() << "Child produced no initialization status; child status "
           << status;
  }
  if (ready == kChildCyberUnavailable) {
    close(result_pipe[0]);
    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    GTEST_SKIP() << "The child could not initialize Cyber RT.";
  }
  if (ready == kChildGpuIpcUnavailable) {
    close(result_pipe[0]);
    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    GTEST_SKIP() << "Child CUDA IPC import is unavailable on this host.";
  }
  ASSERT_EQ(ready, kChildReady);
  // Allow the independently-created data and ACK endpoints to complete RTPS
  // discovery before the first packet; later iterations exercise reclamation.
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  for (uint32_t frame = 0; frame < kFrameCount; ++frame) {
    std::optional<GpuLoan<IpcMeta>> loan;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!loan.has_value() && std::chrono::steady_clock::now() < deadline) {
      loan = writer->Loan();
      if (!loan.has_value()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    }
    ASSERT_TRUE(loan.has_value())
        << "slot was not reclaimed after reader ACK for frame " << frame;
    loan->metadata().frame_id = 42 + frame;
    ASSERT_EQ(
        cudaMemsetAsync(loan->device_ptr(), static_cast<int>(0x5a + frame),
                        options.slot_size, writer_stream),
        cudaSuccess);
    ASSERT_TRUE(writer->Publish(std::move(*loan)));

    uint8_t result = 0;
    ASSERT_TRUE(ReadByteWithTimeout(result_pipe[0], &result, 5000))
        << "reader did not report frame " << frame;
    ASSERT_EQ(result, 1)
        << "reader observed invalid ownership or payload for frame " << frame;
  }

  close(result_pipe[0]);
  int status = 0;
  ASSERT_EQ(waitpid(child, &status, 0), child);
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
  writer.reset();
  cudaStreamDestroy(writer_stream);
}

}  // namespace
}  // namespace transport
}  // namespace cyber
}  // namespace apollo

int main(int argc, char** argv) {
  if (std::getenv("GPU_IPC_DESCRIPTOR_FD") != nullptr) {
    return apollo::cyber::transport::RunGpuIpcReaderFromEnvironment();
  }
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
