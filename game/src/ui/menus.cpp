#include "ui/menus.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

#include "assets/car_lights.h"
#include "imgui.h"
#include "ui/imgui_layer.h"
#include "ui/menu_look.h"

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

namespace {

constexpr float kRowW = 600, kRowH = 68, kRowGap = 4, kCut = 12;   // the main menu's row, its cut right edge


struct MainItem {
    const char* label;
    const char* desc;   // null: not there yet (shown, not selectable)
    MainChoice choice;
};
const MainItem kMainItems[] = {
    {"ONLINE EVENTS", nullptr, MainChoice::None},
    {"QUICK RALLY", "Pick a stage and a car, and go straight to the start line.", MainChoice::QuickRally},
    {"GARAGE", "Every car: what it is, its class, how it drives - and the one you drive.", MainChoice::Garage},
    {"MARKETPLACE", nullptr, MainChoice::None},
    {"SETTINGS", "The RBR folder, graphics, sound and the controls.", MainChoice::Settings},
    {"EXIT", "Quit to desktop.", MainChoice::Exit},
};
constexpr int kMainCount = int(sizeof kMainItems / sizeof kMainItems[0]);

int g_mainAt = 1;              // the selected item (Quick rally first)
float g_mainPlate = -1;        // where the selection's plate is, in rows (eased to g_mainAt; -1: not drawn yet)
float g_mainOn[kMainCount];    // how selected each item looks, 0..1 (its marker, its ink)

} // namespace

MainChoice MainMenu(const ImGuiLayer& ui, int width, int height, const std::string& note, const VideoFrame& video)
{
    // The whole window is the screen: the design at 1080p scaled to its height, the left column from the left edge and
    // the livery's stripes from the right.
    const float k = std::min(height / 1080.0f, width / 1280.0f), W = float(width), H = float(height);
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(W, H));
    constexpr ImGuiWindowFlags kFull = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse;
    MainChoice c = MainChoice::None;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    if (ImGui::Begin("Main menu", nullptr, kFull)) {
        ImDrawList* d = ImGui::GetWindowDrawList();
        Backdrop(d, W, H, k, kSurfaceBase, video, true);

        // The column: 48 px safe margin, 32 px padding.
        const float x0 = 80 * k, y0 = 80 * k;
        ImFont* mark = ui.Wordmark();
        ImFont* item = ui.MenuItem();
        ImFont* text = ui.MenuText();
        const float wr = mark->CalcTextSizeA(72 * k, FLT_MAX, 0, "W-R").x;
        d->AddText(mark, 72 * k, ImVec2(x0, y0), kInk, "W-R");
        d->AddText(mark, 72 * k, ImVec2(x0 + wr, y0), kInkMuted, "ally");

        // The rows under the wordmark, no overline over them: the main menu needs no name.
        const float rowsY = y0 + 72 * k + 116 * k, pitch = (kRowH + kRowGap) * k;

        // The selection: W / S (and the arrows) move it, skipping what is not there yet; Enter or Space picks it; the
        // mouse selects what it moves over and picks what it clicks.
        auto step = [&](int dir) {
            for (int i = g_mainAt + dir; i >= 0 && i < kMainCount; i += dir)
                if (kMainItems[i].desc) return void(g_mainAt = i);
        };
        if (ImGui::IsKeyPressed(ImGuiKey_W) || ImGui::IsKeyPressed(ImGuiKey_UpArrow)) step(-1);
        if (ImGui::IsKeyPressed(ImGuiKey_S) || ImGui::IsKeyPressed(ImGuiKey_DownArrow)) step(+1);
        const ImVec2 mouseMoved = ImGui::GetIO().MouseDelta;
        for (int i = 0; i < kMainCount; ++i) {
            ImGui::SetCursorScreenPos(ImVec2(x0, rowsY + i * pitch));
            ImGui::PushID(i);
            const bool clicked = ImGui::InvisibleButton("row", ImVec2(kRowW * k, kRowH * k));
            ImGui::PopID();
            if (!kMainItems[i].desc) continue;
            if (ImGui::IsItemHovered() && (mouseMoved.x || mouseMoved.y)) g_mainAt = i;
            if (clicked) g_mainAt = i, c = kMainItems[i].choice;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
            ImGui::IsKeyPressed(ImGuiKey_Space, false))
            c = kMainItems[g_mainAt].choice;

        // The animation: the plate slides to the selected row, its marker slides in and its ink brightens, the one it
        // left fading back - each eased toward where it goes.
        const float ease = Ease();
        if (g_mainPlate < 0) {
            g_mainPlate = float(g_mainAt);
            for (int i = 0; i < kMainCount; ++i) g_mainOn[i] = i == g_mainAt ? 1.0f : 0.0f;
        }
        g_mainPlate += (g_mainAt - g_mainPlate) * ease;
        if (std::fabs(g_mainAt - g_mainPlate) < 0.002f) g_mainPlate = float(g_mainAt);
        Plate(d, ImVec2(x0, rowsY + g_mainPlate * pitch), kRowW * k, kRowH * k, kCut * k, kSurfaceRaised);

        for (int i = 0; i < kMainCount; ++i) {
            const MainItem& m = kMainItems[i];
            float& on = g_mainOn[i];
            on += ((i == g_mainAt ? 1.0f : 0.0f) - on) * ease;
            const float y = rowsY + i * pitch, mid = y + kRowH * k * 0.5f;
            Marker(d, x0 + 12 * k, mid, 12 * k, 34 * k, on, k);
            const ImU32 ink = m.desc ? Lerp(kInkSecondary, kInk, on) : Alpha(kInkMuted, 0.55f);
            Tracked(d, item, 28 * k, ImVec2(x0 + 46 * k, mid - 28 * k * 0.5f), ink, m.label, 0.02f);
        }

        // What the selected one does, then what is missing (the note), in the signal.
        float y = rowsY + kMainCount * pitch - kRowGap * k + 24 * k;
        const float wrap = 560 * k;
        if (const char* desc = kMainItems[g_mainAt].desc) {
            d->AddText(text, 23 * k, ImVec2(x0 + 46 * k, y), kInkSecondary, desc, nullptr, wrap);
            y += std::max(64 * k, text->CalcTextSizeA(23 * k, FLT_MAX, wrap, desc).y);
        }
        if (!note.empty()) d->AddText(text, 23 * k, ImVec2(x0 + 46 * k, y + 8 * k), kSignal, note.c_str(), nullptr, wrap);

        // The keys, at the column's foot.
        const float footY = H - 80 * k - 34 * k;
        float x = x0;
        x += KeyHint(d, text, k, ImVec2(x, footY), "W S", "MOVE") + 24 * k;
        KeyHint(d, text, k, ImVec2(x, footY), "ENTER", "SELECT");
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    return c;
}

namespace {

int g_capturing = -1;          // the action waiting for its key (Controls), -1 none
bool g_opened = false;         // its wait began this frame (the key that began it is not the binding)
std::string g_controlsNote;    // what the last binding did ("... was X's: that is unbound now")

int g_setTab = 0;                           // the tab: Content, Graphics, Sound, Controls
int g_setAt[4] = {};                        // each tab's selected row (Controls: the action; kActions: Default keys)
float g_setPlate[4] = {-1, 0, 0, 0};        // the selection's plate (x, y, w, h at 1080p), eased to the selected row's; -1 snap
constexpr int kSetRows = 64;
float g_setOn[kSetRows];                    // how selected each row looks, 0..1
float g_setKnob[kSetRows];                  // each switch's knob, 0 off .. 1 on
float g_tabOn[4] = {1, 0, 0, 0};            // how selected each tab looks
bool g_setFresh = true;                     // the tab just changed (its switches drawn where they are, not eased)
bool g_focusText = false;                   // Enter on a text row: its field takes the keyboard
static_assert(input::kActions + 1 <= kSetRows, "a row's look for each action and Default keys");

const char* const kTabs[4] = {"CONTENT", "GRAPHICS", "SOUND", "CONTROLS"};

// A row of a Settings tab: what it shows and what it changes.
struct SetRow {
    enum Kind { Heading, Text, Choice, Toggle, Slider, Action } kind = Heading;
    const char* label = "";
    const char* help = "";
    bool off = false;                       // shown, not changeable (the co-driver without sound, the content in a stage)
    std::string* text = nullptr;            // Text: the field, `hint` shown while it is empty
    std::string hint;
    int* at = nullptr;                      // Choice: the chosen one of `options`
    std::vector<std::string> options;
    bool* on = nullptr;                     // Toggle
    float* value = nullptr;                 // Slider: `min`..`max` in `step`s, shown with `format`
    float min = 0, max = 1, step = 0.1f;
    const char* format = "%.2f";
    SettingsChoice action = SettingsChoice::None;   // Action
    float y = 0, h = 0;                     // where it is (px at 1080p)
};

} // namespace

SettingsChoice SettingsUi(const ImGuiLayer& ui, app::Settings& s, bool inStage, const std::string& log, UINT keyDown,
                          int width, int height, const VideoFrame& video)
{
    using input::Action;
    const float k = std::min(height / 1080.0f, width / 1280.0f), W = float(width), H = float(height);
    const ImGuiIO& io = ImGui::GetIO();
    // A key wait or a text field has the keyboard: the menu's keys are theirs.
    const bool busy = g_capturing >= 0 || io.WantTextInput;
    SettingsChoice c = SettingsChoice::None;
    bool graphics = false;

    // Controls' wait for a key: Esc keeps the binding, Delete unbinds, any other key binds (and unbinds whoever had it).
    if (g_capturing >= 0) {
        if (g_opened) {
            g_opened = false;
        } else if (keyDown) {
            if (keyDown == input::kEscapeScan) {
                // (Esc is the menus': not bindable)
            } else if (keyDown == 0xe053) {   // Delete
                s.keys.scan[g_capturing] = 0;
            } else if (const Action other = s.keys.Bind(static_cast<Action>(g_capturing), keyDown); other != Action::Count) {
                g_controlsNote = input::KeyName(keyDown) + " was " + input::Info(other).label + "'s: that is unbound now";
            }
            g_capturing = -1;
        }
    }

    // The tab's rows, over copies of what the settings hold as other types.
    int quality = static_cast<int>(std::min(s.quality, 2u));
    const int tab = g_setTab;
    const bool keys = tab == 3;
    std::vector<SetRow> rows;
    auto add = [&](SetRow::Kind kind, const char* label, const char* help) -> SetRow& {
        rows.emplace_back();
        rows.back().kind = kind, rows.back().label = label, rows.back().help = help;
        return rows.back();
    };
    if (tab == 0) {
        SetRow& rbr = add(SetRow::Text, "RBR FOLDER", "Where Richard Burns Rally is: the stages, the cars and their physics are read from it.");
        rbr.text = &s.rbrPath, rbr.hint = "the RBR folder";
        // (The .ngg folders are W-Rally.ini's alone, for development: ngg_folders.)
        add(SetRow::Action, "RESCAN CONTENT", "Read the stages and the cars again from the RBR folder.").action = SettingsChoice::Rescan;
        for (SetRow& r : rows) {
            r.off = inStage;
            if (inStage) r.help = "The content can be changed from the main menu.";
        }
    } else if (tab == 1) {
        SetRow& q = add(SetRow::Choice, "QUALITY", "The renderer's level of detail. Graphics changes apply at once.");
        q.at = &quality, q.options = {"LOW", "MEDIUM", "HIGH"};
        add(SetRow::Toggle, "FULL SCREEN", "The whole screen, or a window on the desktop. Applies at once.").on = &s.fullscreen;
        add(SetRow::Toggle, "RAY TRACING", "Ray tracing in the renderer. Applies at once.").on = &s.rayTracing;
        add(SetRow::Toggle, "TAA", "Temporal anti-aliasing. Applies at once.").on = &s.taa;
        add(SetRow::Toggle, "SHADOWS", "Shadows. Applies at once.").on = &s.shadows;
        add(SetRow::Toggle, "VERTICAL SYNC", "Frames in step with the display. Applies at once.").on = &s.vsync;
    } else if (tab == 2) {
        add(SetRow::Toggle, "SOUND", "The game's sound. From the next stage.").on = &s.sound;
        add(SetRow::Heading, "CO-DRIVER", "");
        SetRow& notes = add(SetRow::Toggle, "PACENOTES", "The co-driver reads the pacenotes out.");
        notes.on = &s.codriver, notes.off = !s.sound;
        SetRow& replay = add(SetRow::Toggle, "IN REPLAYS TOO", "The pacenotes in replays too, not only in live runs.");
        replay.on = &s.codriverReplay, replay.off = !s.sound;
        SetRow& vol = add(SetRow::Slider, "VOLUME", "The co-driver's volume.");
        vol.value = &s.codriverVolume, vol.min = 0, vol.max = 1, vol.step = 0.05f, vol.format = "%.2f", vol.off = !s.sound;
        SetRow& lead = add(SetRow::Slider, "CALL AHEAD",
                           "How early each call comes, in seconds at the car's speed, and at least 60 m ahead. More: called earlier. "
                           "From the next stage.");
        lead.value = &s.codriverLead, lead.min = 1, lead.max = 6, lead.step = 0.5f, lead.format = "%.1f S", lead.off = !s.sound;
        add(SetRow::Heading, "PACENOTES ON THE HUD", "");
        SetRow& next = add(SetRow::Slider, "NEXT CALL",
                           "The next call shows this many seconds before it, at the car's speed. The current call shows as it is "
                           "spoken. Live runs.");
        next.value = &s.pacenoteNext, next.min = 0, next.max = 20, next.step = 1, next.format = "%.1f S";
    }

    // Where everything is (px at 1080p): the header, the tabs, then the rows from 294.
    const float x0 = 80, listY = 294, rowW = 1000;
    float y = listY;
    for (SetRow& r : rows) {
        r.y = y, r.h = r.kind == SetRow::Heading ? 48.0f : 60.0f;
        y += r.h + 4;
    }
    const float listEnd = y;
    // Controls: the actions in three columns of their groups, Default keys under them.
    struct KeyRow {
        float x, y, w, h;
        int col;
    };
    std::vector<KeyRow> keyRows(input::kActions + 1);
    std::vector<std::pair<std::string, ImVec2>> groupHeads;
    {
        constexpr float colW = 360, colGap = 30;
        float colY[3] = {listY, listY, listY};
        const char* group = "";
        int groupNo = -1, col = 0;
        for (int a = 0; a < input::kActions; ++a) {
            const input::ActionInfo& info = input::Info(static_cast<Action>(a));
            if (std::strcmp(group, info.group)) {
                group = info.group;
                col = std::min(2, (++groupNo + 1) / 2);   // Driving | Camera, Lights | Other, Debug
                groupHeads.emplace_back(group, ImVec2(x0 + col * (colW + colGap), colY[col]));
                colY[col] += 36;
            }
            keyRows[a] = {x0 + col * (colW + colGap), colY[col], colW, 38, col};
            colY[col] += 40;
        }
        const float bottom = std::max(colY[0], std::max(colY[1], colY[2]));
        keyRows[input::kActions] = {x0, bottom + 22, 0, 48, -1};   // (its width from its label, below)
    }
    ImFont* mark = ui.Wordmark();
    ImFont* item = ui.MenuItem();
    ImFont* text = ui.MenuText();
    keyRows[input::kActions].w = Tracked(nullptr, item, 26, ImVec2(), 0, "DEFAULT KEYS", 0.02f) + 48;

    // The selection: what is selectable, the selected row, how each kind of row changes.
    const int count = keys ? input::kActions + 1 : static_cast<int>(rows.size());
    auto selectable = [&](int i) { return keys || (rows[i].kind != SetRow::Heading && !rows[i].off); };
    int& at = g_setAt[tab];
    if (at >= count || (!keys && rows[at].kind == SetRow::Heading)) at = 0;
    auto move = [&](int dir) {
        for (int i = at + dir; i >= 0 && i < count; i += dir)
            if (selectable(i)) return void(at = i);
    };
    auto change = [&](SetRow& r, int dir) {   // A / D, the arrows: the next or previous choice, a step, off / on
        if (r.off) return;
        if (r.kind == SetRow::Choice) {
            const int n = static_cast<int>(r.options.size());
            *r.at = (*r.at + dir + n) % n;
        } else if (r.kind == SetRow::Slider) {
            *r.value = std::clamp(std::round((*r.value + dir * r.step) / r.step) * r.step, r.min, r.max);
        } else if (r.kind == SetRow::Toggle) {
            *r.on = dir > 0;
        } else {
            return;
        }
        graphics |= tab == 1;
    };
    auto activate = [&](int i) {   // Enter, Space, a click
        if (keys) {
            if (i < input::kActions) {
                g_capturing = i, g_opened = true;
            } else {
                s.keys = input::Bindings();
            }
            g_controlsNote.clear();
            return;
        }
        SetRow& r = rows[i];
        if (r.off) return;
        if (r.kind == SetRow::Toggle) *r.on = !*r.on, graphics |= tab == 1;
        else if (r.kind == SetRow::Choice) change(r, +1);
        else if (r.kind == SetRow::Action) c = r.action;
        else if (r.kind == SetRow::Text) g_focusText = true;
    };
    auto setTab = [&](int t) {
        if (t < 0 || t > 3 || t == g_setTab) return;
        g_setTab = t, g_setPlate[0] = -1, g_setFresh = true;
        for (float& on : g_setOn) on = 0;   // (the new tab's marker slides in)
    };

    // The keys: Q / E the tabs, W / S (the arrows) the rows, A / D (the arrows) the value - or, in Controls, the column -
    // Enter or Space does the row, Esc goes back.
    if (!busy) {
        if (ImGui::IsKeyPressed(ImGuiKey_Q, false)) setTab(tab - 1);
        if (ImGui::IsKeyPressed(ImGuiKey_E, false)) setTab(tab + 1);
        if (g_setTab == tab) {
            if (ImGui::IsKeyPressed(ImGuiKey_W) || ImGui::IsKeyPressed(ImGuiKey_UpArrow)) move(-1);
            if (ImGui::IsKeyPressed(ImGuiKey_S) || ImGui::IsKeyPressed(ImGuiKey_DownArrow)) move(+1);
            for (int dir = -1; dir <= 1; dir += 2) {
                if (!ImGui::IsKeyPressed(dir < 0 ? ImGuiKey_A : ImGuiKey_D) && !ImGui::IsKeyPressed(dir < 0 ? ImGuiKey_LeftArrow : ImGuiKey_RightArrow))
                    continue;
                if (!keys) {
                    change(rows[at], dir);
                } else if (at < input::kActions) {   // the action at the nearest height in the next column
                    const int col = keyRows[at].col + dir;
                    int best = -1;
                    for (int a = 0; a < input::kActions; ++a)
                        if (keyRows[a].col == col && (best < 0 || std::fabs(keyRows[a].y - keyRows[at].y) < std::fabs(keyRows[best].y - keyRows[at].y)))
                            best = a;
                    if (best >= 0) at = best;
                }
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
                ImGui::IsKeyPressed(ImGuiKey_Space, false))
                activate(at);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) c = SettingsChoice::Back;
    }

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(W, H));
    constexpr ImGuiWindowFlags kFull = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    if (ImGui::Begin("Settings", nullptr, kFull)) {
        ImDrawList* d = ImGui::GetWindowDrawList();
        auto P = [&](float px, float py) { return ImVec2(px * k, py * k); };   // (1080p px -> the window's)
        const bool mouseMoved = io.MouseDelta.x || io.MouseDelta.y;
        const bool click = !busy && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        auto over = [&](ImVec2 a, ImVec2 b) { return !busy && ImGui::IsMouseHoveringRect(a, b, false); };

        // The ground (over a stage: its scrim) and the livery where the main menu has it - without the video's stripe,
        // which the rows run over: it fades out (over a stage it was never there).
        if (inStage) HideVideoStripe();
        Backdrop(d, W, H, k, inStage ? IM_COL32(10, 10, 11, 235) : kSurfaceBase, video, false);

        // The header: where the screen is, its title.
        Tracked(d, text, 17 * k, P(x0, 80), kInkMuted, inStage ? "W-RALLY \xC2\xB7 PAUSED" : "W-RALLY \xC2\xB7 MAIN MENU", 0.16f);
        d->AddText(mark, 48 * k, P(x0, 108), kInk, "SETTINGS");

        // The tabs between their keys: the selected one's ink and its underline eased in.
        const float ease = Ease();
        float tx = x0 * k;
        tx += KeyHint(d, text, k, ImVec2(tx, 206 * k), "Q", "") + 28 * k;
        for (int t = 0; t < 4; ++t) {
            float& on = g_tabOn[t];
            on += ((t == g_setTab ? 1.0f : 0.0f) - on) * ease;
            const float w = Tracked(nullptr, item, 28 * k, ImVec2(), 0, kTabs[t], 0.02f);
            Tracked(d, item, 28 * k, ImVec2(tx, 206 * k), Lerp(kInkMuted, kInk, on), kTabs[t], 0.02f);
            if (on > 0.01f) {
                const float by = 246 * k, bh = 4 * k, lean = bh * 0.5f * kTanPlate;
                d->AddQuadFilled(ImVec2(tx + lean, by), ImVec2(tx + w * on + lean, by), ImVec2(tx + w * on - lean, by + bh),
                                 ImVec2(tx - lean, by + bh), kSignal);
            }
            if (click && over(ImVec2(tx - 4 * k, 198 * k), ImVec2(tx + w + 4 * k, 252 * k))) setTab(t);
            tx += w + 36 * k;
        }
        KeyHint(d, text, k, ImVec2(tx - 8 * k, 206 * k), "E", "");
        d->AddRectFilled(P(x0, 262), P(x0 + rowW, 263), kLine);

        // The selection's plate, eased to the selected row (snapped on a new tab).
        float target[4];
        if (keys) target[0] = keyRows[at].x, target[1] = keyRows[at].y, target[2] = keyRows[at].w, target[3] = keyRows[at].h;
        else target[0] = x0, target[1] = rows[at].y, target[2] = rowW, target[3] = rows[at].h;
        const bool snap = g_setPlate[0] < 0;
        for (int i = 0; i < 4; ++i) g_setPlate[i] = snap ? target[i] : g_setPlate[i] + (target[i] - g_setPlate[i]) * ease;
        if (keys || rows[at].kind != SetRow::Heading)
            Plate(d, P(g_setPlate[0], g_setPlate[1]), g_setPlate[2] * k, g_setPlate[3] * k, (keys ? 10 : 12) * k, kSurfaceRaised);

        if (!keys) {
            const float xr = (x0 + rowW - 24) * k;   // the values' right edge
            for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
                SetRow& r = rows[i];
                const float top = r.y * k, mid = (r.y + r.h * 0.5f) * k;
                if (r.kind == SetRow::Heading) {
                    Tracked(d, text, 17 * k, P(x0 + 12, r.y + 20), kInkMuted, r.label, 0.16f);
                    continue;
                }
                float& on = g_setOn[i];
                on += ((i == at ? 1.0f : 0.0f) - on) * ease;
                const float fade = r.off ? 0.45f : 1.0f;
                if (selectable(i) && mouseMoved && over(ImVec2(x0 * k, top), ImVec2((x0 + rowW) * k, top + r.h * k))) at = i;
                Marker(d, (x0 + 12) * k, mid, 12 * k, 34 * k, on, k);
                Tracked(d, item, 26 * k, ImVec2((x0 + 46) * k, mid - 13 * k), Alpha(Lerp(kInkSecondary, kInk, on), fade), r.label, 0.02f);

                // An arrow button `size` px square at `cx` (its centre): drawn, and its click.
                auto arrow = [&](float cx, bool right) {
                    const ImVec2 a(cx - 22 * k, mid - 22 * k), b(cx + 22 * k, mid + 22 * k);
                    const bool hot = !r.off && over(a, b);
                    Chevron(d, ImVec2(cx, mid), 22 * k, right, Alpha(hot ? kInk : kInkSecondary, fade));
                    if (hot && click) at = i, change(r, right ? +1 : -1);
                };
                if (r.kind == SetRow::Toggle) {
                    float& knob = g_setKnob[i];
                    knob = g_setFresh ? (*r.on ? 1.0f : 0.0f) : knob + ((*r.on ? 1.0f : 0.0f) - knob) * ease;
                    const ImVec2 a(xr - 60 * k, mid - 15 * k), b(xr, mid + 15 * k);
                    const float cut = 8 * k, bw = 2 * k;
                    const ImVec2 track[4] = {a, ImVec2(b.x, a.y), ImVec2(b.x - cut, b.y), ImVec2(a.x, b.y)};
                    if (knob > 0.01f) d->AddConvexPolyFilled(track, 4, Alpha(kSignal, knob * fade));
                    d->AddPolyline(track, 4, Alpha(Lerp(kLineStrong, kSignal, knob), fade), ImDrawFlags_Closed, bw);
                    const float kx = a.x + (4 + 26 * knob) * k, half = 10 * k, lean = half * kTanPlate;
                    d->AddQuadFilled(ImVec2(kx + lean, mid - half), ImVec2(kx + 18 * k + lean, mid - half), ImVec2(kx + 18 * k - lean, mid + half),
                                     ImVec2(kx - lean, mid + half), Alpha(Lerp(kInkSecondary, kSurfaceBase, knob), fade));
                    const char* shown = *r.on ? "ON" : "OFF";
                    const float sw = Tracked(nullptr, item, 22 * k, ImVec2(), 0, shown, 0.02f);
                    Tracked(d, item, 22 * k, ImVec2(a.x - 16 * k - sw, mid - 11 * k), Alpha(kInk, fade), shown, 0.02f);
                    if (!r.off && click && over(ImVec2(a.x - 80 * k, mid - 22 * k), ImVec2(b.x, mid + 22 * k))) at = i, *r.on = !*r.on, graphics |= tab == 1;
                } else if (r.kind == SetRow::Choice) {
                    const int n = static_cast<int>(r.options.size());
                    const float right = xr - 22 * k, left = xr - (44 + 8 + 220 + 8 + 22) * k, cx = xr - (44 + 8 + 110) * k;
                    arrow(left, false);
                    arrow(right, true);
                    const char* shown = r.options[*r.at].c_str();
                    const float sw = Tracked(nullptr, item, 22 * k, ImVec2(), 0, shown, 0.02f);
                    Tracked(d, item, 22 * k, ImVec2(cx - sw * 0.5f, mid - 16 * k), Alpha(kInk, fade), shown, 0.02f);
                    const float pw = (n > 4 ? 18 : 40) * k, ph = 4 * k, total = n * pw + (n - 1) * 4 * k, lean = ph * 0.5f * kTanPlate;
                    for (int p = 0; p < n; ++p) {
                        const float px = cx - total * 0.5f + p * (pw + 4 * k), py = mid + 12 * k;
                        d->AddQuadFilled(ImVec2(px + lean, py), ImVec2(px + pw + lean, py), ImVec2(px + pw - lean, py + ph), ImVec2(px - lean, py + ph),
                                         Alpha(p == *r.at ? kSignal : kLineStrong, fade));
                    }
                } else if (r.kind == SetRow::Slider) {
                    char shown[32];
                    std::snprintf(shown, sizeof shown, r.format, *r.value);
                    const float sw = Tracked(nullptr, item, 22 * k, ImVec2(), 0, shown, 0.02f);
                    Tracked(d, item, 22 * k, ImVec2(xr - sw, mid - 11 * k), Alpha(kInk, fade), shown, 0.02f);
                    const float plus = xr - (96 + 8 + 22) * k, segs = plus - (22 + 8) * k - 276 * k, minus = segs - (8 + 22) * k;
                    arrow(minus, false);
                    arrow(plus, true);
                    const int lit = static_cast<int>(std::round((*r.value - r.min) / (r.max - r.min) * 20));
                    const float half = 12 * k, lean = half * kTanPlate;
                    for (int g = 0; g < 20; ++g) {
                        const float gx = segs + g * 14 * k;
                        d->AddQuadFilled(ImVec2(gx + lean, mid - half), ImVec2(gx + 10 * k + lean, mid - half), ImVec2(gx + 10 * k - lean, mid + half),
                                         ImVec2(gx - lean, mid + half), Alpha(g < lit ? kSignal : kLine, fade));
                    }
                } else if (r.kind == SetRow::Action) {
                    Chevron(d, ImVec2(xr - 11 * k, mid), 22 * k, true, Alpha(Lerp(kInkSecondary, kInk, on), fade));
                    if (!r.off && click && over(ImVec2(x0 * k, top), ImVec2((x0 + rowW) * k, top + r.h * k))) at = i, activate(i);
                } else if (r.kind == SetRow::Text) {
                    // The field: ImGui's, in the design's well (surface-overlay, a line-strong border), the menu text at 20 px.
                    ImGui::PushID(i);
                    ImGui::PushFont(text);
                    ImGui::SetWindowFontScale(20 * k / text->FontSize);
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14 * k, 11 * k));
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, std::max(1.0f, k));
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0);
                    const ImVec4 well = ImGui::ColorConvertU32ToFloat4(kSurfaceOverlay);
                    ImGui::PushStyleColor(ImGuiCol_FrameBg, well);
                    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, well);
                    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, well);
                    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(i == at ? kInkSecondary : kLineStrong));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Alpha(kInk, fade)));
                    ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImGui::ColorConvertU32ToFloat4(kInkMuted));
                    ImGui::SetCursorScreenPos(ImVec2(xr - 460 * k, mid - 21 * k));
                    ImGui::SetNextItemWidth(460 * k);
                    if (g_focusText && i == at) ImGui::SetKeyboardFocusHere(), g_focusText = false;
                    char buf[1024];
                    std::snprintf(buf, sizeof buf, "%s", r.text->c_str());
                    if (ImGui::InputTextWithHint("##field", r.hint.c_str(), buf, sizeof buf, r.off ? ImGuiInputTextFlags_ReadOnly : 0))
                        *r.text = buf;
                    if (ImGui::IsItemActive() && !r.off) at = i;
                    ImGui::PopStyleColor(6);
                    ImGui::PopStyleVar(3);
                    ImGui::SetWindowFontScale(1);
                    ImGui::PopFont();
                    ImGui::PopID();
                }
            }
            // The last Rescan's report, under the content.
            if (tab == 0 && !log.empty()) {
                const float by = listEnd + 12, wrap = (rowW - 48) * k;
                const float th = text->CalcTextSizeA(20 * k, FLT_MAX, wrap, log.c_str()).y;
                d->AddRectFilled(P(x0, by), ImVec2((x0 + rowW) * k, by * k + 16 * k + 20 * k + 8 * k + th + 16 * k), kSurfaceRaised);
                Tracked(d, text, 17 * k, P(x0 + 24, by + 16), kInkMuted, "LAST RESCAN", 0.16f);
                d->AddText(text, 20 * k, P(x0 + 24, by + 16 + 28), kInkSecondary, log.c_str(), nullptr, wrap);
            }
        } else {
            for (auto [label, pos] : groupHeads) {   // (overlines: upper case)
                for (char& ch : label) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                Tracked(d, text, 17 * k, ImVec2((pos.x + 12) * k, (pos.y + 10) * k), kInkMuted, label.c_str(), 0.16f);
            }
            for (int a = 0; a <= input::kActions; ++a) {
                const KeyRow& kr = keyRows[a];
                const ImVec2 lo = P(kr.x, kr.y), hi = P(kr.x + kr.w, kr.y + kr.h);
                const float mid = (lo.y + hi.y) * 0.5f;
                float& on = g_setOn[a];
                on += ((a == at ? 1.0f : 0.0f) - on) * ease;
                if (over(lo, hi)) {
                    if (mouseMoved) at = a;
                    if (click) at = a, activate(a);
                }
                if (a == input::kActions) {   // Default keys: a secondary button, cut like the plates
                    const ImU32 fill = Lerp(kSurfaceOverlay, kSurfaceRaised, on);
                    Plate(d, lo, hi.x - lo.x, hi.y - lo.y, 12 * k, fill);
                    const ImVec2 q[4] = {lo, ImVec2(hi.x, lo.y), ImVec2(hi.x - 12 * k, hi.y), ImVec2(lo.x, hi.y)};
                    d->AddPolyline(q, 4, Lerp(kLineStrong, kInkSecondary, on), ImDrawFlags_Closed, std::max(1.0f, k));
                    Tracked(d, item, 26 * k, ImVec2(lo.x + 20 * k, mid - 13 * k), kInk, "DEFAULT KEYS", 0.02f);
                    continue;
                }
                const input::ActionInfo& info = input::Info(static_cast<Action>(a));
                Marker(d, lo.x + 10 * k, mid, 8 * k, 24 * k, on, k);
                d->AddText(text, 20 * k, ImVec2(lo.x + 30 * k, mid - 10 * k), Lerp(kInkSecondary, kInk, on), info.label);
                // Its key in a chip at the right; unbound in the signal.
                const UINT scan = s.keys.scan[a];
                const std::string name = scan ? input::KeyName(scan) : "UNBOUND";
                const float size = 16 * k, ch = 28 * k, pad = 10 * k;
                const float tw = Tracked(nullptr, text, size, ImVec2(), 0, name.c_str(), 0.08f), cw = std::max(34 * k, tw + 2 * pad);
                const ImVec2 c0(hi.x - 12 * k - cw, mid - ch * 0.5f), c1(hi.x - 12 * k, mid + ch * 0.5f);
                d->AddRectFilled(c0, c1, kSurfaceOverlay, 2 * k);
                d->AddRect(c0, c1, scan ? kLineStrong : kSignal, 2 * k, 0, std::max(1.0f, k));
                Tracked(d, text, size, ImVec2(c0.x + (cw - tw) * 0.5f, mid - size * 0.5f), scan ? kInk : kSignal, name.c_str(), 0.08f);
            }
        }

        // What the selected row does (or what the last binding did), then the keys at the column's foot.
        const float footY = H - (80 + 34) * k, helpY = footY - (24 + 64) * k;
        const char* help = keys ? "Enter waits for a key for the selected action. Esc keeps its key, Delete unbinds it."
                                : rows[at].help;
        if (keys && !g_controlsNote.empty())
            d->AddText(text, 23 * k, ImVec2((x0 + 46) * k, helpY), kSignal, g_controlsNote.c_str(), nullptr, 900 * k);
        else
            d->AddText(text, 23 * k, ImVec2((x0 + 46) * k, helpY), kInkSecondary, help, nullptr, 900 * k);
        float fx = x0 * k;
        fx += KeyHint(d, text, k, ImVec2(fx, footY), "Q E", "TAB") + 28 * k;
        fx += KeyHint(d, text, k, ImVec2(fx, footY), "W S", "MOVE") + 28 * k;
        fx += KeyHint(d, text, k, ImVec2(fx, footY), "A D", "CHANGE") + 28 * k;
        fx += KeyHint(d, text, k, ImVec2(fx, footY), "ENTER", "SELECT") + 28 * k;
        const float back = KeyHint(d, text, k, ImVec2(fx, footY), "ESC", "BACK");
        if (click && over(ImVec2(fx, footY), ImVec2(fx + back, footY + 34 * k))) c = SettingsChoice::Back;

        // Controls' wait for a key: over everything, its own plate.
        if (g_capturing >= 0) {
            d->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), IM_COL32(10, 10, 11, 217));
            const ImVec2 a((W - 720 * k) * 0.5f, (H - 220 * k) * 0.5f);
            Plate(d, a, 720 * k, 220 * k, 12 * k, kSurfaceRaised);
            d->AddRectFilled(a, ImVec2(a.x + 6 * k, a.y + 220 * k), kSignal);
            Tracked(d, text, 17 * k, ImVec2(a.x + 40 * k, a.y + 32 * k), kInkMuted, "CONTROLS", 0.16f);
            std::string ask = std::string("PRESS A KEY FOR ") + input::Info(static_cast<Action>(g_capturing)).label;
            for (char& ch : ask) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            Tracked(d, item, 28 * k, ImVec2(a.x + 40 * k, a.y + 64 * k), kInk, ask.c_str(), 0.02f);
            const UINT scan = s.keys.scan[g_capturing];
            const std::string keep = "KEEP " + (scan ? input::KeyName(scan) : std::string("UNBOUND"));
            float hx = a.x + 40 * k;
            hx += KeyHint(d, text, k, ImVec2(hx, a.y + 150 * k), "ESC", keep.c_str()) + 28 * k;
            KeyHint(d, text, k, ImVec2(hx, a.y + 150 * k), "DELETE", "UNBOUND");
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    // A tab chosen this frame (its rows are drawn from the next): its plate and its switches snap there.
    g_setFresh = g_setTab != tab;
    if (g_setFresh) g_setPlate[0] = -1;

    // The copies back into the settings; a graphics change to apply now.
    if (static_cast<uint32_t>(quality) != s.quality) s.quality = static_cast<uint32_t>(quality), graphics = true;
    if (graphics && c == SettingsChoice::None) c = SettingsChoice::Graphics;
    return c;
}

bool CapturingKey() { return g_capturing >= 0 || ImGui::GetIO().WantTextInput; }

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
