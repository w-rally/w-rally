// yaml-check: wrally::formats::ParseYaml (libs/json) - the YAML subset W-Rally's manifests are written in: what it
// reads, what every value is (text: "1.10" stays "1.10"), and the errors a modder gets, with their lines.
//
//   yaml-check
//
// Prints "yaml-check: all good", or each check that failed and exits 1.

#include <cstdio>
#include <stdexcept>
#include <string>

#include "wrally/formats/json.h"

using wrally::formats::Json;
using wrally::formats::ParseYaml;

namespace {

int g_failed = 0;

void Check(bool ok, const std::string& what)
{
    if (!ok) std::printf("FAILED: %s\n", what.c_str()), ++g_failed;
}

// The error ParseYaml throws, "" none.
std::string Error(const std::string& text)
{
    try {
        ParseYaml(text);
    } catch (const std::exception& e) {
        return e.what();
    }
    return "";
}

bool Has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

} // namespace

int main()
{
    // a car manifest as a modder writes it
    const Json car = ParseYaml("\xEF\xBB\xBF# my car\n"
                               "---\n"
                               "id: i20_r5\n"
                               "version: 1.10\n"
                               "name: Hyundai i20 R5   # the menu's\n"
                               "year: 2016\n"
                               "power_hp: 280.5\n"
                               "country: no\n"
                               "splash: Textures\\Splash\\load_23.dds\n"
                               "url: http://example.com/a#b\n"
                               "note: 'it''s # not a comment'\n"
                               "quoted: \"tab\\there \\u00e9\"\n"
                               "empty:\n"
                               "tilde: ~\n"
                               "\n"
                               "lamps:\n"
                               "  - kind: halogen-low\n"
                               "    at: [0.5, -0.2, 1.8]\n"
                               "  - kind: led-bar\n"
                               "    at: {x: 0, y: 1.2}\n"
                               "tags:\n"
                               "- gravel\n"
                               "- \"tarmac\"\n"
                               "-\n"
                               "  nested: yes\n"
                               "about: |\n"
                               "  line one\n"
                               "    indented\n"
                               "\n"
                               "  last\n"
                               "folded: >-\n"
                               "  one\n"
                               "  two\n"
                               "\n"
                               "  three\n"
                               "long: a plain value\n"
                               "  going on\n"
                               "...\n");
    Check(car.type == Json::Object, "a mapping");
    Check(car.Str("id") == "i20_r5", "plain text");
    Check(car.Str("version") == "1.10" && car.Num("version") == 1.1, "1.10 stays text, Num reads it");
    Check(car.Str("name") == "Hyundai i20 R5", "a comment cut, trailing spaces too: [" + car.Str("name") + "]");
    Check(car.Num("year") == 2016 && car.Num("power_hp") == 280.5, "numbers through Num");
    Check(car.Str("country") == "no", "no is text, not false");
    Check(car.Str("splash") == "Textures\\Splash\\load_23.dds", "backslashes kept in a plain value");
    Check(car.Str("url") == "http://example.com/a#b", "a ':' and '#' inside a plain value");
    Check(car.Str("note") == "it's # not a comment", "'' quoting: [" + car.Str("note") + "]");
    Check(car.Str("quoted") == "tab\there \xC3\xA9", "\"\" escapes");
    Check(car.Get("empty") && car.Get("empty")->type == Json::Null && car.Get("tilde")->type == Json::Null, "nulls");
    Check(car.Num("name", -1) == -1 && car.Num("missing", 7) == 7, "Num's fallback");

    const Json* lamps = car.Get("lamps");
    Check(lamps && lamps->type == Json::Array && lamps->items.size() == 2, "a list of mappings");
    if (lamps && lamps->items.size() == 2) {
        const Json* at = lamps->items[0].Get("at");
        Check(lamps->items[0].Str("kind") == "halogen-low" && at && at->items.size() == 3 && at->items[1].text == "-0.2",
              "a flow list in a list item");
        Check(lamps->items[1].Get("at") && lamps->items[1].Get("at")->Num("y") == 1.2, "a flow mapping");
    }
    const Json* tags = car.Get("tags");
    Check(tags && tags->items.size() == 3 && tags->items[0].text == "gravel" && tags->items[1].text == "tarmac" &&
              tags->items[2].Str("nested") == "yes",
          "a list at its key's indent, an item on the next line");
    Check(car.Str("about") == "line one\n  indented\n\nlast\n", "| block: [" + car.Str("about") + "]");
    Check(car.Str("folded") == "one two\nthree", ">- block: [" + car.Str("folded") + "]");
    Check(car.Str("long") == "a plain value going on", "a plain value over two lines");
    Check(car.members[0].first == "id" && car.members.back().first == "long", "keys in their order");

    // a JSON text is YAML too (the manifests made in memory, an older manifest renamed)
    const Json json = ParseYaml("{\"id\": \"rbr\", \"rbr\": 41,\n \"list\": [1, \"two\", {\"a\": null}], \"t\": true}");
    Check(json.Str("id") == "rbr" && json.Num("rbr") == 41 && json.Get("list")->items.size() == 3 &&
              json.Get("list")->items[2].Get("a")->type == Json::Null && json.Str("t") == "true",
          "JSON as YAML");
    Check(ParseYaml("41: cz_ploskovice\n4: fi_x\n").Str("41") == "cz_ploskovice", "number keys are text");
    Check(ParseYaml("").type == Json::Null && ParseYaml("# nothing\n").type == Json::Null, "an empty document");
    Check(ParseYaml("just text").text == "just text", "a scalar document");

    // the errors, each with its line
    Check(Has(Error("a: 1\n\tb: 2\n"), "tab") && Has(Error("a: 1\n\tb: 2\n"), "line 2"), "a tab in the indentation");
    Check(Has(Error("a: 1\n   b: 2\n"), "bad indentation (line 2)"), "a deeper line after a value");
    Check(Has(Error("a:\n  b: 1\n c: 2\n"), "bad indentation (line 3)"), "an indent between two levels");
    Check(Has(Error("a: 1\na: 2\n"), "twice (line 2)"), "a key twice");
    Check(Has(Error("a: b: c\n"), "quote it"), "a ': ' in a plain value");
    Check(Has(Error("a: &x 1\n"), "anchors"), "anchors refused");
    Check(Has(Error("a: \"open\n"), "unclosed"), "an unclosed string");
    Check(Has(Error("a: [1, 2\n"), "expected"), "an unclosed flow list");
    Check(Has(Error("a: 1\n---\nb: 2\n"), "several documents"), "one document");
    Check(Has(Error("- a\nb: 1\n"), "bad indentation") || Has(Error("- a\nb: 1\n"), "line 2"), "a key after a list");
    Check(Has(Error("a: [1] x\n"), "text after the value"), "text after a flow value");

    if (g_failed) {
        std::printf("yaml-check: %d failed\n", g_failed);
        return 1;
    }
    std::printf("yaml-check: all good\n");
    return 0;
}
