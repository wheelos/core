// Copyright 2026 WheelOS. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.

#pragma once

#include <memory>
#include <string>

#include "cyber/component/component.h"
#include "examples/proto/examples.pb.h"

class RecorderInputComponent
    : public apollo::cyber::Component<
          apollo::cyber::examples::proto::Chatter> {
 public:
  bool Init() override;

 private:
  bool Proc(const std::shared_ptr<
            apollo::cyber::examples::proto::Chatter>& message) override;

  std::string token_;
  std::shared_ptr<apollo::cyber::Writer<
      apollo::cyber::examples::proto::Chatter>>
      writer_;
};

CYBER_REGISTER_COMPONENT(RecorderInputComponent)
