#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "beatsaber-hook/shared/rapidjson/include/rapidjson/document.h"
#include "ppv4.hpp"

namespace Qounters::PPv4 {
    namespace {
        using Json = rapidjson::Value;
        Json const* Member(Json const& j, std::string const& key) {
            auto it = j.FindMember(key.c_str());
            return it == j.MemberEnd() ? nullptr : &it->value;
        }
        double Number(Json const& j, std::string const& key, bool required = false) {
            auto p = Member(j, key);
            if (!p) {
                if (required)
                    throw std::runtime_error("Missing " + key);
                return 0;
            }
            if (p->IsNull())
                return 0;
            if (!p->IsNumber() || !std::isfinite(p->GetDouble()))
                throw std::runtime_error("Invalid " + key);
            return p->GetDouble();
        }
        std::string String(Json const& j, char const* key) {
            auto p = Member(j, key);
            if (!p || !p->IsString())
                throw std::runtime_error(std::string("Missing ") + key);
            return {p->GetString(), p->GetStringLength()};
        }
        Ratings ReadRatings(Json const& j, std::string prefix = "") {
            auto key = [&](std::string name) {
                if (!prefix.empty())
                    name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
                return prefix + name;
            };
            Ratings r;
            r.acc = Number(j, key("accRating"), true);
            r.pass = Number(j, key("passRating"), true);
            r.tech = Number(j, key("techRating"), true);
            r.predicted = Number(j, key("predictedAcc"), true);
            r.sliders = Number(j, key("sliderPercentage"), true);
            auto p = Member(j, key("stars"));
            if (p && !p->IsNull())
                r.reportedStars = Number(j, key("stars"));
            if (r.acc < 0 || r.pass < 0 || r.tech < 0)
                throw std::runtime_error("Negative ratings");
            return r;
        }
    }  // namespace
    Difficulty ParseDifficulty(std::string const& json, bool wrapped) {
        rapidjson::Document doc;
        doc.Parse(json.c_str(), json.size());
        if (doc.HasParseError() || !doc.IsObject())
            throw std::runtime_error("Invalid JSON");
        auto const* obj = wrapped ? Member(doc, "difficulty") : &doc;
        if (!obj || !obj->IsObject())
            throw std::runtime_error("Missing difficulty");
        auto const& j = *obj;
        Difficulty d;
        d.name = String(j, "difficultyName");
        d.mode = String(j, "modeName");
        auto status = Member(j, "status");
        if (!status)
            throw std::runtime_error("Missing status");
        if (status->IsInt())
            d.status = status->GetInt();
        else if (status->IsString()) {
            std::string s = status->GetString();
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
            std::map<std::string, int> const names = {
                {"unranked", 0}, {"nominated", 1}, {"qualified", 2}, {"ranked", 3}, {"unrankable", 4}, {"outdated", 5}, {"inevent", 6}, {"ost", 7}
            };
            if (names.contains(s))
                d.status = names.at(s);
            else if (s.size() == 1 && s[0] >= '0' && s[0] <= '7')
                d.status = s[0] - '0';
            else
                throw std::runtime_error("Invalid status");
        } else
            throw std::runtime_error("Invalid status");
        // Unranked maps often have no ratings; status is authoritative even if stars are present.
        if (!d.Eligible())
            return d;
        d.base = ReadRatings(j);
        auto notes = Member(j, "notes"), maxScore = Member(j, "maxScore");
        if (!notes || !notes->IsInt() || notes->GetInt() < 0)
            throw std::runtime_error("Invalid notes");
        if (!maxScore || !maxScore->IsInt() || maxScore->GetInt() <= 0)
            throw std::runtime_error("Invalid maxScore");
        d.notes = notes->GetInt();
        d.maxScore = maxScore->GetInt();
        auto span = Member(j, "noteSpan");
        if (span && !span->IsNull()) {
            d.noteSpan = Number(j, "noteSpan");
            if (*d.noteSpan < 0)
                throw std::runtime_error("Invalid noteSpan");
        }
        auto mv = Member(j, "modifierValues");
        if (mv && !mv->IsNull()) {
            if (!mv->IsObject())
                throw std::runtime_error("Invalid modifierValues");
            for (auto it = mv->MemberBegin(); it != mv->MemberEnd(); ++it) {
                std::string name = it->name.GetString();
                if (name != "modifierId")
                    d.modifierValues[name] = Number(*mv, name);
            }
        }
        auto mr = Member(j, "modifiersRating");
        if (mr && !mr->IsNull()) {
            if (!mr->IsObject())
                throw std::runtime_error("Invalid modifiersRating");
            d.hasSpeedRatings = true;
            for (auto key : {"ss", "fs", "sf"}) {
                auto p = Member(*mr, std::string(key) + "AccRating");
                if (p && !p->IsNull())
                    d.speeds.emplace(key, ReadRatings(*mr, key));
            }
        }
        return d;
    }
}  // namespace Qounters::PPv4
