#include "pp.hpp"

#include "GlobalNamespace/BeatmapCharacteristicSO.hpp"
#include "GlobalNamespace/BeatmapDifficulty.hpp"
#include "GlobalNamespace/BeatmapDifficultySerializedMethods.hpp"
#include "System/Action_1.hpp"
#include "beatleader.hpp"
#include "bsml/shared/BSML/MainThreadScheduler.hpp"
#include "custom-types/shared/delegate.hpp"
#include "environment.hpp"
#include "events.hpp"
#include "main.hpp"
#include "metacore/shared/events.hpp"
#include "metacore/shared/songs.hpp"

using namespace Qounters;
using namespace GlobalNamespace;

static uint64_t requestGeneration = 0;

bool PP::blSongValid = false;
MetaCore::PP::BLSongDiff PP::latestBeatleaderSong;
bool PP::ssSongValid = false;
MetaCore::PP::SSSongDiff PP::latestScoresaberSong;

bool PP::IsRankedBL() {
    if (Environment::InSettings())
        return blSongValid;
    return BeatLeader::IsRanked();
}

bool PP::IsRankedSS() {
    if (Environment::InSettings())
        return ssSongValid;
    return ssSongValid && latestScoresaberSong > 0;
}

float PP::CalculateBL(float percentage, GameplayModifiers* modifiers, bool failed) {
    if (!Environment::InSettings())
        return BeatLeader::Calculate(percentage, modifiers, failed);
    if (!blSongValid)
        return 0;

    MetaCore::PP::BLSongDiff customDiff = {};
    customDiff.Pass = 0.8 * settingsStarsBL;
    customDiff.Acc = settingsStarsBL;
    customDiff.Tech = 0.5 * settingsStarsBL;
    return MetaCore::PP::Calculate(customDiff, percentage, modifiers, failed);
}

float PP::CalculateSS(float percentage, GameplayModifiers* modifiers, bool failed) {
    if (!ssSongValid)
        return 0;
    return MetaCore::PP::Calculate(Environment::InSettings() ? settingsStarsSS : latestScoresaberSong, percentage, modifiers, failed);
}

static void OnMapInfo(std::optional<MetaCore::PP::BLSongDiff> bl, std::optional<MetaCore::PP::SSSongDiff> ss) {
    if (bl.has_value()) {
        PP::latestBeatleaderSong = *bl;
        PP::blSongValid = true;
    }
    if (ss.has_value()) {
        PP::latestScoresaberSong = *ss;
        PP::ssSongValid = true;
    }
    Events::BroadcastQountersEvent(Events::MapInfo);
}

void PP::GetMapInfo(BeatmapKey map) {
    if (Environment::InSettings())
        return;
    if (!map.IsValid()) {
        Reset();
        return;
    }

    blSongValid = false;
    ssSongValid = false;
    auto ticket = ++requestGeneration;
    BeatLeader::GetMapInfo(map);
    Events::BroadcastQountersEvent(Events::MapInfo);

    MetaCore::PP::GetMapInfo(map, [ticket](std::optional<MetaCore::PP::BLSongDiff> bl, std::optional<MetaCore::PP::SSSongDiff> ss) {
        BSML::MainThreadScheduler::Schedule([ticket, bl = std::move(bl), ss]() {
            if (ticket == requestGeneration && !Environment::InSettings())
                OnMapInfo(bl, ss);
        });
    });
}

void PP::Reset() {
    blSongValid = false;
    ssSongValid = false;
    ++requestGeneration;
    BeatLeader::Reset();
    Events::BroadcastQountersEvent(Events::MapInfo);
}

ON_EVENT(MetaCore::Events::MapSelected) {
    PP::GetMapInfo(MetaCore::Songs::GetSelectedKey());
}

ON_EVENT(MetaCore::Events::MapDeselected) {
    if (!Environment::InSettings())
        PP::Reset();
}

ON_EVENT(MetaCore::Events::SoftRestart) {
    PP::Reset();
}
