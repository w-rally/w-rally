#include "wrally/formats/json.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace wrally::formats {

namespace {

struct Reader {
    const std::string& s;
    size_t at = 0;

    [[noreturn]] void Fail(const char* what) const
    {
        size_t line = 1;
        for (size_t i = 0; i < at && i < s.size(); ++i) line += s[i] == '\n';
        throw std::runtime_error(std::string(what) + " (line " + std::to_string(line) + ")");
    }
    void Space()
    {
        while (at < s.size() && (s[at] == ' ' || s[at] == '\t' || s[at] == '\r' || s[at] == '\n')) ++at;
    }
    bool Eat(char c)
    {
        Space();
        if (at < s.size() && s[at] == c) return ++at, true;
        return false;
    }
    void Expect(const char* word)
    {
        for (const char* p = word; *p; ++p, ++at)
            if (at >= s.size() || s[at] != *p) Fail("unexpected text");
    }
    // A \uXXXX's code unit.
    unsigned Hex4()
    {
        if (at + 4 > s.size()) Fail("a short \\u escape");
        const unsigned v = static_cast<unsigned>(std::strtoul(s.substr(at, 4).c_str(), nullptr, 16));
        at += 4;
        return v;
    }
    void Utf8(unsigned c, std::string& out)
    {
        if (c < 0x80) out += static_cast<char>(c);
        else if (c < 0x800) out += static_cast<char>(0xc0 | c >> 6), out += static_cast<char>(0x80 | (c & 0x3f));
        else if (c < 0x10000)
            out += static_cast<char>(0xe0 | c >> 12), out += static_cast<char>(0x80 | (c >> 6 & 0x3f)), out += static_cast<char>(0x80 | (c & 0x3f));
        else
            out += static_cast<char>(0xf0 | c >> 18), out += static_cast<char>(0x80 | (c >> 12 & 0x3f)),
                out += static_cast<char>(0x80 | (c >> 6 & 0x3f)), out += static_cast<char>(0x80 | (c & 0x3f));
    }
    std::string String()
    {
        if (!Eat('"')) Fail("a string expected");
        std::string out;
        while (at < s.size() && s[at] != '"') {
            const char c = s[at++];
            if (c != '\\') {
                out += c;
                continue;
            }
            if (at >= s.size()) break;
            switch (const char e = s[at++]) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                unsigned c16 = Hex4();
                if (c16 >= 0xd800 && c16 < 0xdc00 && at + 6 <= s.size() && s[at] == '\\' && s[at + 1] == 'u') {
                    at += 2;
                    c16 = 0x10000 + ((c16 - 0xd800) << 10) + (Hex4() - 0xdc00);
                }
                Utf8(c16, out);
                break;
            }
            default: out += e;   // \" \\ \/
            }
        }
        if (at >= s.size()) Fail("an unclosed string");
        ++at;
        return out;
    }
    Json Value(int depth)
    {
        if (depth > 64) Fail("nested too deep");
        Space();
        if (at >= s.size()) Fail("a value expected");
        Json v;
        const char c = s[at];
        if (c == '{') {
            ++at;
            v.type = Json::Object;
            if (Eat('}')) return v;
            do {
                std::string key = String();
                if (!Eat(':')) Fail("':' expected");
                v.members.emplace_back(std::move(key), Value(depth + 1));
            } while (Eat(','));
            if (!Eat('}')) Fail("'}' expected");
        } else if (c == '[') {
            ++at;
            v.type = Json::Array;
            if (Eat(']')) return v;
            do v.items.push_back(Value(depth + 1));
            while (Eat(','));
            if (!Eat(']')) Fail("']' expected");
        } else if (c == '"') {
            v.type = Json::String, v.text = String();
        } else if (c == 't') {
            Expect("true"), v.type = Json::Bool, v.boolean = true;
        } else if (c == 'f') {
            Expect("false"), v.type = Json::Bool;
        } else if (c == 'n') {
            Expect("null");
        } else {
            char* end = nullptr;
            v.number = std::strtod(s.c_str() + at, &end);
            if (end == s.c_str() + at) Fail("a value expected");
            v.type = Json::Number;
            at = static_cast<size_t>(end - s.c_str());
        }
        return v;
    }
};

} // namespace

const Json* Json::Get(const std::string& key) const
{
    if (type != Object) return nullptr;
    for (const auto& [k, v] : members)
        if (k == key) return &v;
    return nullptr;
}

std::string Json::Str(const std::string& key, const std::string& fallback) const
{
    const Json* v = Get(key);
    return v && v->type == String ? v->text : fallback;
}

double Json::Num(const std::string& key, double fallback) const
{
    const Json* v = Get(key);
    return v && v->type == Number ? v->number : fallback;
}

Json ParseJson(const std::string& text)
{
    Reader r{text};
    if (text.size() >= 3 && !text.compare(0, 3, "\xEF\xBB\xBF")) r.at = 3;   // (a UTF-8 BOM)
    Json v = r.Value(0);
    r.Space();
    if (r.at != text.size()) r.Fail("text after the value");
    return v;
}

Json ReadJsonFile(const std::string& path, std::string& error)
{
    error.clear();
    std::ifstream f(path, std::ios::binary);
    if (!f) return error = "no file", Json{};
    std::stringstream ss;
    ss << f.rdbuf();
    try {
        return ParseJson(ss.str());
    } catch (const std::exception& e) {
        error = e.what();
        return {};
    }
}

} // namespace wrally::formats
