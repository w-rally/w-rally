#pragma once
// wrally::formats::Json - small JSON and YAML readers for W-Rally's own manifests (car.yaml, track.yaml, physics.yaml;
// the older .json names too): the whole value tree in memory, objects keeping their keys' order. Readers only
// (decision: the game reads, tools write).
//
// YAML is what modders write (docs/content-layers.md): a subset - block mappings and lists by indentation (spaces
// only), flow [..] and {..} (a JSON text is YAML too), plain, '...' and "..." scalars, | and > blocks, # comments. No
// anchors, aliases, tags, complex keys or several documents. Every YAML scalar is a String - a plain null, ~ or nothing
// a Null - so "version: 1.10" stays "1.10" and "country: no" stays "no": the key says what its value is, and Num()
// reads a number from a String.

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
    // A member's value as that type, else `fallback` (missing, or another type). Num() reads a String that is all a
    // number too (YAML's scalars).
    std::string Str(const std::string& key, const std::string& fallback = "") const;
    double Num(const std::string& key, double fallback = 0) const;
};

// The text as one value. Throws std::runtime_error with the line on bad JSON.
Json ParseJson(const std::string& text);
// The text as one YAML value (the subset above). Throws std::runtime_error with the line on bad YAML.
Json ParseYaml(const std::string& text);
// By the file name's extension: .yaml or .yml YAML, else JSON.
bool IsYamlName(const std::string& name);
Json ParseData(const std::string& text, const std::string& name);
// A file's (by its extension): "" in `error` and the value, or what was wrong (no file, bad JSON or YAML) and a null.
Json ReadDataFile(const std::string& path, std::string& error);

} // namespace wrally::formats
