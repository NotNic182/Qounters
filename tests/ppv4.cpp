#include "ppv4.hpp"

#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>

#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

using namespace Qounters::PPv4;
std::string Serialize(rapidjson::Value const& value) {
    rapidjson::StringBuffer s;
    rapidjson::Writer<rapidjson::StringBuffer> w(s);
    value.Accept(w);
    return s.GetString();
}
void Check(bool test, std::string const& description) {
    if (!test)
        throw std::runtime_error(description);
}
std::vector<std::string> Mods(std::string s) {
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::stringstream in(s);
    std::vector<std::string> result;
    std::string code;
    while (std::getline(in, code, ','))
        if (!code.empty())
            result.push_back(code);
    return result;
}
int main(int argc, char** argv) try {
    Check(argc == 2, "Usage: vectors fixtures.json");
    std::ifstream input(argv[1]);
    Check(input.good(), "Fixture missing");
    std::string json((std::istreambuf_iterator<char>(input)), {});
    rapidjson::Document doc;
    doc.Parse(json.c_str());
    Check(!doc.HasParseError(), "Fixture JSON invalid");
    std::map<std::string, Difficulty> maps;
    double maxStars = 0, maxPp = 0;
    int stars = 0, vectors = 0;
    for (auto const& j : doc["beatleader"]["leaderboards"].GetArray()) {
        auto d = ParseDifficulty(Serialize(j), false);
        maps.emplace(Serialize(j["leaderboardId"]), d);
        auto c = Prepare(d, {});
        maxStars = std::max(maxStars, std::abs(c.stars - *d.base.reportedStars));
        ++stars;
        for (auto code : {"ss", "fs", "sf"}) {
            c = Prepare(d, {code});
            maxStars = std::max(maxStars, std::abs(c.stars - *d.speeds.at(code).reportedStars));
            ++stars;
        }
    }
    for (auto const& j : doc["beatleader"]["vectors"].GetArray()) {
        auto d = maps.at(Serialize(j["leaderboardId"]));
        auto mods = Mods(j["modifiers"].GetString());
        auto c = Prepare(d, mods);
        auto r = c.Evaluate(j["accuracy"].GetDouble());
        auto const& e = j["expected"];
        for (auto [actual, key] : {std::pair{r.total, "pp"}, {r.pass, "passPP"}, {r.acc, "accPP"}, {r.tech, "techPP"}}) {
            double error = std::abs(actual - e[key].GetDouble());
            maxPp = std::max(maxPp, error);
            Check(error <= .01, "PP vector mismatch: " + Serialize(j["scoreId"]) + " " + key);
        }
        double fcError = std::abs(c.Evaluate(j["fcAccuracy"].GetDouble()).total - j["expectedFcPp"].GetDouble());
        maxPp = std::max(maxPp, fcError);
        Check(fcError <= .01, "FC PP mismatch");
        ++vectors;
    }
    Check(maxStars < .0001, "Stars mismatch");
    auto d = maps.begin()->second;
    auto c = Prepare(d, {});
    for (double x : {0., -1., 1.01, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()})
        Check(c.Evaluate(x).total == 0, "Invalid accuracy");
    Check(Prepare(d, {"nf"}).Evaluate(.95).total == 0, "No Fail must be zero");
    Check(Prepare(d, {"gn", "gn"}).Evaluate(.95).total == Prepare(d, {"gn"}).Evaluate(.95).total, "Duplicate mods");
    auto fs = Prepare(d, {"fs"});
    d.modifierValues["fs"] = 1000;
    Check(fs.Evaluate(.95).total == Prepare(d, {"fs"}).Evaluate(.95).total, "Speed double applied");
    d.noteSpan.reset();
    bool threw = false;
    try {
        Prepare(d, {});
    } catch (std::exception const&) {
        threw = true;
    }
    Check(threw, "Missing v4 data must not silently calculate");
    for (int status : {0, 1, 4, 5, 7}) {
        d.status = status;
        Check(!d.Eligible(), "Unranked status");
    }
    for (int status : {2, 3, 6}) {
        d.status = status;
        Check(d.Eligible(), "Eligible status");
    }
    Check(EncodeSegment("90Degree /a") == "90Degree%20%2Fa", "Path encoding");
    for (std::string bad : {"null", "[]", "{}", "{\"difficulty\":null}", "{bad"}) {
        threw = false;
        try {
            ParseDifficulty(bad);
        } catch (std::exception const&) {
            threw = true;
        }
        Check(threw, "Malformed response accepted");
    }
    auto unranked = ParseDifficulty(R"({"difficulty":{"difficultyName":"ExpertPlus","modeName":"Standard","status":0,"stars":99}})");
    Check(!unranked.Eligible(), "Unranked positive stars");
    std::cout << "PASS: " << vectors << " BeatLeader vectors + " << stars << " star values + edge cases. Max PP error " << maxPp
              << ", max star error " << maxStars << "\n";
    return 0;
} catch (std::exception const& e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
}
