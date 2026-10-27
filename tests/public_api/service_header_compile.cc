#include "cyber/proto/parameter.pb.h"

#include "cyber/service/service.h"

namespace apollo {
namespace cyber {

bool CompilePublicServiceHeader() {
  Service<proto::ParamName, proto::ParamName> service(
      "server", "/service",
      [](const std::shared_ptr<proto::ParamName>& request,
         std::shared_ptr<proto::ParamName>& response) {
        *response = *request;
      });
  return service.Init();
}

}  // namespace cyber
}  // namespace apollo
