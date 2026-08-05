// Copyright 2026 WheelOS. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.

#include <iostream>
#include <string>

#include "cyber/record/record_reader.h"

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: record_play_channels_dump <record>" << std::endl;
    return 1;
  }

  apollo::cyber::record::RecordReader reader(argv[1]);
  if (!reader.IsValid()) {
    std::cerr << "invalid record: " << argv[1] << std::endl;
    return 1;
  }

  const auto channels = reader.GetChannelList();
  std::cout << "channels=" << channels.size() << std::endl;
  for (const auto& channel : channels) {
    std::cout << channel << "\t" << reader.GetMessageType(channel) << std::endl;
  }
  return 0;
}
