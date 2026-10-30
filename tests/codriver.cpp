// The co-driver on stage 41's pacenotes, without a voice (game/src/sound/codriver.h): that nothing is called before
// the stage time runs, and that the HUD shows a call once it is called (spoken) and the next by the time to it. Prints
// "codriver: all good" or the first failure.
//
//   codriver <the stage's .dls>
//
// Stage 41's first calls: R4 (medium right) at 155 m, L1 long (flat left long) at 203 m; the car starts at 134 m.

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "data/dls.h"
#include "sound/codriver.h"

namespace {

int Fail(const std::string& why)
{
    std::printf("codriver: FAILED - %s\n", why.c_str());
    return 1;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) return Fail("usage: codriver <stage .dls>");
    wrally::sound::CoDriver c;
    c.LoadAsync(nullptr, "", phys::data::ReadDls(argv[1]).pacenotes);
    for (int i = 0; i < 500 && !c.Ready(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!c.Ready()) return Fail("not ready after 5 s");

    // The HUD after each step: the car at `distance` going `speed`, the co-driver updated there first when `update`
    // (lead 3 s, at least 60 m; `running` the stage time). Without a voice a phrase counts as spoken once called.
    struct Step {
        float distance, speed;
        bool update, running;
        const char *current, *next;
        const char* what;
    } steps[] = {
        {134, 0, true, false, "", "", "before GO: nothing called, nothing shown"},
        {134, 0, true, true, "R4", "", "GO, standing 21 m before R4: called and shown (L1 long 69 m: not yet), no next"},
        {140, 20, true, true, "R4", "L1 long", "20 m/s, L1 long 63 m (3.2 s, past the 60 m lead): not called, the next"},
        {158, 20, false, true, "R4", "L1 long", "3 m past R4: still R4"},
        {161, 20, false, true, "", "", "6 m past R4, L1 long not called yet: nothing (the HUD waits for the voice)"},
        {161, 20, true, true, "L1 long", "", "called: L1 long; the next (K left, 235 m) 11.8 s away"},
    };
    for (const Step& k : steps) {
        if (k.update) c.Update(k.distance, k.speed, 3.0f, k.running);
        std::string current, next;
        c.Upcoming(k.distance, k.speed, 8.0f, current, next);
        if (current != k.current || next != k.next)
            return Fail(std::string(k.what) + ": shown \"" + current + "\" / \"" + next + "\", wanted \"" + k.current +
                        "\" / \"" + k.next + "\"");
        if (&k == &steps[1] && c.LastCall() != "MEDIUMRIGHT")
            return Fail("the first call at GO: \"" + c.LastCall() + "\", wanted MEDIUMRIGHT");
    }
    std::printf("codriver: %zu calls; all good\n", c.Phrases());
    return 0;
}
