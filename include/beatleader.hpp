#pragma once

#include <string>

#include "GlobalNamespace/BeatmapKey.hpp"
#include "GlobalNamespace/GameplayModifiers.hpp"

namespace Qounters::BeatLeader {
    bool IsRanked();
    double Calculate(double accuracy, GlobalNamespace::GameplayModifiers* modifiers, bool failed);
    std::string GetText(int decimals);
    void GetMapInfo(GlobalNamespace::BeatmapKey map);
    void Reset();
}
