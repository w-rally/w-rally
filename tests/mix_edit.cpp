// The Debug menu's sound mix (game/src/sound/mix_edit.h) against the audio module's own reading of it (audio::Mix):
// the file's values loaded, a change made and saved, the saved file read back by both, its comments kept. Prints
// "mix-edit: all good" or the first failure.
//
//   mix-edit <game/config/sound-mix.ini> <a scratch file to save to>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

#include "audio/mix.h"
#include "sound/mix_edit.h"

namespace {

int Fail(const std::string& why)
{
    std::printf("mix-edit: FAILED - %s\n", why.c_str());
    return 1;
}

std::string ReadAll(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

wrally::sound::MixValue* Find(wrally::sound::MixEdit& e, const std::string& key)
{
    for (wrally::sound::MixValue& v : e.values)
        if (key == v.key) return &v;
    return nullptr;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) return Fail("usage: mix-edit <sound-mix.ini> <scratch file>");
    const std::string text = ReadAll(argv[1]);
    if (text.empty()) return Fail(std::string("cannot read ") + argv[1]);
    std::ofstream(argv[2], std::ios::binary) << text;

    wrally::sound::MixEdit e;
    e.Load(text);
    wrally::sound::MixValue *grains = Find(e, "engine.grains"), *jumps = Find(e, "engine.grainJumps"), *wind = Find(e, "wind"),
                            *on = Find(e, "engine.on");
    if (!grains || !jumps || !wind || !on) return Fail("a value is missing");
    // A change: the engine as loops, fewer jumps, the wind soloed at half, the engine on the throttle apart from off it.
    grains->value = 0, jumps->value = 0.5f, wind->value = 0.5f, wind->solo = true, on->value = 1.25f;

    // What the sound gets says it.
    audio::Mix m;
    if (const std::string err = m.Parse(e.Text(), "text"); !err.empty()) return Fail("the text: " + err);
    auto same = [](float a, float b) { return std::fabs(a - b) < 1e-4f; };
    auto check = [&](const audio::Mix& x, const char* what) -> std::string {
        if (x.grains != 0) return std::string(what) + ": engine.grains not 0";
        if (!same(x.grainJumps, 0.5f)) return std::string(what) + ": engine.grainJumps not 0.5";
        if (!same(x.volume[audio::Mix::kWind], 0.5f) || !x.solo[audio::Mix::kWind]) return std::string(what) + ": wind not 0.5 solo";
        if (!same(x.volume[audio::Mix::kEngineOn], 1.25f) || !same(x.volume[audio::Mix::kEngineOff], 1.0f))
            return std::string(what) + ": engine.on / off not 1.25 / 1";
        return "";
    };
    if (const std::string err = check(m, "the text"); !err.empty()) return Fail(err);

    // Saved: the module reads the same from the file, the editor too, and the comments are still there.
    if (const std::string err = e.Save(argv[2]); !err.empty()) return Fail(err);
    audio::Mix saved;
    if (const std::string err = saved.Read(argv[2]); !err.empty()) return Fail("the saved file: " + err);
    if (const std::string err = check(saved, "the saved file"); !err.empty()) return Fail(err);
    wrally::sound::MixEdit back;
    back.Load(ReadAll(argv[2]));
    for (size_t i = 0; i < e.values.size(); ++i)
        if (!same(back.values[i].value, e.values[i].value) || back.values[i].solo != e.values[i].solo)
            return Fail(std::string("read back: ") + e.values[i].key);
    const std::string after = ReadAll(argv[2]);
    for (size_t at = 0; (at = text.find('#', at)) != std::string::npos;) {
        const size_t end = text.find('\n', at);
        const std::string comment = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
        std::string c = comment;
        if (!c.empty() && c.back() == '\r') c.pop_back();
        if (after.find(c) == std::string::npos) return Fail("a comment lost: " + c);
        at = end == std::string::npos ? text.size() : end;
    }
    // Saved again unchanged: the same file.
    if (const std::string err = back.Save(argv[2]); !err.empty()) return Fail(err);
    if (ReadAll(argv[2]) != after) return Fail("saving again changed the file");
    std::printf("mix-edit: all good\n");
    return 0;
}
