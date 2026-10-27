#include <type_traits>

#include "cyber/tools/cyber_recorder/record/core/recorder.h"

static_assert(
    std::is_constructible_v<apollo::cyber::record::Recorder, const std::string&,
                            bool, const std::vector<std::string>&,
                            const std::vector<std::string>&>);
