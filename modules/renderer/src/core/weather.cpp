#include "weather.h"
#include "log.h"
#include "../format/ngg_format.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <map>
#include <memory>
#include <vector>

namespace ngg {
namespace core {

namespace {

// ---- a minimal JSON reader: objects, arrays, numbers, strings, true/false/null.
// Enough for presets; deliberately not a general library.
struct Json
{
    enum Kind { kNull, kBool, kNumber, kString, kArray, kObject } kind = kNull;
    double number = 0.0;
    bool   boolean = false;
    std::string str;
    std::vector<Json> items;
    std::map<std::string, Json> fields;

    const Json* Get(const char* key) const
    {
        if (kind != kObject) return nullptr;
        auto it = fields.find(key);
        return it == fields.end() ? nullptr : &it->second;
    }
};

struct Parser
{
    const char* p;
    bool ok = true;

    void Ws() { while (*p && isspace((unsigned char)*p)) ++p; }
    bool Eat(char c) { Ws(); if (*p == c) { ++p; return true; } return false; }

    std::string String()
    {
        std::string s;
        if (*p != '"') { ok = false; return s; }
        ++p;
        while (*p && *p != '"')
        {
            if (*p == '\\' && p[1]) { ++p; s.push_back(*p == 'n' ? '\n' : *p == 't' ? '\t' : *p); ++p; }
            else s.push_back(*p++);
        }
        if (*p == '"') ++p; else ok = false;
        return s;
    }

    Json Value()
    {
        Json v;
        Ws();
        if (*p == '{')
        {
            ++p;
            v.kind = Json::kObject;
            if (Eat('}')) return v;
            do
            {
                Ws();
                std::string key = String();
                if (!Eat(':')) { ok = false; return v; }
                v.fields[key] = Value();
            } while (ok && Eat(','));
            if (!Eat('}')) ok = false;
        }
        else if (*p == '[')
        {
            ++p;
            v.kind = Json::kArray;
            if (Eat(']')) return v;
            do { v.items.push_back(Value()); } while (ok && Eat(','));
            if (!Eat(']')) ok = false;
        }
        else if (*p == '"') { v.kind = Json::kString; v.str = String(); }
        else if (!strncmp(p, "true", 4))  { v.kind = Json::kBool; v.boolean = true;  p += 4; }
        else if (!strncmp(p, "false", 5)) { v.kind = Json::kBool; v.boolean = false; p += 5; }
        else if (!strncmp(p, "null", 4))  { p += 4; }
        else
        {
            char* end = nullptr;
            v.number = strtod(p, &end);
            if (end == p) ok = false; else { v.kind = Json::kNumber; p = end; }
        }
        return v;
    }
};

void Num(const Json* o, const char* key, float& out)
{
    if (const Json* v = o ? o->Get(key) : nullptr)
        if (v->kind == Json::kNumber) out = (float)v->number;
}
void Vec3(const Json* o, const char* key, float out[3])
{
    if (const Json* v = o ? o->Get(key) : nullptr)
        if (v->kind == Json::kArray && v->items.size() == 3)
            for (int k = 0; k < 3; ++k) out[k] = (float)v->items[k].number;
}

std::string Escaped(const std::string& s)
{
    std::string r;
    for (char c : s) { if (c == '"' || c == '\\') r.push_back('\\'); r.push_back(c); }
    return r;
}

} // namespace

// The time and place as JSON members (with their leading comma), or "" when the preset has none.
static std::string TimeJson(const WeatherPreset& w)
{
    if (!w.timeSet) return std::string();
    char buf[256];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
        ",\n  \"time\":          { \"date\": \"%04d-%02d-%02d\", \"local\": %.6g, \"utcOffset\": %.6g },\n"
        "  \"place\":         { \"latitude\": %.6g, \"longitude\": %.6g, \"north\": %.6g }",
        w.time.year, w.time.month, w.time.day, w.time.localHour, w.time.utcOffset,
        w.time.latitude, w.time.longitude, w.time.northDeg);
    return buf;
}

bool WeatherFromJson(const char* text, WeatherPreset& out)
{
    Parser ps{ text };
    Json root = ps.Value();
    if (!ps.ok || root.kind != Json::kObject) return false;

    const Json* fmt = root.Get("format");
    if (!fmt || fmt->kind != Json::kString || fmt->str != "ngg-weather") return false;
    float version = 0.0f;
    Num(&root, "version", version);
    if (version > 1.0f)
        Log("weather: preset is format version %.0f, this build reads 1 - newer keys ignored", version);

    if (const Json* v = root.Get("name"))   if (v->kind == Json::kString) out.name = v->str;
    if (const Json* v = root.Get("source")) if (v->kind == Json::kString) out.source = v->str;

    const Json* sun = root.Get("sun");
    Vec3(sun, "direction", out.sky.sunDir);
    Num(sun, "intensity", out.sky.sunIntensity);

    const Json* atm = root.Get("atmosphere");
    Num(atm, "turbidity", out.sky.turbidity);
    Num(atm, "groundAlbedo", out.sky.groundAlbedo);
    Vec3(atm, "groundColour", out.sky.groundColour);

    Num(root.Get("clouds"), "cover", out.cloudCover);
    Num(root.Get("fog"), "visibility", out.fogVisibility);
    Num(root.Get("fog"), "height", out.fogHeight);
    Num(root.Get("precipitation"), "rain", out.rain);
    Num(root.Get("precipitation"), "snow", out.snow);
    Num(root.Get("exposure"), "value", out.sky.exposure);

    // "time": { "date": "2026-06-21", "local": 22.5, "utcOffset": 2 }, "place": { "latitude": 50, "longitude":
    // 14.4, "north": 0 } - both or neither (NGG design 5.4).
    const Json* tm = root.Get("time");
    const Json* pl = root.Get("place");
    if (tm && pl)
    {
        const Json* date = tm->Get("date");
        int y = 0, m = 0, d = 0;
        if (date && date->kind == Json::kString && sscanf_s(date->str.c_str(), "%d-%d-%d", &y, &m, &d) == 3)
        {
            float hour = (float)out.time.localHour, utc = (float)out.time.utcOffset;
            float lat = (float)out.time.latitude, lon = (float)out.time.longitude, north = (float)out.time.northDeg;
            Num(tm, "local", hour);
            Num(tm, "utcOffset", utc);
            Num(pl, "latitude", lat);
            Num(pl, "longitude", lon);
            Num(pl, "north", north);
            out.timeSet = true;
            out.time.year = y, out.time.month = m, out.time.day = d;
            out.time.localHour = hour, out.time.utcOffset = utc;
            out.time.latitude = lat, out.time.longitude = lon, out.time.northDeg = north;
        }
    }
    return true;
}

std::string WeatherToJson(const WeatherPreset& w)
{
    char buf[2048];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
        "{\n"
        "  \"format\": \"ngg-weather\", \"version\": 1,\n"
        "  \"name\": \"%s\",\n"
        "  \"source\": \"%s\",\n"
        "  \"sun\":           { \"direction\": [%.6g, %.6g, %.6g], \"intensity\": %.6g },\n"
        "  \"atmosphere\":    { \"turbidity\": %.6g, \"groundAlbedo\": %.6g, \"groundColour\": [%.6g, %.6g, %.6g] },\n"
        "  \"clouds\":        { \"cover\": %.6g },\n"
        "  \"fog\":           { \"visibility\": %.6g, \"height\": %.6g },\n"
        "  \"precipitation\": { \"rain\": %.6g, \"snow\": %.6g },\n"
        "  \"exposure\":      { \"value\": %.6g }%s\n"
        "}\n",
        Escaped(w.name).c_str(), Escaped(w.source).c_str(),
        w.sky.sunDir[0], w.sky.sunDir[1], w.sky.sunDir[2], w.sky.sunIntensity,
        w.sky.turbidity, w.sky.groundAlbedo,
        w.sky.groundColour[0], w.sky.groundColour[1], w.sky.groundColour[2],
        w.cloudCover, w.fogVisibility, w.fogHeight, w.rain, w.snow, w.sky.exposure, TimeJson(w).c_str());
    return buf;
}

bool LoadWeather(const wchar_t* path, WeatherPreset& out)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") || !f) return false;
    std::string text;
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) text.append(chunk, n);
    fclose(f);
    if (!WeatherFromJson(text.c_str(), out))
    {
        Log("weather: %ls is not a valid ngg-weather preset", path);
        return false;
    }
    Log("weather: loaded %ls (%s)", path, out.name.c_str());
    return true;
}

bool SaveWeather(const wchar_t* path, const WeatherPreset& in)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") || !f) return false;
    const std::string s = WeatherToJson(in);
    const bool ok = fwrite(s.data(), 1, s.size(), f) == s.size();
    fclose(f);
    return ok;
}

// The format's default record is the renderer's default sky: a producer that starts from it and changes nothing gets
// the weather a scene without presets draws.
namespace {
constexpr SkyParams kSky{};
constexpr WeatherRecord kRec = DefaultWeatherRecord();
static_assert(kRec.sunDirection[0] == kSky.sunDir[0] && kRec.sunDirection[1] == kSky.sunDir[1] &&
              kRec.sunDirection[2] == kSky.sunDir[2] && kRec.sunIntensity == kSky.sunIntensity &&
              kRec.turbidity == kSky.turbidity && kRec.groundAlbedo == kSky.groundAlbedo &&
              kRec.groundColour[0] == kSky.groundColour[0] && kRec.groundColour[1] == kSky.groundColour[1] &&
              kRec.groundColour[2] == kSky.groundColour[2] && kRec.cloudCover == kSky.cloudCover &&
              kRec.fogVisibility == kSky.fogVisibility && kRec.fogHeight == kSky.fogHeight &&
              kRec.exposure == kSky.exposure,
              "ngg_format.h's DefaultWeatherRecord() and sky.h's SkyParams defaults disagree");
constexpr SkyTime kTime{};
static_assert(kRec.year == kTime.year && kRec.month == kTime.month && kRec.day == kTime.day &&
              kRec.localHour == (float)kTime.localHour && kRec.utcOffset == (float)kTime.utcOffset &&
              kRec.latitude == (float)kTime.latitude && kRec.longitude == (float)kTime.longitude &&
              kRec.northDeg == (float)kTime.northDeg && kRec.timeFlags == 0,
              "ngg_format.h's DefaultWeatherRecord() and astronomy.h's SkyTime defaults disagree");
} // namespace

bool ParseWeatherChunk(const void* payload, size_t bytes, std::vector<WeatherPreset>& out)
{
    out.clear();
    if (!payload || bytes < sizeof(WeatherChunkHeader))
    {
        Log("weather: NGGW chunk too small (%zu bytes)", bytes);
        return false;
    }
    const uint8_t* p = static_cast<const uint8_t*>(payload);
    WeatherChunkHeader h;
    memcpy(&h, p, sizeof(h));
    // A file written before 5.0t carried JSON here; its first word is a count,
    // not 1, and the sizes cannot add up - so it lands in this refusal rather
    // than being read as garbage.
    if (h.version != kWeatherVersion || h.recordSize < kWeatherRecordMinSize ||
        (uint64_t)sizeof(h) + (uint64_t)h.count * h.recordSize > bytes)
    {
        Log("weather: NGGW not understood (version %u, %u record(s) of %u bytes in %zu) - "
            "re-run rbrtm --import-weather or reconvert", h.version, h.count, h.recordSize, bytes);
        return false;
    }
    for (uint32_t i = 0; i < h.count; ++i)
    {
        // What the record has, over the defaults: a shorter (older) record leaves the newer fields at theirs.
        WeatherRecord r = DefaultWeatherRecord();
        memcpy(&r, p + sizeof(h) + (size_t)i * h.recordSize, h.recordSize < sizeof(r) ? h.recordSize : sizeof(r));
        r.name[sizeof(r.name) - 1] = 0;

        WeatherPreset w;
        w.name = r.name;
        for (int k = 0; k < 3; ++k) { w.sky.sunDir[k] = r.sunDirection[k]; w.sky.groundColour[k] = r.groundColour[k]; }
        w.sky.sunIntensity = r.sunIntensity;
        w.sky.turbidity    = r.turbidity;
        w.sky.groundAlbedo = r.groundAlbedo;
        w.sky.exposure     = r.exposure;
        w.cloudCover       = r.cloudCover;
        w.fogVisibility    = r.fogVisibility;
        w.fogHeight        = r.fogHeight;
        w.rain             = r.rain;
        w.snow             = r.snow;
        w.timeSet          = (r.timeFlags & kWeatherTimeSet) != 0;
        w.time.year = r.year, w.time.month = r.month, w.time.day = r.day;
        w.time.localHour = r.localHour, w.time.utcOffset = r.utcOffset;
        w.time.latitude = r.latitude, w.time.longitude = r.longitude, w.time.northDeg = r.northDeg;
        out.push_back(w);
    }
    return true;
}

std::vector<uint8_t> PackWeatherChunk(const std::vector<WeatherPreset>& presets)
{
    WeatherChunkHeader h = {};
    h.version = kWeatherVersion;
    h.count = (uint32_t)presets.size();
    h.recordSize = (uint32_t)sizeof(WeatherRecord);

    std::vector<uint8_t> out(sizeof(h) + presets.size() * sizeof(WeatherRecord));
    memcpy(out.data(), &h, sizeof(h));
    for (size_t i = 0; i < presets.size(); ++i)
    {
        const WeatherPreset& w = presets[i];
        WeatherRecord r = DefaultWeatherRecord();
        if (w.name.size() >= sizeof(r.name))
            Log("weather: preset name '%s' is longer than %zu bytes - truncated", w.name.c_str(), sizeof(r.name) - 1);
        strncpy_s(r.name, sizeof(r.name), w.name.c_str(), _TRUNCATE);
        for (int k = 0; k < 3; ++k) { r.sunDirection[k] = w.sky.sunDir[k]; r.groundColour[k] = w.sky.groundColour[k]; }
        r.sunIntensity  = w.sky.sunIntensity;
        r.turbidity     = w.sky.turbidity;
        r.groundAlbedo  = w.sky.groundAlbedo;
        r.exposure      = w.sky.exposure;
        r.cloudCover    = w.cloudCover;
        r.fogVisibility = w.fogVisibility;
        r.fogHeight     = w.fogHeight;
        r.rain          = w.rain;
        r.snow          = w.snow;
        r.timeFlags     = w.timeSet ? kWeatherTimeSet : 0u;
        r.year = w.time.year, r.month = w.time.month, r.day = w.time.day;
        r.localHour = (float)w.time.localHour, r.utcOffset = (float)w.time.utcOffset;
        r.latitude = (float)w.time.latitude, r.longitude = (float)w.time.longitude, r.northDeg = (float)w.time.northDeg;
        memcpy(out.data() + sizeof(h) + i * sizeof(r), &r, sizeof(r));
    }
    return out;
}

void ApplyWeather(const WeatherPreset& w, SkyParams& sky)
{
    // Exposure is the viewer's dial as much as the preset's: keep the caller's
    // when the preset leaves it at the default.
    const float exposure = sky.exposure;
    sky = w.sky;
    if (w.sky.exposure == SkyParams().exposure) sky.exposure = exposure;
    sky.cloudCover    = w.cloudCover;
    sky.fogVisibility = w.fogVisibility;
    sky.fogHeight     = w.fogHeight;
    if (w.timeSet) ApplySkyTime(w.time, sky);
}

SkyBodies ApplySkyTime(const SkyTime& t, SkyParams& sky)
{
    const SkyBodies b = ComputeSkyBodies(t);
    for (int k = 0; k < 3; ++k) sky.sunDir[k] = -b.sunTowards[k];   // the direction the light travels
    return b;
}

} // namespace core
} // namespace ngg
