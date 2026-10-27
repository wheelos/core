#include "module_state.h"

#include <atomic>
#include <thread>

#include "callback_lifecycle.h"

#include "cyber/common/log.h"
#include "cyber/cyber.h"
#include "cyber/init.h"
#include "cyber/internal/python_lifecycle.h"

namespace apollo {
namespace cyber {
namespace {

std::atomic<bool>& PythonShutdownRequested() {
  static std::atomic<bool> requested(false);
  return requested;
}

std::atomic<bool>& DeferredShutdownInFlight() {
  static std::atomic<bool> in_flight(false);
  return in_flight;
}

void WaitForPythonShutdownQuiescence() {
  // Full shutdown must wait for both Python callbacks and any
  // callback-triggered deferred close tails before tearing down
  // transport/topology singletons.
  WaitForPythonCallbacksToDrain();
  WaitForDetachedCloseTasksToDrain();
}

void CompletePythonShutdown(bool for_python_exit) {
  if (PyGILState_Check()) {
    py::gil_scoped_release release;
    WaitForPythonShutdownQuiescence();
    if (for_python_exit) {
      ClearForPythonExit();
    } else {
      ClearFromPython();
    }
    return;
  }

  WaitForPythonShutdownQuiescence();
  if (for_python_exit) {
    ClearForPythonExit();
  } else {
    ClearFromPython();
  }
}

void RequestShutdown(bool for_python_exit) {
  if (GetState() == STATE_UNINITIALIZED || GetState() == STATE_SHUTDOWN) {
    return;
  }
  PythonShutdownRequested().store(true, std::memory_order_release);
  if (InPythonCallbackOnCurrentThread()) {
    bool expected = false;
    if (DeferredShutdownInFlight().compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
      std::thread([for_python_exit] {
        CompletePythonShutdown(for_python_exit);
        DeferredShutdownInFlight().store(false, std::memory_order_release);
      }).detach();
    }
    return;
  }
  CompletePythonShutdown(for_python_exit);
}

}  // namespace

bool CallbacksSuppressed() {
  return PythonShutdownRequested().load(std::memory_order_acquire) ||
         GetState() == STATE_SHUTTING_DOWN || GetState() == STATE_SHUTDOWN;
}

bool InitBinding(const std::string& module_name) {
  EnablePythonExitHandling();

  const auto state = GetState();
  if (state == STATE_INITIALIZED) {
    PythonShutdownRequested().store(false, std::memory_order_release);
    AINFO << "cyber already inited.";
    return true;
  }
  if (state == STATE_SHUTTING_DOWN || state == STATE_SHUTDOWN) {
    AERROR << "cyber cannot be re-initialized after shutdown in the same "
              "process.";
    return false;
  }
  if (!Init(module_name.c_str())) {
    AERROR << "cyber::Init failed:" << module_name;
    return false;
  }
  PythonShutdownRequested().store(false, std::memory_order_release);
  return true;
}

void ShutdownBinding() { RequestShutdown(false); }

void ShutdownForPythonExitBinding() { RequestShutdown(true); }

bool OkBinding() { return OK(); }

bool IsShutdownBinding() { return IsShutdown(); }

void WaitForShutdownBinding() { WaitForShutdown(); }

}  // namespace cyber
}  // namespace apollo
