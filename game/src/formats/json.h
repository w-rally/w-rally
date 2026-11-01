#pragma once
// wrally::formats::Json - a small JSON reader for W-Rally's own manifests (car.json, track-<n>.json): the whole value
// tree in memory, objects keeping their keys' order. Readers only (decision: the game reads, tools write).

#include <string>
#include <utility>
#include <vector>

namespace wrally::formats {

struct Json {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool boolean = false;
    double number = 0;
    std::string text;                                   // String
    std::vector<Json> items;                            // Array
    std::vector<std::pair<std::string, Json>> members;  // Object

    // An object's member (null: none, or not an object).
    const Json* Get(const std::string& key) const;
    // A member's value as that type, else `fallback` (missing, or another type).
    std::string Str(const std::string& key, const std::string& fallback = "") const;
    double Num(const std::string& key, double fallback = 0) const;
};

// The text as one value. Throws std::runtime_error with the line on bad JSON.
Json ParseJson(const std::string& text);
// A file's: "" in `error` and the value, or what was wrong (no file, bad JSON) and a null.
Json ReadJsonFile(const std::string& path, std::string& error);

} // namespace wrally::formats
