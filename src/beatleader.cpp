#include "beatleader.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <memory>
#include <stdexcept>

#include "GlobalNamespace/BeatmapCharacteristicSO.hpp"
#include "GlobalNamespace/BeatmapDifficultySerializedMethods.hpp"
#include "UnityEngine/Networking/DownloadHandler.hpp"
#include "UnityEngine/Networking/UnityWebRequest.hpp"
#include "UnityEngine/Networking/UnityWebRequestAsyncOperation.hpp"
#include "UnityEngine/WaitForSecondsRealtime.hpp"
#include "bsml/shared/BSML/SharedCoroutineStarter.hpp"
#include "environment.hpp"
#include "events.hpp"
#include "main.hpp"
#include "metacore/shared/events.hpp"
#include "metacore/shared/internals.hpp"
#include "metacore/shared/pp.hpp"
#include "metacore/shared/songs.hpp"
#include "metacore/shared/strings.hpp"
#include "ppv4.hpp"

using namespace Qounters;
using namespace GlobalNamespace;

namespace {
    namespace I = MetaCore::Internals;
    using WebRequest = UnityEngine::Networking::UnityWebRequest;
    using Clock = std::chrono::steady_clock;
    enum class State { Empty, Loading, Ready, Unranked, NotFound, NetworkError, ParseError, MissingV4 };
    struct Entry {
        PPv4::Difficulty difficulty;
        Clock::time_point fetched;
    };
    std::map<std::string, Entry> cache;
    State state = State::Empty;
    std::string currentKey;
    uint64_t generation = 0;
    std::optional<PPv4::Difficulty> difficulty;
    std::optional<PPv4::Context> context;
    std::vector<std::string> contextMods;
    bool failed = false, ended = false;

    void Notify() {
        Qounters::Events::BroadcastQountersEvent(Qounters::Events::MapInfo);
    }
    bool Eligible() {
        return difficulty && difficulty->Eligible();
    }
    void Accept(PPv4::Difficulty d) {
        difficulty = std::move(d);
        context.reset();
        state = !difficulty->Eligible() ? State::Unranked : !difficulty->noteSpan ? State::MissingV4 : State::Ready;
    }
    void Clear() {
        ++generation;
        currentKey.clear();
        difficulty.reset();
        context.reset();
        state = State::Empty;
        failed = false;
        ended = false;
    }
    // Unity objects and all shared state stay on Unity's main thread. Requests are asynchronous.
    custom_types::Helpers::Coroutine Fetch(std::string key, std::string diffName, std::string mode, uint64_t ticket) {
        // Collapse rapid menu scrolling into one request for the final selection.
        co_yield reinterpret_cast<System::Collections::IEnumerator*>(UnityEngine::WaitForSecondsRealtime::New_ctor(.25f));
        if (ticket != generation)
            co_return;
        auto* request = WebRequest::Get("https://api.beatleader.xyz/leaderboard/" + key);
        SafePtr<WebRequest> requestRoot(request);
        auto cleanup = std::unique_ptr<WebRequest, void (*)(WebRequest*)>(request, [](WebRequest* r) { r->Dispose(); });
        request->set_timeout(15);
        request->set_redirectLimit(3);
        auto* operation = request->SendWebRequest();
        SafePtr<UnityEngine::Networking::UnityWebRequestAsyncOperation> operationRoot(operation);
        // Polling the async operation permits cancellation immediately after a different map is selected.
        while (!operation->get_isDone()) {
            if (ticket != generation) {
                request->Abort();
                co_return;
            }
            co_yield nullptr;
        }
        if (ticket != generation)
            co_return;
        auto code = request->get_responseCode();
        if (code == 404)
            state = State::NotFound;
        else if (code != 200 || request->get_result() != WebRequest::Result::Success) {
            state = State::NetworkError;
            logger.warn("BeatLeader request failed HTTP {}", static_cast<long long>(code));
        } else {
            try {
                std::string json = request->get_downloadHandler()->get_text();
                auto d = PPv4::ParseDifficulty(json);
                if (d.name != diffName || d.mode != mode)
                    throw std::runtime_error("Response difficulty mismatch");
                if (cache.size() >= 32 && !cache.contains(key)) {
                    auto oldest = std::min_element(cache.begin(), cache.end(), [](auto const& a, auto const& b) {
                        return a.second.fetched < b.second.fetched;
                    });
                    cache.erase(oldest);
                }
                cache.insert_or_assign(key, Entry{d, Clock::now()});
                Accept(std::move(d));
                logger.debug("PPv4 map loaded: {} (status {})", key, difficulty->status);
            } catch (std::exception const& e) {
                state = State::ParseError;
                logger.error("BeatLeader parse error: {}", e.what());
            }
        }
        Notify();
    }
    void Select(BeatmapKey map) {
        if (Environment::InSettings())
            return;
        if (!map.IsValid()) {
            Clear();
            Notify();
            return;
        }
        std::string id = map.levelId, hash = MetaCore::Songs::GetHash(map);
        if (hash.size() != 40 || id.ends_with(" WIP") || !std::all_of(hash.begin(), hash.end(), [](unsigned char c) { return std::isxdigit(c); })) {
            Clear();
            Notify();
            return;
        }
        std::string name = BeatmapDifficultySerializedMethods::SerializedName(map.difficulty);
        std::string mode = map.beatmapCharacteristic->serializedName;
        std::string key = hash + "/" + PPv4::EncodeSegment(name) + "/" + PPv4::EncodeSegment(mode);
        // Repeated gameplay-start notifications do not restart a request or rebuild a curve.
        if (key == currentKey && (state == State::Loading || state == State::Ready || state == State::Unranked || state == State::MissingV4)) {
            auto it = cache.find(key);
            if (state == State::Loading || (it != cache.end() && Clock::now() - it->second.fetched < std::chrono::minutes(10)))
                return;
        }
        ++generation;
        currentKey = key;
        context.reset();
        difficulty.reset();
        state = State::Loading;
        auto it = cache.find(key);
        if (it != cache.end() && Clock::now() - it->second.fetched < std::chrono::minutes(10))
            Accept(it->second.difficulty);
        else
            BSML::SharedCoroutineStarter::StartCoroutine(Fetch(key, name, mode, generation));
        Notify();
    }
    void ObserveFailure() {
        // The didReach0Energy flag is latched by the game, including when No Fail health later recovers.
        if (I::health <= 0)
            failed = true;
        if (I::referencesValid && I::gameEnergyCounter && I::gameEnergyCounter->_didReach0Energy)
            failed = true;
    }
    bool PrepareContext(GameplayModifiers* modifiers, bool isFailed) {
        auto mods = MetaCore::PP::GetModStringsBL(modifiers, true, isFailed);
        if (!context || mods != contextMods) {
            try {
                context = PPv4::Prepare(*difficulty, mods);
                contextMods = mods;
                if (context->approximate)
                    logger.warn("PPv4 star check differs: {}, marking estimate approximate", context->stars);
            } catch (std::exception const& e) {
                logger.warn("Cannot prepare PPv4: {}", e.what());
                state = State::MissingV4;
                return false;
            }
        }
        return true;
    }
    std::string Text(int decimals) {
        switch (state) {
            case State::Empty:
                return "-- PP";
            case State::Loading:
                return "Loading PP...";
            case State::Unranked:
                return "Unranked";
            case State::NotFound:
                return "No leaderboard";
            case State::NetworkError:
                return "PP offline";
            case State::ParseError:
                return "PP data error";
            case State::MissingV4:
                return "PPv4 unavailable";
            default:
                break;
        }
        if (!I::modifiers || !difficulty)
            return "-- PP";
        ObserveFailure();
        if (failed && !I::modifiers->_noFailOn0Energy)
            return "Failed";
        int max = I::leftMaxScore + I::rightMaxScore;
        if (max <= 0)
            return "-- PP";
        if (!PrepareContext(I::modifiers, failed))
            return "PPv4 unavailable";
        // MetaCore sums cutScore*multiplier and maxPossibleCutScore*maxMultiplier as each scoring element resolves.
        // Neither includes gameplay-modifier score bonuses. At completion use the leaderboard's maxScore.
        double accuracy = double(I::leftScore + I::rightScore) / (ended ? difficulty->maxScore : max);
        if (accuracy < 0 || accuracy > 1)
            return "PP score mismatch";
        auto pp = context->Evaluate(accuracy).total;
        std::string result = (context->approximate ? "~" : "") + MetaCore::Strings::FormatDecimals(pp, std::clamp(decimals, 0, 6)) + " PP";
        if (difficulty->status == 2)
            result += " (Q)";
        if (difficulty->status == 6)
            result += " (E)";
        return result;
    }
}

bool BeatLeader::IsRanked() {
    return Eligible();
}

double BeatLeader::Calculate(double accuracy, GameplayModifiers* modifiers, bool isFailed) {
    if (state != State::Ready || !difficulty || !modifiers)
        return 0;
    ObserveFailure();
    isFailed |= failed;
    if (isFailed && !modifiers->_noFailOn0Energy)
        return 0;
    if (!PrepareContext(modifiers, isFailed))
        return 0;
    return context->Evaluate(accuracy).total;
}

std::string BeatLeader::GetText(int decimals) {
    return Text(decimals);
}

void BeatLeader::GetMapInfo(BeatmapKey map) {
    Select(map);
}

void BeatLeader::Reset() {
    Clear();
}

ON_EVENT(MetaCore::Events::GameplaySceneStarted) {
    if (Environment::InSettings())
        return;
    failed = false;
    ended = false;
    Select(I::beatmapKey);
    Notify();
}

ON_EVENT(MetaCore::Events::MapStarted) {
    Select(I::beatmapKey);
}

ON_EVENT(MetaCore::Events::HealthChanged) {
    if (!Environment::InSettings())
        ObserveFailure();
}

ON_EVENT(MetaCore::Events::MapEnded) {
    if (Environment::InSettings())
        return;
    ended = !I::mapWasQuit;
    Notify();
}

ON_EVENT(MetaCore::Events::SoftRestart) {
    Clear();
    cache.clear();
}
