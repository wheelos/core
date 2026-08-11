#include "node_bindings.h"

#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "cyber/common/log.h"
#include "cyber/cyber.h"
#include "cyber/message/protobuf_factory.h"
#include "cyber/message/py_message.h"
#include "cyber/message/raw_message.h"
#include "cyber/proto/role_attributes.pb.h"
#include "binding_registrars.h"
#include "callback_lifecycle.h"
#include "module_state.h"

namespace apollo {
namespace cyber {

using message::PyMessageWrap;
using message::RawMessage;

namespace {

void WaitForDrainWithOptionalGilRelease(CallbackLifecycle* lifecycle) {
  if (PyGILState_Check()) {
    py::gil_scoped_release release;
    lifecycle->WaitForDrain();
    return;
  }
  lifecycle->WaitForDrain();
}

}  // namespace

class WriterBinding {
 public:
  WriterBinding(std::shared_ptr<Node> node, std::string channel_name,
                std::string data_type, uint32_t qos_depth)
      : node_(std::move(node)),
        channel_name_(std::move(channel_name)),
        data_type_(std::move(data_type)) {
    std::string proto_desc;
    message::ProtobufFactory::Instance()->GetDescriptorString(data_type_,
                                                              &proto_desc);
    if (proto_desc.empty()) {
      throw std::runtime_error("message descriptor is not registered: " +
                               data_type_);
    }

    proto::RoleAttributes role_attr;
    role_attr.set_channel_name(channel_name_);
    role_attr.set_message_type(data_type_);
    role_attr.set_proto_desc(proto_desc);
    role_attr.mutable_qos_profile()->set_depth(qos_depth);
    writer_ = node_->CreateWriter<PyMessageWrap>(role_attr);
    if (writer_ == nullptr) {
      throw std::runtime_error("failed to create writer for channel " +
                               channel_name_);
    }
  }

  bool write(const py::bytes& data) {
    EnsureOpen(writer_, "writer");
    auto message =
        std::make_shared<PyMessageWrap>(BytesToString(data), data_type_);
    message->set_type_name(data_type_);
    return writer_->Write(message);
  }

  void close() {
    writer_.reset();
    node_.reset();
  }

 private:
  std::shared_ptr<Node> node_;
  std::string channel_name_;
  std::string data_type_;
  std::shared_ptr<Writer<PyMessageWrap>> writer_;
};

class ClientBinding {
 public:
  ClientBinding(std::shared_ptr<Node> node, std::string service_name,
                std::string request_type)
      : node_(std::move(node)),
        service_name_(std::move(service_name)),
        request_type_(std::move(request_type)) {
    client_ = node_->CreateClient<PyMessageWrap, PyMessageWrap>(service_name_);
    if (client_ == nullptr) {
      throw std::runtime_error("failed to create client for service " +
                               service_name_);
    }
  }

  py::bytes send_request(const py::bytes& request) {
    EnsureOpen(client_, "client");
    auto request_message =
        std::make_shared<PyMessageWrap>(BytesToString(request), request_type_);

    std::shared_ptr<PyMessageWrap> response;
    {
      py::gil_scoped_release release;
      response = client_->SendRequest(request_message);
    }

    if (response == nullptr) {
      return py::bytes("");
    }
    response->ParseFromString(response->data());
    return StringToBytes(response->data());
  }

  void close() {
    client_.reset();
    node_.reset();
  }

 private:
  std::shared_ptr<Node> node_;
  std::string service_name_;
  std::string request_type_;
  std::shared_ptr<Client<PyMessageWrap, PyMessageWrap>> client_;
};

class ReaderBinding : public std::enable_shared_from_this<ReaderBinding> {
 public:
  ReaderBinding(std::shared_ptr<Node> node, std::string channel_name,
                bool raw_data, py::function callback)
      : node_(std::move(node)),
        channel_name_(std::move(channel_name)),
        raw_data_(raw_data) {
    lifecycle_.SetCallback(std::move(callback));

    if (raw_data_) {
      auto handler = [this](const std::shared_ptr<const PyMessageWrap>& msg) {
        Dispatch(msg->data());
      };
      wrapped_reader_ = node_->CreateReader<PyMessageWrap>(channel_name_, handler);
      if (wrapped_reader_ == nullptr) {
        throw std::runtime_error("failed to create raw-data reader for channel " +
                                 channel_name_);
      }
      return;
    }

    auto handler = [this](const std::shared_ptr<const RawMessage>& msg) {
      Dispatch(msg->message);
    };
    raw_reader_ = node_->CreateReader<RawMessage>(channel_name_, handler);
    if (raw_reader_ == nullptr) {
      throw std::runtime_error("failed to create reader for channel " +
                               channel_name_);
    }
  }

  ~ReaderBinding() {
    try {
      close();
    } catch (...) {
    }
  }

  void close() {
    if (!lifecycle_.BeginClose()) {
      return;
    }

    if (lifecycle_.IsActiveOnCurrentThread()) {
      RunDetachedCloseTask(
          [self = shared_from_this()]() mutable { self->FinishClose(); });
      return;
    }

    FinishClose();
  }

 private:
  void FinishClose() {
    WaitForDrainWithOptionalGilRelease(&lifecycle_);
    wrapped_reader_.reset();
    raw_reader_.reset();
    py::gil_scoped_acquire acquire;
    lifecycle_.ClearCallback();
    node_.reset();
  }

  void Dispatch(const std::string& payload) {
    if (lifecycle_.IsClosed() || CallbacksSuppressed()) {
      return;
    }

    py::gil_scoped_acquire acquire;

    py::function callback;
    CallbackToken token;
    if (!lifecycle_.Acquire(&callback, &token)) {
      return;
    }

    try {
      callback(StringToBytes(payload));
    } catch (py::error_already_set& err) {
      DiscardUnhandledPythonError(err, "pycyber reader callback");
    }
  }

  std::shared_ptr<Node> node_;
  std::string channel_name_;
  bool raw_data_ = false;
  CallbackLifecycle lifecycle_;
  std::shared_ptr<Reader<PyMessageWrap>> wrapped_reader_;
  std::shared_ptr<Reader<RawMessage>> raw_reader_;
};

class ServiceBinding : public std::enable_shared_from_this<ServiceBinding> {
 public:
  ServiceBinding(std::shared_ptr<Node> node, std::string service_name,
                 std::string response_type, py::function callback)
      : node_(std::move(node)),
        service_name_(std::move(service_name)),
        response_type_(std::move(response_type)) {
    lifecycle_.SetCallback(std::move(callback));

    auto handler = [this](const std::shared_ptr<const PyMessageWrap>& request,
                          std::shared_ptr<PyMessageWrap>& response) {
      response = HandleRequest(request);
    };
    service_ =
        node_->CreateService<PyMessageWrap, PyMessageWrap>(service_name_, handler);
    if (service_ == nullptr) {
      throw std::runtime_error("failed to create service " + service_name_);
    }
  }

  ~ServiceBinding() {
    try {
      close();
    } catch (...) {
    }
  }

  void close() {
    if (!lifecycle_.BeginClose()) {
      return;
    }

    if (lifecycle_.IsActiveOnCurrentThread()) {
      RunDetachedCloseTask(
          [self = shared_from_this()]() mutable { self->FinishClose(); });
      return;
    }

    FinishClose();
  }

 private:
  void FinishClose() {
    service_.reset();
    WaitForDrainWithOptionalGilRelease(&lifecycle_);
    py::gil_scoped_acquire acquire;
    lifecycle_.ClearCallback();
    node_.reset();
  }

  std::shared_ptr<PyMessageWrap> HandleRequest(
      const std::shared_ptr<const PyMessageWrap>& request) {
    std::string response_data;

    if (lifecycle_.IsClosed() || CallbacksSuppressed()) {
      return std::make_shared<PyMessageWrap>(response_data, response_type_);
    }

    py::gil_scoped_acquire acquire;

    py::function callback;
    CallbackToken token;
    if (!lifecycle_.Acquire(&callback, &token)) {
      return std::make_shared<PyMessageWrap>(response_data, response_type_);
    }

    try {
      py::object response = callback(StringToBytes(request->data()));
      if (!response.is_none()) {
        response_data = response.cast<std::string>();
      }
    } catch (py::error_already_set& err) {
      DiscardUnhandledPythonError(err, "pycyber service callback");
    }

    return std::make_shared<PyMessageWrap>(response_data, response_type_);
  }

  std::shared_ptr<Node> node_;
  std::string service_name_;
  std::string response_type_;
  CallbackLifecycle lifecycle_;
  std::shared_ptr<Service<PyMessageWrap, PyMessageWrap>> service_;
};

NodeBinding::NodeBinding(const std::string& node_name)
    : node_name_(node_name), node_(CreateNode(node_name)) {
  if (node_ == nullptr) {
    throw std::runtime_error("failed to create node " + node_name);
  }
}

void NodeBinding::close() { node_.reset(); }

void NodeBinding::register_message(const py::bytes& descriptor) {
  EnsureOpen();
  message::ProtobufFactory::Instance()->RegisterPythonMessage(
      BytesToString(descriptor));
}

std::shared_ptr<WriterBinding> NodeBinding::create_writer(
    const std::string& channel_name, const std::string& data_type,
    uint32_t qos_depth) {
  EnsureOpen();
  return std::make_shared<WriterBinding>(node_, channel_name, data_type,
                                         qos_depth);
}

std::shared_ptr<ReaderBinding> NodeBinding::create_reader(
    const std::string& channel_name, bool raw_data, py::function callback) {
  EnsureOpen();
  return std::make_shared<ReaderBinding>(node_, channel_name, raw_data,
                                         std::move(callback));
}

std::shared_ptr<ClientBinding> NodeBinding::create_client(
    const std::string& service_name, const std::string& request_type) {
  EnsureOpen();
  return std::make_shared<ClientBinding>(node_, service_name, request_type);
}

std::shared_ptr<ServiceBinding> NodeBinding::create_service(
    const std::string& service_name, const std::string& response_type,
    py::function callback) {
  EnsureOpen();
  return std::make_shared<ServiceBinding>(node_, service_name, response_type,
                                          std::move(callback));
}

std::shared_ptr<Node> NodeBinding::shared_node() const {
  EnsureOpen();
  return node_;
}

void NodeBinding::EnsureOpen() const {
  if (node_ == nullptr) {
    throw std::runtime_error("node has been closed");
  }
}

void RegisterNodeBindings(py::module_& m) {
  py::class_<WriterBinding, std::shared_ptr<WriterBinding>>(m, "Writer")
      .def("write", &WriterBinding::write)
      .def("close", &WriterBinding::close);

  py::class_<ReaderBinding, std::shared_ptr<ReaderBinding>>(m, "Reader")
      .def("close", &ReaderBinding::close);

  py::class_<ClientBinding, std::shared_ptr<ClientBinding>>(m, "Client")
      .def("send_request", &ClientBinding::send_request)
      .def("close", &ClientBinding::close);

  py::class_<ServiceBinding, std::shared_ptr<ServiceBinding>>(m, "Service")
      .def("close", &ServiceBinding::close);

  py::class_<NodeBinding, std::shared_ptr<NodeBinding>>(m, "Node")
      .def(py::init<const std::string&>())
      .def("close", &NodeBinding::close)
      .def("register_message", &NodeBinding::register_message)
      .def("create_writer", &NodeBinding::create_writer, py::arg("channel_name"),
           py::arg("data_type"), py::arg("qos_depth") = 1)
      .def("create_reader", &NodeBinding::create_reader,
           py::arg("channel_name"), py::arg("raw_data"), py::arg("callback"))
      .def("create_client", &NodeBinding::create_client,
           py::arg("service_name"), py::arg("request_type"))
      .def("create_service", &NodeBinding::create_service,
           py::arg("service_name"), py::arg("response_type"),
           py::arg("callback"));
}

}  // namespace cyber
}  // namespace apollo
