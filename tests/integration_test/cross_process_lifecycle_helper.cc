// Copyright 2026 WheelOS. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <unistd.h>

#include "cyber/cyber.h"
#include "cyber/service_discovery/topology_manager.h"
#include "examples/proto/examples.pb.h"

namespace {

using apollo::cyber::examples::proto::Chatter;
using apollo::cyber::examples::proto::Driver;

std::string NodeName(const std::string& prefix) {
  return prefix + "_" + std::to_string(getpid());
}

int RunWriter(const std::string& channel, const std::string& token) {
  auto node = apollo::cyber::CreateNode(NodeName("churn_writer"));
  auto writer = node->CreateWriter<Chatter>(channel);
  if (writer == nullptr) {
    return 2;
  }
  std::cout << "ENDPOINT_CREATED writer" << std::endl;
  uint64_t sequence = 0;
  bool had_reader = false;
  uint64_t writes_after_topology = 0;
  while (apollo::cyber::OK()) {
    const bool has_reader = writer->HasReader();
    if (has_reader != had_reader) {
      std::cout << (has_reader ? "TOPOLOGY_OBSERVED reader"
                               : "TOPOLOGY_LOST reader")
                << std::endl;
      had_reader = has_reader;
      writes_after_topology = 0;
    }
    auto message = std::make_shared<Chatter>();
    message->set_seq(sequence++);
    message->set_content(token);
    message->set_timestamp(sequence);
    if (!writer->Write(message)) {
      std::cerr << "WRITE_FAILED" << std::endl;
      return 3;
    }
    if (has_reader && ++writes_after_topology == 100) {
      std::cout << "WRITE_ACCEPTED_AFTER_TOPOLOGY count=100" << std::endl;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return 0;
}

int RunReader(const std::string& channel) {
  std::mutex mutex;
  std::string last_token;
  uint64_t delivery_count = 0;
  auto node = apollo::cyber::CreateNode(NodeName("churn_reader"));
  auto reader = node->CreateReader<Chatter>(
      channel, [&](const std::shared_ptr<Chatter>& message) {
        std::lock_guard<std::mutex> lock(mutex);
        if (message->content() != last_token) {
          last_token = message->content();
          delivery_count = 0;
        }
        ++delivery_count;
        if (delivery_count <= 2) {
          std::cout << "DELIVERY " << last_token
                    << " count=" << delivery_count
                    << " seq=" << message->seq() << std::endl;
        }
      });
  if (reader == nullptr) {
    return 2;
  }
  std::cout << "ENDPOINT_CREATED reader" << std::endl;
  bool had_writer = false;
  while (apollo::cyber::OK()) {
    const bool has_writer = reader->HasWriter();
    if (has_writer != had_writer) {
      std::cout << (has_writer ? "TOPOLOGY_OBSERVED writer"
                               : "TOPOLOGY_LOST writer")
                << std::endl;
      had_writer = has_writer;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return 0;
}

int RunServer(const std::string& service_name, const std::string& token) {
  auto node = apollo::cyber::CreateNode(NodeName("churn_server"));
  auto service = node->CreateService<Driver, Driver>(
      service_name,
      [token](const std::shared_ptr<Driver>& request,
              std::shared_ptr<Driver>& response) {
        std::cout << "REQUEST_DELIVERED " << request->content() << std::endl;
        response->set_content(token + ":" + request->content());
        response->set_msg_id(request->msg_id() + 1);
        response->set_timestamp(request->timestamp() + 1);
      });
  if (service == nullptr) {
    return 2;
  }
  std::cout << "ENDPOINT_CREATED server" << std::endl;
  apollo::cyber::WaitForShutdown();
  return 0;
}

int RunClient(const std::string& service_name,
              const std::string& request_token, bool keep_running) {
  auto node = apollo::cyber::CreateNode(NodeName("churn_client"));
  auto client = node->CreateClient<Driver, Driver>(service_name);
  if (client == nullptr) {
    return 2;
  }
  std::cout << "ENDPOINT_CREATED client" << std::endl;

  uint64_t sequence = 0;
  auto service_manager =
      apollo::cyber::service_discovery::TopologyManager::Instance()
          ->service_manager();
  bool service_was_present = service_manager->HasService(service_name);
  std::cout << "SERVICE_TOPOLOGY "
            << (service_was_present ? "present" : "absent") << std::endl;
  bool response_was_available = false;
  while (apollo::cyber::OK()) {
    const bool service_is_present =
        service_manager->HasService(service_name);
    if (service_is_present != service_was_present) {
      std::cout << "SERVICE_TOPOLOGY "
                << (service_is_present ? "present" : "absent") << std::endl;
      service_was_present = service_is_present;
    }
    if (!service_is_present) {
      if (keep_running && response_was_available) {
        response_was_available = false;
        std::cout << "RESPONSE_LOST service" << std::endl;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }
    auto request = std::make_shared<Driver>();
    request->set_content(request_token);
    request->set_msg_id(sequence);
    request->set_timestamp(sequence);
    auto response =
        client->SendRequest(request, std::chrono::seconds(1));
    if (response != nullptr &&
        response->msg_id() == request->msg_id() + 1 &&
        response->timestamp() == request->timestamp() + 1) {
      if (keep_running && !response_was_available) {
        std::cout << "RESPONSE_RECOVERED service" << std::endl;
      }
      response_was_available = true;
      std::cout << "RESPONSE " << response->content() << std::endl;
      if (!keep_running) {
        return 0;
      }
    } else if (keep_running && response_was_available) {
      response_was_available = false;
      std::cout << "RESPONSE_LOST service" << std::endl;
    }
    ++sequence;
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    return 1;
  }
  apollo::cyber::Init(argv[0]);

  int result = 1;
  const std::string mode = argv[1];
  if (mode == "writer" && argc == 4) {
    result = RunWriter(argv[2], argv[3]);
  } else if (mode == "reader" && argc == 3) {
    result = RunReader(argv[2]);
  } else if (mode == "server" && argc == 4) {
    result = RunServer(argv[2], argv[3]);
  } else if (mode == "client" && argc == 4) {
    result = RunClient(argv[2], argv[3], false);
  } else if (mode == "client_watch" && argc == 4) {
    result = RunClient(argv[2], argv[3], true);
  }

  apollo::cyber::Clear();
  return result;
}
