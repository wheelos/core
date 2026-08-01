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

#ifndef CYBER_NODE_NODE_SERVICE_IMPL_H_
#define CYBER_NODE_NODE_SERVICE_IMPL_H_

#include <memory>
#include <string>
#include <vector>

#include "cyber/common/global_data.h"
#include "cyber/common/util.h"
#include "cyber/message/message_traits.h"
#include "cyber/service/client.h"
#include "cyber/service/service.h"
#include "cyber/service_discovery/topology_manager.h"

namespace apollo {
namespace cyber {

class Node;

/**
 * @class NodeServiceImpl
 * @brief The implementation for Node to create Objects connected by Param.
 * e.g. Param Server and Client
 */
class NodeServiceImpl {
 public:
  friend class Node;

  /**
   * @brief Construct a new Node Service Impl object
   *
   * @param node_name node name
   */
  explicit NodeServiceImpl(const std::string& node_name)
      : node_name_(node_name) {
    attr_.set_host_name(common::GlobalData::Instance()->HostName());
    attr_.set_process_id(common::GlobalData::Instance()->ProcessId());
    attr_.set_node_name(node_name);
    auto node_id = common::GlobalData::RegisterNode(node_name);
    attr_.set_node_id(node_id);
  }

  /**
   * @brief Forbid default-constructor
   */
  NodeServiceImpl() = delete;

  /**
   * @brief Destroy the Node Service Impl object
   *
   */
  ~NodeServiceImpl() {}

 private:
  template <typename Request, typename Response>
  auto CreateService(const std::string& service_name,
                     const typename Service<Request, Response>::ServiceCallback&
                         service_callback) ->
      typename std::shared_ptr<Service<Request, Response>>;

  template <typename Request, typename Response>
  auto CreateService(
      const std::string& service_name, const ServiceOptions& options,
      const typename Service<Request, Response>::ServiceCallback&
          service_callback) ->
      typename std::shared_ptr<Service<Request, Response>>;

  template <typename Request, typename Response>
  auto CreateService(
      const std::string& service_name, const ServiceOptions& options,
      const typename Service<Request, Response>::ContextServiceCallback&
          service_callback) ->
      typename std::shared_ptr<Service<Request, Response>>;

  template <typename Request, typename Response>
  auto CreateClient(const std::string& service_name) ->
      typename std::shared_ptr<Client<Request, Response>>;

  std::vector<std::weak_ptr<ServiceBase>> service_list_;
  std::vector<std::weak_ptr<ClientBase>> client_list_;
  std::string node_name_;
  proto::RoleAttributes attr_;
};

template <typename Request, typename Response>
auto NodeServiceImpl::CreateService(
    const std::string& service_name,
    const typename Service<Request, Response>::ServiceCallback&
        service_callback) ->
    typename std::shared_ptr<Service<Request, Response>> {
  return CreateService<Request, Response>(service_name, ServiceOptions(),
                                          service_callback);
}

template <typename Request, typename Response>
auto NodeServiceImpl::CreateService(
    const std::string& service_name, const ServiceOptions& options,
    const typename Service<Request, Response>::ServiceCallback&
        service_callback) ->
    typename std::shared_ptr<Service<Request, Response>> {
  typename Service<Request, Response>::ContextServiceCallback callback =
      [service_callback](RpcContext&, const std::shared_ptr<Request>& request,
                         std::shared_ptr<Response>& response) {
        service_callback(request, response);
        return RpcStatus::OK();
      };
  return CreateService<Request, Response>(service_name, options, callback);
}

template <typename Request, typename Response>
auto NodeServiceImpl::CreateService(
    const std::string& service_name, const ServiceOptions& options,
    const typename Service<Request, Response>::ContextServiceCallback&
        service_callback) ->
    typename std::shared_ptr<Service<Request, Response>> {
  auto service_manager =
      service_discovery::TopologyManager::Instance()->service_manager();
  std::vector<proto::RoleAttributes> existing_servers;
  service_manager->GetServers(service_name, &existing_servers);
  if (!existing_servers.empty()) {
    if (options.instance_policy == ServiceInstancePolicy::EXCLUSIVE) {
      AERROR << "Service already exists: " << service_name;
      return nullptr;
    }
    for (const auto& server : existing_servers) {
      if (!server.service_replicated()) {
        AERROR << "Cannot replicate exclusive service: " << service_name;
        return nullptr;
      }
    }
  }

  ServiceOptions resolved_options = options;
  if (resolved_options.instance_id == 0) {
    resolved_options.instance_id = common::Hash(
        attr_.host_name() + std::to_string(attr_.process_id()) + node_name_ +
        service_name);
  }
  auto service_ptr = std::make_shared<Service<Request, Response>>(
      node_name_, service_name, resolved_options, service_callback);
  RETURN_VAL_IF(!service_ptr->Init(), nullptr);

  service_list_.emplace_back(service_ptr);
  auto role_attr = attr_;
  role_attr.set_service_name(service_name);
  auto service_id = common::GlobalData::RegisterService(service_name);
  role_attr.set_service_id(service_id);
  role_attr.set_service_request_type(message::MessageType<Request>());
  role_attr.set_service_response_type(message::MessageType<Response>());
  role_attr.set_service_interface_major(resolved_options.interface_major);
  role_attr.set_service_interface_minor(resolved_options.interface_minor);
  role_attr.set_service_instance_id(resolved_options.instance_id);
  role_attr.set_service_request_channel(service_ptr->request_channel());
  role_attr.set_service_replicated(
      resolved_options.instance_policy == ServiceInstancePolicy::REPLICATED);
  service_manager->Join(role_attr, RoleType::ROLE_SERVER);
  service_ptr->SetOnShutdown([service_manager, role_attr]() {
    service_manager->Leave(role_attr, RoleType::ROLE_SERVER);
  });
  return service_ptr;
}

template <typename Request, typename Response>
auto NodeServiceImpl::CreateClient(const std::string& service_name) ->
    typename std::shared_ptr<Client<Request, Response>> {
  auto client_ptr =
      std::make_shared<Client<Request, Response>>(node_name_, service_name);
  RETURN_VAL_IF(!client_ptr->Init(), nullptr);

  client_list_.emplace_back(client_ptr);
  auto role_attr = attr_;
  role_attr.set_service_name(service_name);
  auto service_id = common::GlobalData::RegisterService(service_name);
  role_attr.set_service_id(service_id);
  role_attr.set_service_request_type(message::MessageType<Request>());
  role_attr.set_service_response_type(message::MessageType<Response>());
  auto service_manager =
      service_discovery::TopologyManager::Instance()->service_manager();
  service_manager->Join(role_attr, RoleType::ROLE_CLIENT);
  client_ptr->SetOnShutdown([service_manager, role_attr]() {
    service_manager->Leave(role_attr, RoleType::ROLE_CLIENT);
  });
  return client_ptr;
}

}  // namespace cyber
}  // namespace apollo

#endif  // CYBER_NODE_NODE_SERVICE_IMPL_H_
