// Copyright 2026 WheelOS. All Rights Reserved.
// Licensed under the Apache License, Version 2.0.

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>

#include "cyber/component/component.h"
#include "cyber/component/timer_component.h"

class InstalledE2EComponent final : public apollo::cyber::TimerComponent {
 public:
  bool Init() override {
    const char* marker = std::getenv("WHEELOS_E2E_COMPONENT_MARKER");
    if (marker == nullptr) {
      return false;
    }
    marker_ = marker;
    std::ofstream output(marker_);
    return output.good();
  }

  bool Proc() override {
    std::ofstream output(marker_, std::ios::app);
    output << ++sequence_ << '\n';
    return output.good();
  }

 private:
  std::string marker_;
  uint64_t sequence_ = 0;
};

CYBER_REGISTER_COMPONENT(InstalledE2EComponent)
