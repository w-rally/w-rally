#include "ui/menus.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>

#include "assets/car_lights.h"
#include "imgui.h"
#include "ui/imgui_layer.h"

namespace wrally::ui {

namespace {

constexpr ImGuiWindowFlags kScreen = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;

// A screen: a window of `title` centred, in the menu's font. Every Begin has its End (ImGui's rule), so the body
// runs inside.
template <class F> void Screen(const ImGuiLayer& ui, const char* title, int width, int height, float minWidth, F&& body)
{
    ImGui::SetNextWindowPos(ImVec2(width * 0.5f, height * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(std::min(minWidth * ui.Scale(), width * 0.95f), 0), ImVec2(width * 0.95f, height * 0.95f));
    if (ui.MenuFont()) ImGui::PushFont(ui.MenuFont());
    if (ImGui::Begin(title, nullptr, kScreen)) body();
    ImGui::End();
    if (ui.MenuFont()) ImGui::PopFont();
}

// A full-width button; the first one of a screen takes the keyboard's focus when it opens.
bool Button(const char* label, bool first = false)
{
    if (first && ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    return ImGui::Button(label, ImVec2(-FLT_MIN, 0));
}

// A combo of `items`; `at` the chosen one. True when it changed.
bool Choose(const char* label, int& at, const std::vector<std::string>& items)
{
    bool changed = false;
    const char* shown = at >= 0 && at < static_cast<int>(items.size()) ? items[at].c_str() : "";
    if (ImGui::BeginCombo(label, shown)) {
        for (int i = 0; i < static_cast<int>(items.size()); ++i) {
            ImGui::PushID(i);
            if (ImGui::Selectable(items[i].c_str(), i == at)) at = i, changed = true;
            if (i == at) ImGui::SetItemDefaultFocus();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    return changed;
}

// A text field over a std::string. True when it changed.
bool Text(const char* label, std::string& s, bool readOnly)
{
    char buf[1024];
    std::snprintf(buf, sizeof buf, "%s", s.c_str());
    if (!ImGui::InputText(label, buf, sizeof buf, readOnly ? ImGuiInputTextFlags_ReadOnly : 0)) return false;
    s = buf;
    return true;
}

bool Check(const char* label, bool& v) { return ImGui::Checkbox(label, &v); }

// The sound mix's values: the engine's switches and amounts, then each layer's volume with its solo; Save / Reload /
// Defaults under them, `status` after.
MixChoice MixControls(std::vector<sound::MixValue>& values, const std::string& status)
{
    MixChoice c = MixChoice::None;
    bool layers = false;
    ImGui::SeparatorText("Engine and anti-lag");
    for (size_t i = 0; i < values.size(); ++i) {
        sound::MixValue& v = values[i];
        ImGui::PushID(static_cast<int>(i));
        bool ch = false;
        if (v.kind == sound::MixValue::Switch) {
            bool on = v.value >= 0.5f;
            if (Check(v.label, on)) v.value = on ? 1.0f : 0.0f, ch = true;
        } else {
            if (v.kind == sound::MixValue::Volume && !layers) ImGui::SeparatorText("Layers (volume, solo)"), layers = true;
            ch = ImGui::SliderFloat(v.label, &v.value, v.min, v.max, "%.2f");
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) v.value = v.init, ch = true;   // right click: its default
            if (v.kind == sound::MixValue::Volume) {
                ImGui::SameLine();
                ch |= Check("solo", v.solo);
            }
        }
        if (ch) c = MixChoice::Changed;
        ImGui::PopID();
    }
    ImGui::TextDisabled("Right click a slider: its default.");
    const float w = (ImGui::GetContentRegionAvail().x - 2 * ImGui::GetStyle().ItemSpacing.x) / 3;
    if (ImGui::Button("Save to mix file", ImVec2(w, 0))) c = MixChoice::Save;
    ImGui::SameLine();
    if (ImGui::Button("Reload file", ImVec2(w, 0))) c = MixChoice::Reload;
    ImGui::SameLine();
    if (ImGui::Button("Defaults", ImVec2(w, 0))) c = MixChoice::Defaults;
    if (!status.empty()) ImGui::TextWrapped("%s", status.c_str());
    return c;
}

} // namespace

MixChoice MixPanel(std::vector<sound::MixValue>& values, const std::string& status, bool& open, int width, int height)
{
    ImGui::SetNextWindowPos(ImVec2(width - 8.0f, 8.0f), ImGuiCond_Always, ImVec2(1, 0));
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(width * 0.5f, height - 16.0f));
    ImGui::SetNextWindowBgAlpha(0.85f);
    MixChoice c = MixChoice::None;
    constexpr ImGuiWindowFlags kPanel = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing;
    if (ImGui::Begin("Sound mix", &open, kPanel)) {
        ImGui::PushItemWidth(ImGui::GetFontSize() * 12.0f);
        c = MixControls(values, status);
        ImGui::PopItemWidth();
    }
    ImGui::End();
    return c;
}

void DrawBackdrop(int width, int height)
{
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    d->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(float(width), float(height)), IM_COL32(18, 24, 34, 255),
                               IM_COL32(18, 24, 34, 255), IM_COL32(52, 34, 22, 255), IM_COL32(52, 34, 22, 255));
}

MainChoice MainMenu(const ImGuiLayer& ui, int width, int height, const std::string& note)
{
    // The title above the menu, in the HUD's large face.
    if (ImFont* f = ui.HudSmall()) {
        const char* title = "WRally";
        const float size = std::max(48.0f, height * 0.12f);
        const ImVec2 t = f->CalcTextSizeA(size, FLT_MAX, 0, title);
        ImGui::GetBackgroundDrawList()->AddText(f, size, ImVec2((width - t.x) * 0.5f, height * 0.12f), IM_COL32(235, 235, 235, 255), title);
    }
    MainChoice c = MainChoice::None;
    Screen(ui, "Main menu", width, height, 320.0f, [&] {
        if (Button("Quick rally", true)) c = MainChoice::QuickRally;
        if (Button("Settings")) c = MainChoice::Settings;
        if (Button("Exit")) c = MainChoice::Exit;
        if (!note.empty()) {
            ImGui::Separator();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "%s", note.c_str());
            ImGui::PopTextWrapPos();
        }
    });
    return c;
}

QuickChoice QuickRally(const ImGuiLayer& ui, const assets::Catalog& catalog, app::Settings& s, int width, int height)
{
    QuickChoice c = QuickChoice::None;
    Screen(ui, "Quick rally", width, height, 720.0f, [&] {
        ImGui::PushItemWidth(-ImGui::GetFontSize() * 7.0f);
        // The stage
        std::vector<std::string> items;
        int stage = 0;
        for (size_t i = 0; i < catalog.stages.size(); ++i) {
            items.push_back(catalog.stages[i].title);
            if (catalog.stages[i].key == s.stage) stage = static_cast<int>(i);
        }
        if (Choose("Stage", stage, items) || s.stage.empty()) s.weather.clear();
        const assets::Stage* st = catalog.stages.empty() ? nullptr : &catalog.stages[stage];
        if (st) s.stage = st->key;

        // The car, with where its physics comes from
        items.clear();
        int car = 0;
        for (size_t i = 0; i < catalog.cars.size(); ++i) {
            items.push_back(catalog.cars[i].name + "   [" + catalog.cars[i].physicsFrom + "]");
            if (!_stricmp(catalog.cars[i].folder.c_str(), s.car.c_str())) car = static_cast<int>(i);
        }
        if (Choose("Car", car, items)) s.setup.clear();
        const assets::Car* cr = catalog.cars.empty() ? nullptr : &catalog.cars[car];
        if (cr) s.car = cr->folder;

        // Its setup: the default for the surface, or a saved one
        items = {"Default"};
        int setup = 0;
        const std::vector<assets::Setup> setups = cr ? catalog.SetupsFor(*cr) : std::vector<assets::Setup>{};
        for (size_t i = 0; i < setups.size(); ++i) {
            items.push_back(setups[i].name);
            if (setups[i].path == s.setup) setup = static_cast<int>(i) + 1;
        }
        Choose("Setup", setup, items);
        s.setup = setup ? setups[setup - 1].path : "";

        // Its lamps (NGG design 5.4 N3b): the ramps it can run - its own pods where its .ini places them - and the ramp's
        // aim, remembered per car.
        if (cr) {
            // Per car folder, read once: the layouts' names, and as the menu shows them.
            struct Layouts {
                std::vector<std::string> keys, shown;
            };
            static std::map<std::string, Layouts> layoutsOf;
            Layouts& ls = layoutsOf[cr->folder];
            if (ls.keys.empty()) {
                const float noCog[3] = {};
                const assets::CarLamps lamps = assets::LoadCarLamps(cr->sgc, noCog);
                for (const assets::LampLayout& l : lamps.layouts) {
                    ls.keys.push_back(l.name);
                    ls.shown.push_back(l.name != "ramp" ? l.name
                                                        : std::string(lamps.fromCar ? "its own ramp (" : "a ramp (") +
                                                              std::to_string(l.lamps.size()) + " pods)");
                }
            }
            const std::vector<std::string>& keys = ls.keys;
            const std::vector<std::string>& names = ls.shown;
            app::Settings::Lamps& lamp = s.lamps[cr->folder];
            int layout = 0;
            for (size_t i = 0; i < keys.size(); ++i)
                if (keys[i] == lamp.layout) layout = static_cast<int>(i);
            Choose("Lamps", layout, names);
            lamp.layout = keys.empty() ? "" : keys[layout];
            ImGui::SliderFloat("Ramp spread", &lamp.spread, -5.0f, 10.0f, "%+.1f deg out");
            ImGui::SliderFloat("Ramp aim", &lamp.pitch, -3.0f, 3.0f, "%+.1f deg up");
        }

        items = {"Off", "Safe", "Reduced", "Realistic"};
        int damage = static_cast<int>(std::min(s.damage, 3u));
        Choose("Damage", damage, items);
        s.damage = static_cast<uint32_t>(damage);

        // The weather: the stage's presets (custom ones later)
        items = {"Stage default"};
        int weather = 0;
        if (st)
            for (size_t i = 0; i < st->weathers.size(); ++i) {
                items.push_back(st->weathers[i]);
                if (!_stricmp(st->weathers[i].c_str(), s.weather.c_str())) weather = static_cast<int>(i) + 1;
            }
        Choose("Weather", weather, items);
        s.weather = weather && st ? st->weathers[weather - 1] : "";
        if (st && !st->weathersDrawn && weather)
            ImGui::TextDisabled("   this .ngg has no weather presets: its own sky is drawn, the weather sets the road");

        items = {"From the weather", "Dry", "Damp", "Wet"};
        int road = static_cast<int>(std::min(s.road, 3u));
        Choose("Road", road, items);
        s.road = static_cast<uint32_t>(road);

        // The time of day, on an autumn day (kQuickDate): the sun, the moon and the stars from it (NGG design 5.4).
        static const char* const kTimes[] = {"", "08:00", "12:00", "17:30", "18:30", "22:00"};
        items = {"The weather's own", "Morning 08:00", "Noon 12:00", "Evening 17:30", "Dusk 18:30", "Night 22:00"};
        int time = 0;
        for (int i = 1; i < 6; ++i)
            if (s.time == kTimes[i]) time = i;
        Choose("Time of day", time, items);
        s.time = kTimes[time];
        ImGui::PopItemWidth();

        ImGui::Separator();
        ImGui::BeginDisabled(!st || !cr);
        if (Button("Start", true)) c = QuickChoice::Start;
        ImGui::EndDisabled();
        if (Button("Back")) c = QuickChoice::Back;
        if (!st) ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "No stages found: see Settings.");
        else if (!cr) ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "No cars found: see Settings.");
    });
    return c;
}

namespace {

int g_capturing = -1;          // the action waiting for its key (Controls), -1 none
bool g_opened = false;         // its popup opened this frame (the key that opened it is not the binding)
std::string g_controlsNote;    // what the last binding did ("also unbinds ...")

// Settings -> Controls: each action and its key; a click waits for a key (Esc: no change), Delete unbinds.
void Controls(input::Bindings& keys, UINT keyDown)
{
    using input::Action;
    const char* group = "";
    if (ImGui::BeginTable("keys", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("action", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("key", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        for (int a = 0; a < input::kActions; ++a) {
            const input::ActionInfo& info = input::Info(static_cast<Action>(a));
            if (std::strcmp(group, info.group)) {
                group = info.group;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", group);
            }
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(info.label);
            ImGui::TableNextColumn();
            ImGui::PushID(a);
            const UINT scan = keys.scan[a];
            if (!scan) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.4f, 1.0f));
            if (ImGui::Button(scan ? input::KeyName(scan).c_str() : "unbound", ImVec2(-FLT_MIN, 0))) {
                g_capturing = a;
                g_opened = true;
                g_controlsNote.clear();
                ImGui::OpenPopup("press a key");
            }
            if (!scan) ImGui::PopStyleColor();
            if (ImGui::BeginPopupModal("press a key", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
                ImGui::Text("Press a key for %s", info.label);
                ImGui::TextDisabled("Esc: keep %s    Delete: unbound", input::KeyName(scan).c_str());
                if (g_opened) {
                    g_opened = false;
                } else if (keyDown && g_capturing == a) {
                    if (keyDown == input::kEscapeScan) {
                        // (Esc is the menus': not bindable)
                    } else if (keyDown == 0xe053) {   // Delete
                        keys.scan[a] = 0;
                    } else if (const Action other = keys.Bind(static_cast<Action>(a), keyDown); other != Action::Count) {
                        g_controlsNote = input::KeyName(keyDown) + " was " + input::Info(other).label + "'s: that is unbound now";
                    }
                    g_capturing = -1;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (!g_controlsNote.empty()) ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "%s", g_controlsNote.c_str());
    if (Button("Default keys")) {
        keys = input::Bindings();
        g_controlsNote.clear();
    }
}

} // namespace

SettingsChoice SettingsUi(const ImGuiLayer& ui, app::Settings& s, bool inStage, const std::string& log, UINT keyDown,
                          int width, int height)
{
    SettingsChoice c = SettingsChoice::None;
    Screen(ui, "Settings", width, height, 760.0f, [&] {
        if (!ImGui::BeginTabBar("settings")) return;
        ImGui::PushItemWidth(-ImGui::GetFontSize() * 9.0f);
        if (ImGui::BeginTabItem("Content")) {
        ImGui::BeginDisabled(inStage);
        Text("RBR folder", s.rbrPath, inStage);
        Text(".ngg folders", s.nggFolders, inStage);
        ImGui::TextDisabled("   ';' between folders; empty: %s", s.rbrPath.empty() ? "<RBR folder>\\Ngg" : (s.rbrPath + "\\Ngg").c_str());
        std::vector<std::string> slots;
        int base = 5;
        for (int i = 0; i < 8; ++i) {
            slots.emplace_back(assets::SlotPhysics(i));
            if (slots.back() == s.basePhysics) base = i;
        }
        Choose("Base physics", base, slots);
        s.basePhysics = slots[base];
        ImGui::TextDisabled("   for a car without physics of its own");
        if (Button("Rescan content")) c = SettingsChoice::Rescan;
        if (Button("Extract the slot cars' physics into their folders")) c = SettingsChoice::Extract;
        ImGui::EndDisabled();
        if (inStage) ImGui::TextDisabled("   (the content can be changed from the main menu)");
        if (!log.empty()) {
            ImGui::Separator();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 36.0f);
            ImGui::TextUnformatted(log.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Graphics")) {
        std::vector<std::string> quality = {"Low", "Medium", "High"};
        int q = static_cast<int>(std::min(s.quality, 2u));
        bool g = Choose("Quality", q, quality);
        s.quality = static_cast<uint32_t>(q);
        g |= Check("Ray tracing", s.rayTracing);
        g |= Check("TAA", s.taa);
        g |= Check("Shadows", s.shadows);
        g |= Check("Vertical sync", s.vsync);
        if (g && c == SettingsChoice::None) c = SettingsChoice::Graphics;
        ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Sound")) {
        Check("Sound", s.sound);
        ImGui::SameLine();
        ImGui::TextDisabled("(from the next stage)");
        ImGui::SeparatorText("Co-driver");
        ImGui::BeginDisabled(!s.sound);
        Check("Pacenotes", s.codriver);
        ImGui::SameLine();
        Check("in replays too", s.codriverReplay);
        ImGui::SliderFloat("Volume", &s.codriverVolume, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Call ahead", &s.codriverLead, 1.0f, 6.0f, "%.1f s at the speed");
        ImGui::TextDisabled("   more: called earlier; at least 60 m ahead (from the next stage)");
        ImGui::EndDisabled();
        ImGui::SeparatorText("Pacenotes on the HUD");
        ImGui::SliderFloat("Next call", &s.pacenoteNext, 0.0f, 20.0f, "%.1f s before it");
        ImGui::TextDisabled("   at the car's speed; the current call shows as it is spoken (Call ahead; live runs)");
        ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Controls")) {
            Controls(s.keys, keyDown);
            ImGui::EndTabItem();
        }
        ImGui::PopItemWidth();
        ImGui::EndTabBar();

        ImGui::Separator();
        if (Button("Back", true)) c = SettingsChoice::Back;
    });
    return c;
}

bool CapturingKey() { return g_capturing >= 0; }

PauseChoice PauseMenu(const ImGuiLayer& ui, int width, int height)
{
    PauseChoice c = PauseChoice::None;
    Screen(ui, "Paused", width, height, 320.0f, [&] {
        if (Button("Resume", true)) c = PauseChoice::Resume;
        if (Button("Restart stage")) c = PauseChoice::Restart;
        if (Button("Settings")) c = PauseChoice::Settings;
        if (Button("Debug")) c = PauseChoice::Debug;
        if (Button("Exit to main menu")) c = PauseChoice::ExitToMenu;
    });
    return c;
}

DebugChoice DebugMenu(const ImGuiLayer& ui, DebugSwitches& d, const std::vector<std::string>& views, const input::Bindings& keys,
                      int width, int height, MixChoice& mixDone)
{
    DebugChoice c = DebugChoice::None;
    mixDone = MixChoice::None;
    Screen(ui, "Debug", width, height, 560.0f, [&] {
        ImGui::PushItemWidth(-ImGui::GetFontSize() * 9.0f);
        bool ch = false;
        ImGui::SeparatorText("Renderer");
        using input::Action;
        // A label with its key(s): "TAA  (T)".
        auto k = [&](const char* label, Action a, Action b = Action::Count) {
            std::string s = std::string(label) + "  (" + input::KeyName(keys[a]);
            if (b != Action::Count) s += " " + input::KeyName(keys[b]);
            return s + ")";
        };
        ch |= Check(k("TAA", Action::Taa).c_str(), d.taa);
        ch |= Check(k("Ambient occlusion", Action::Ao).c_str(), d.ao);
        ch |= Check(k("Shadows", Action::Shadows).c_str(), d.shadows);
        ch |= Check(k("Ray tracing", Action::RayTracing).c_str(), d.rayTracing);
        ch |= Check(k("Readout", Action::Readout).c_str(), d.readout);
        ch |= Choose(k("View", Action::DebugView).c_str(), d.debugView, views);
        if (d.legacyProfile >= 0) {   // (an imported .lbs stage: how its cards are lit)
            const std::vector<std::string> profiles = {"Off (as a modern stage)", "A - RBR's bake, today's light",
                                                       "B - dynamic, bake as occlusion"};
            ch |= Choose("Legacy cards", d.legacyProfile, profiles);
        }
        ImGui::SeparatorText("Camera");
        std::vector<std::string> cameras = {"Chase", "Bonnet", "Above", "Beside", "TV"};
        cameras.resize(static_cast<size_t>(std::max(1, std::min(d.cameras, 5))));
        ch |= Choose(k("Camera", Action::Camera).c_str(), d.camera, cameras);
        ch |= ImGui::SliderFloat(k("Chase distance", Action::ChaseIn, Action::ChaseOut).c_str(), &d.chaseDistance, 2.0f, 20.0f, "%.1f m");
        ch |= ImGui::SliderFloat(k("Beside angle", Action::OrbitLeft, Action::OrbitRight).c_str(), &d.orbitDegrees, -180.0f, 180.0f, "%.0f deg");
        ImGui::SeparatorText("Sound and driving");
        ch |= Check(k("Muted", Action::Mute).c_str(), d.muted);
        ch |= Check(k("Anti-lag armed at standstill", Action::AntiLagIdle).c_str(), d.antiLagIdle);
        if (d.mix) {
            ch |= Check("Sound mix panel while driving", d.mixPanel);
            if (ImGui::CollapsingHeader("Sound mix")) mixDone = MixControls(*d.mix, d.mixStatus);
        }
        ImGui::PopItemWidth();
        ImGui::Separator();
        if (Button("Back", true)) c = DebugChoice::Back;
        if (ch && c == DebugChoice::None) c = DebugChoice::Changed;
    });
    return c;
}

} // namespace wrally::ui
