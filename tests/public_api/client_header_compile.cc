#include "cyber/proto/parameter.pb.h"

#include "cyber/service/client.h"

namespace apollo {
namespace cyber {

bool CompilePublicClientHeader() {
  Client<proto::ParamName, proto::ParamName> client("client", "/service");
  return client.Init() && client.WaitForService(std::chrono::milliseconds(1));
}

}  // namespace cyber
}  // namespace apollo
