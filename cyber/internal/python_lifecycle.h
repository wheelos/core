#ifndef CYBER_INTERNAL_PYTHON_LIFECYCLE_H_
#define CYBER_INTERNAL_PYTHON_LIFECYCLE_H_

namespace apollo {
namespace cyber {

void ClearFromPython();
void ClearForPythonExit();
void EnablePythonExitHandling();

}  // namespace cyber
}  // namespace apollo

#endif  // CYBER_INTERNAL_PYTHON_LIFECYCLE_H_
