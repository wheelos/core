#include "cyber/service/client_base.h"

#include <thread>

#include "cyber/service_discovery/topology_manager.h"

namespace apollo {
namespace cyber {

bool ClientBase::WaitForServiceNanoseconds(std::chrono::nanoseconds time_out) {
  bool has_service = false;
  auto step_duration = std::chrono::nanoseconds(5 * 1000 * 1000);
  while (time_out.count() > 0) {
    has_service = service_discovery::TopologyManager::Instance()
                      ->service_manager()
                      ->HasService(service_name_);
    if (!has_service) {
      std::this_thread::sleep_for(step_duration);
      time_out -= step_duration;
    } else {
      break;
    }
  }
  return has_service;
}

}  // namespace cyber
}  // namespace apollo
