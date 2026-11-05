// Quick rally and its pages (ui/menus.h QuickRally), drawn to the W-Rally design: the choice of a stage, a car, its
// setup, the damage and the weather, and the pages they open - Stage (a collection, then its stages), Garage (every
// car), Car setup (the chosen setup's values, the car's lamps) and Weather (custom: the preset's sky changed, the time,
// the season, the road). The pages are drawn where the main menu's livery is; the video's stripe is the preview's
// place (the stage's picture, the car rendered: later - the video loop meanwhile).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "assets/car_lights.h"
#include "data/lsp.h"
#include "imgui.h"
#include "ui/imgui_layer.h"
#include "ui/menu_look.h"
#include "ui/menus.h"

namespace wrally::ui {

namespace {

enum class Page { Quick, Stage, Garage, Setup, Weather };
Page g_page = Page::Quick;

// A row of a page: what it shows and what it changes.
struct Row {
    enum Kind { Heading, Open, Choice, Slider, Value } kind = Heading;
    std::string label, help;
    std::string shown, sub;                 // Open, Value: the value and the line under it
    std::string shown2;                     // Value: a second column's (the rear's beside the front's; "" one column)
    bool subSignal = false;                 // the sub line in the signal (what Enter does)
    int* at = nullptr;                      // Choice: the chosen one of `options`
    std::vector<std::string> options;
    float* value = nullptr;                 // Slider: `min`..`max` in `step`s, shown by `format`
    float min = 0, max = 1, step = 0.1f;
    std::string (*format)(float) = nullptr;
    bool off = false;                       // shown, not changeable
    float y = 0, h = 0;                     // where it is (px at 1080p)
};

// A list's selection: the selected row, the plate eased to it, how selected each row looks.
struct List {
    int at = 0;
    float plateY = -1, plateH = 0;          // -1: snap
    float on[64] = {};
};

bool Selectable(const Row& r) { return r.kind != Row::Heading && !r.off; }

void Move(List& l, const std::vector<Row>& rows, int dir)
{
    for (int i = l.at + dir; i >= 0 && i < static_cast<int>(rows.size()); i += dir)
        if (Selectable(rows[i])) return void(l.at = i);
}

void Change(Row& r, int dir)
{
    if (r.off) return;
    if (r.kind == Row::Choice && !r.options.empty()) {
        const int n = static_cast<int>(r.options.size());
        *r.at = (*r.at + dir + n) % n;
    } else if (r.kind == Row::Slider) {
        *r.value = std::clamp(std::round((*r.value + dir * r.step) / r.step) * r.step, r.min, r.max);
    }
}

bool KeyW() { return ImGui::IsKeyPressed(ImGuiKey_W) || ImGui::IsKeyPressed(ImGuiKey_UpArrow); }
bool KeyS() { return ImGui::IsKeyPressed(ImGuiKey_S) || ImGui::IsKeyPressed(ImGuiKey_DownArrow); }
bool KeyA() { return ImGui::IsKeyPressed(ImGuiKey_A) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow); }
bool KeyD() { return ImGui::IsKeyPressed(ImGuiKey_D) || ImGui::IsKeyPressed(ImGuiKey_RightArrow); }
bool KeyEnter()
{
    return ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
           ImGui::IsKeyPressed(ImGuiKey_Space, false);
}
bool KeyEsc() { return ImGui::IsKeyPressed(ImGuiKey_Escape, false); }

// The fonts and the scale a page draws with.
struct Look {
    ImDrawList* d;
    ImFont *mark, *item, *text;
    float k, W, H;
    ImVec2 P(float x, float y) const { return ImVec2(x * k, y * k); }
};

// A full-window screen: `body` draws it on the window's draw list.
template <class F> void FullScreen(const char* id, const ImGuiLayer& ui, int width, int height, F&& body)
{
    const float k = std::min(height / 1080.0f, width / 1280.0f);
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(float(width), float(height)));
    constexpr ImGuiWindowFlags kFull = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    if (ImGui::Begin(id, nullptr, kFull)) {
        const Look l{ImGui::GetWindowDrawList(), ui.Wordmark(), ui.MenuItem(), ui.MenuText(), k, float(width), float(height)};
        body(l);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

constexpr float kX0 = 80;   // the column's left (px at 1080p)

// The header: where the page is, its title.
void Header(const Look& l, const char* where, const char* title)
{
    Tracked(l.d, l.text, 17 * l.k, l.P(kX0, 80), kInkMuted, where, 0.16f);
    l.d->AddText(l.mark, 48 * l.k, l.P(kX0, 108), kInk, title);
}

// The keys at the foot: (key, what it does) pairs; the last one's chip clickable as Esc. True when it was clicked.
bool Footer(const Look& l, std::initializer_list<std::pair<const char*, const char*>> keys)
{
    float x = kX0 * l.k;
    const float y = l.H - (80 + 34) * l.k;
    bool clicked = false;
    for (const auto& [key, does] : keys) {
        const float w = KeyHint(l.d, l.text, l.k, ImVec2(x, y), key, does);
        if (!std::strcmp(key, "ESC") && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
            ImGui::IsMouseHoveringRect(ImVec2(x, y), ImVec2(x + w, y + 34 * l.k), false))
            clicked = true;
        x += w + 28 * l.k;
    }
    return clicked;
}

// The help line over the foot.
void Help(const Look& l, const std::string& help)
{
    if (!help.empty())
        l.d->AddText(l.text, 23 * l.k, l.P(kX0 + 46, 1080 - 80 - 34 - 24 - 96), kInkSecondary, help.c_str(), nullptr, 680 * l.k);
}

// The rows from `y0`, `w` wide (px at 1080p): laid out, drawn, the mouse's. Returns the row clicked to be done (-1 none).
int Rows(const Look& l, std::vector<Row>& rows, List& list, float y0, float w, float rowH)
{
    const float k = l.k;
    float y = y0;
    for (Row& r : rows) {
        r.y = y, r.h = r.kind == Row::Heading ? 44.0f : rowH;
        y += r.h + 4;
    }
    if (list.at >= static_cast<int>(rows.size()) || !Selectable(rows[list.at])) {
        list.at = 0;
        while (list.at < static_cast<int>(rows.size()) - 1 && !Selectable(rows[list.at])) ++list.at;
    }
    const ImGuiIO& io = ImGui::GetIO();
    const bool moved = io.MouseDelta.x || io.MouseDelta.y, click = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    auto over = [&](ImVec2 a, ImVec2 b) { return ImGui::IsMouseHoveringRect(a, b, false); };
    const float ease = Ease();
    int done = -1;

    if (!rows.empty()) {
        const Row& sel = rows[list.at];
        if (list.plateY < 0) list.plateY = sel.y, list.plateH = sel.h;
        list.plateY += (sel.y - list.plateY) * ease, list.plateH += (sel.h - list.plateH) * ease;
        if (sel.kind != Row::Heading) Plate(l.d, l.P(kX0, list.plateY), w * k, list.plateH * k, 12 * k, kSelect);
    }
    const float xr = (kX0 + w - 24) * k;   // the values' right edge
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        Row& r = rows[i];
        const float top = r.y * k, mid = (r.y + r.h * 0.5f) * k;
        if (r.kind == Row::Heading) {
            Tracked(l.d, l.text, 17 * k, l.P(kX0 + 12, r.y + 18), kInkMuted, r.label.c_str(), 0.16f);
            continue;
        }
        float& on = list.on[std::min(i, 63)];
        on += ((i == list.at ? 1.0f : 0.0f) - on) * ease;
        const float fade = r.off ? 0.45f : 1.0f;
        const bool hot = over(ImVec2(kX0 * k, top), ImVec2((kX0 + w) * k, top + r.h * k));
        if (Selectable(r) && moved && hot) list.at = i;
        Marker(l.d, (kX0 + 12) * k, mid, 12 * k, 34 * k, on, k);
        Tracked(l.d, l.item, 26 * k, ImVec2((kX0 + 46) * k, mid - 13 * k), Alpha(Lerp(kInkSecondary, kInk, on), fade), r.label.c_str(), 0.02f);

        auto arrow = [&](float cx, bool right) {
            const ImVec2 a(cx - 22 * k, mid - 22 * k), b(cx + 22 * k, mid + 22 * k);
            const bool h = !r.off && over(a, b);
            Chevron(l.d, ImVec2(cx, mid), 22 * k, right, Alpha(h ? kInk : kInkSecondary, fade));
            if (h && click) list.at = i, Change(r, right ? +1 : -1);
            return h;
        };
        bool onArrow = false;
        if (r.kind == Row::Value && !r.shown2.empty()) {   // two columns: the front's, the rear's
            for (int col = 0; col < 2; ++col) {
                const std::string& v = col ? r.shown2 : r.shown;
                const float sw = Tracked(nullptr, l.item, 22 * k, ImVec2(), 0, v.c_str(), 0.02f);
                Tracked(l.d, l.item, 22 * k, ImVec2(xr - (col ? 0 : 180) * k - sw, mid - 11 * k), Alpha(kInk, fade), v.c_str(), 0.02f);
            }
        } else if (r.kind == Row::Open || r.kind == Row::Value) {
            const bool open = r.kind == Row::Open;
            const float right = open ? xr - 34 * k : xr;
            const float sw = Tracked(nullptr, l.item, 22 * k, ImVec2(), 0, r.shown.c_str(), 0.02f);
            const float sy = r.sub.empty() ? mid - 11 * k : mid - 20 * k;
            Tracked(l.d, l.item, 22 * k, ImVec2(right - sw, sy), Alpha(kInk, fade), r.shown.c_str(), 0.02f);
            if (!r.sub.empty()) {
                const float bw = Tracked(nullptr, l.text, 14 * k, ImVec2(), 0, r.sub.c_str(), 0.16f);
                Tracked(l.d, l.text, 14 * k, ImVec2(right - bw, mid + 8 * k), r.subSignal ? kSignal : kInkMuted, r.sub.c_str(), 0.16f);
            }
            if (open) Chevron(l.d, ImVec2(xr - 11 * k, mid), 22 * k, true, Alpha(Lerp(kInkSecondary, kInk, on), fade));
        } else if (r.kind == Row::Choice) {
            const int n = static_cast<int>(r.options.size());
            const float right = xr - 22 * k, left = xr - (44 + 8 + 230 + 8 + 22) * k, cx = xr - (44 + 8 + 115) * k;
            onArrow |= arrow(left, false);
            onArrow |= arrow(right, true);
            const char* shown = n ? r.options[std::clamp(*r.at, 0, n - 1)].c_str() : "-";
            const float sw = Tracked(nullptr, l.item, 22 * k, ImVec2(), 0, shown, 0.02f);
            const bool sub = !r.sub.empty();
            Tracked(l.d, l.item, 22 * k, ImVec2(cx - sw * 0.5f, mid - (sub ? 24 : 16) * k), Alpha(kInk, fade), shown, 0.02f);
            if (n > 1 && n <= 12) {
                const float pw = (n > 4 ? 14 : 40) * k, ph = 4 * k, total = n * pw + (n - 1) * 4 * k, lean = ph * 0.5f * kTanPlate;
                const float py = mid + (sub ? 4 : 12) * k;
                for (int p = 0; p < n; ++p) {
                    const float px = cx - total * 0.5f + p * (pw + 4 * k);
                    l.d->AddQuadFilled(ImVec2(px + lean, py), ImVec2(px + pw + lean, py), ImVec2(px + pw - lean, py + ph),
                                       ImVec2(px - lean, py + ph), Alpha(p == *r.at ? kSignal : kLineStrong, fade));
                }
            }
            if (sub) {
                const float bw = Tracked(nullptr, l.text, 14 * k, ImVec2(), 0, r.sub.c_str(), 0.16f);
                Tracked(l.d, l.text, 14 * k, ImVec2(cx - bw * 0.5f, mid + 14 * k), r.subSignal ? kSignal : kInkMuted, r.sub.c_str(), 0.16f);
            }
        } else if (r.kind == Row::Slider) {
            const std::string shown = r.format ? r.format(*r.value) : std::to_string(*r.value);
            const float sw = Tracked(nullptr, l.item, 22 * k, ImVec2(), 0, shown.c_str(), 0.02f);
            Tracked(l.d, l.item, 22 * k, ImVec2(xr - sw, mid - 11 * k), Alpha(kInk, fade), shown.c_str(), 0.02f);
            const float plus = xr - (110 + 8 + 22) * k, segs = plus - (22 + 8) * k - 236 * k, minus = segs - (8 + 22) * k;
            onArrow |= arrow(minus, false);
            onArrow |= arrow(plus, true);
            const int lit = static_cast<int>(std::round((*r.value - r.min) / (r.max - r.min) * 20));
            const float half = 11 * k, lean = half * kTanPlate;
            for (int g = 0; g < 20; ++g) {
                const float gx = segs + g * 12 * k;
                l.d->AddQuadFilled(ImVec2(gx + lean, mid - half), ImVec2(gx + 8 * k + lean, mid - half), ImVec2(gx + 8 * k - lean, mid + half),
                                   ImVec2(gx - lean, mid + half), Alpha(g < lit ? kSignal : kLine, fade));
            }
        }
        if (hot && click && !onArrow && Selectable(r)) list.at = i, done = i;
    }
    return done;
}

// The keys of a list of rows: W / S the rows, A / D the value. Returns the row Enter does (-1 none).
int ListKeys(std::vector<Row>& rows, List& list)
{
    if (rows.empty()) return -1;
    list.at = std::clamp(list.at, 0, static_cast<int>(rows.size()) - 1);   // (the rows may be fewer than last time)
    if (KeyW()) Move(list, rows, -1);
    if (KeyS()) Move(list, rows, +1);
    if (KeyA()) Change(rows[list.at], -1);
    if (KeyD()) Change(rows[list.at], +1);
    return KeyEnter() ? list.at : -1;
}

std::string Upper(std::string s)
{
    for (char& c : s)
        if (static_cast<unsigned char>(c) < 0x80) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string FileName(const std::string& path)
{
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string Fmt(const char* format, float x);   // (below: a value, "-" for NaN)

const char* const kDamage[4] = {"OFF", "SAFE", "REDUCED", "REALISTIC"};
const char* const kSeasons[4] = {"SPRING", "SUMMER", "AUTUMN", "WINTER"};
const char* const kRoads[4] = {"FROM THE WEATHER", "DRY", "DAMP", "WET"};

// ---------------------------------------------------------------- Quick rally

List g_quick;

QuickChoice QuickPage(const ImGuiLayer& ui, const assets::Catalog& catalog, app::Settings& s, int width, int height,
                      const VideoFrame& video)
{
    QuickChoice c = QuickChoice::None;
    const assets::Stage* st = catalog.FindStage(s.stage);
    if (!st && !catalog.stages.empty()) st = &catalog.stages.front(), s.stage = st->key;
    const assets::Car* car = catalog.FindCar(s.car);
    auto usableCar = [&](const assets::Car& c) {
        const assets::CarInfo* info = catalog.carRegister.Find(c.folder);
        return info && info->known && assets::Catalog::Drivable(c);
    };
    if (!car)   // none chosen yet: the first W-Rally can use
        for (const assets::Car& each : catalog.cars)
            if (usableCar(each)) {
                car = &each, s.car = each.folder;
                break;
            }
    const bool carOk = car && usableCar(*car);

    std::vector<Row> rows(7);
    Row& stage = rows[0];
    stage.kind = Row::Open, stage.label = "STAGE";
    stage.shown = st ? Upper(st->title) : "NO STAGES";
    stage.sub = st ? (st->imported ? "RBR STAGE" : ".NGG STAGE") + std::string(st->map >= 0 ? " \xC2\xB7 MAP " + std::to_string(st->map) : "") : "SEE SETTINGS";
    stage.help = "Enter opens the stages: a collection first, then its stages.";
    Row& carRow = rows[1];
    carRow.kind = Row::Open, carRow.label = "CAR";
    carRow.shown = car ? Upper(car->name) : "NO CARS";
    carRow.sub = !car ? "SEE SETTINGS" : carOk ? "PHYSICS: " + Upper(car->physicsFrom) : "NOT USABLE BY W-RALLY";
    carRow.subSignal = car && !carOk;
    carRow.help = "The car from the garage. Enter opens the garage: every car, to choose one.";

    // The car's physics: the variants a module drives (docs/content-layers.md 4), the chosen one's setups.
    std::vector<const assets::PhysicsVariant*> drivable;
    if (car)
        for (const assets::PhysicsVariant& p : car->variants)
            if (!p.modules.empty()) drivable.push_back(&p);
    const assets::PhysicsVariant* chosen = car ? catalog.PhysicsFor(*car, s) : nullptr;
    int physics = 0;
    Row& physicsRow = rows[2];
    physicsRow.kind = Row::Choice, physicsRow.label = "PHYSICS";
    for (size_t i = 0; i < drivable.size(); ++i) {
        physicsRow.options.push_back(Upper(drivable[i]->name));
        if (drivable[i] == chosen) physics = static_cast<int>(i);
    }
    if (drivable.empty()) physicsRow.options = {"NONE"};
    physicsRow.at = &physics;
    if (chosen) {
        physicsRow.sub = (chosen->version == "rbr" ? std::string("RBR'S") : "VERSION " + Upper(chosen->version)) + " \xC2\xB7 " +
                         Upper(chosen->source) + (car->variants.size() > 1 && chosen == &car->variants.front() ? " \xC2\xB7 THE CAR'S OWN" : "");
    } else {
        physicsRow.sub = "NONE A PHYSICS MODULE DRIVES", physicsRow.subSignal = true;
    }
    physicsRow.help = "How the car drives: the physics it has that a physics module installed can drive - RBR's own, or "
                      "others downloaded. The setups are the physics'.";

    const std::vector<assets::Setup> setups = car ? catalog.SetupsFor(*car, chosen) : std::vector<assets::Setup>{};
    int setup = 0;
    Row& setupRow = rows[3];
    setupRow.kind = Row::Choice, setupRow.label = "CAR SETUP", setupRow.options = {"DEFAULT"};
    for (size_t i = 0; i < setups.size(); ++i) {
        setupRow.options.push_back(Upper(setups[i].name));
        if (setups[i].path == s.setup) setup = static_cast<int>(i) + 1;
    }
    setupRow.at = &setup, setupRow.sub = "ENTER: ITS VALUES, THE LAMPS", setupRow.subSignal = false;
    setupRow.help = "The default setup for the stage's surface, or a saved one. Enter shows its values and sets the car's lamps.";

    int damage = static_cast<int>(std::min(s.damage, 3u));
    Row& damageRow = rows[4];
    damageRow.kind = Row::Choice, damageRow.label = "DAMAGE", damageRow.at = &damage;
    damageRow.options.assign(std::begin(kDamage), std::end(kDamage));
    damageRow.help = "How much the car takes: off, safe, reduced or realistic.";

    Row& weatherRow = rows[5];
    weatherRow.kind = Row::Choice, weatherRow.label = "WEATHER", weatherRow.options = {"STAGE DEFAULT"};
    int weather = 0;
    if (st)
        for (size_t i = 0; i < st->weathers.size(); ++i) {
            weatherRow.options.push_back(Upper(st->weathers[i]));
            if (!_stricmp(st->weathers[i].c_str(), s.weather.c_str())) weather = static_cast<int>(i) + 1;
        }
    const int custom = static_cast<int>(weatherRow.options.size());
    weatherRow.options.push_back("CUSTOM");
    if (s.weatherCustom) weather = custom;
    weatherRow.at = &weather;
    if (weather == custom) weatherRow.sub = "ENTER: EDIT", weatherRow.subSignal = true;
    weatherRow.help = "A preset weather, or Custom: Enter opens the weather - the clouds, the fog, the rain, the time of day, the season "
                      "and the road.";

    Row& start = rows[6];
    start.kind = Row::Open, start.label = "START STAGE", start.shown = "", start.sub = "";
    start.off = !st || !carOk;
    start.help = "To the start line.";

    FullScreen("Quick rally", ui, width, height, [&](const Look& l) {
        Backdrop(l.d, l.W, l.H, l.k, kSurfaceBase, video, true);
        Header(l, "W-RALLY \xC2\xB7 MAIN MENU", "QUICK RALLY");
        l.d->AddRectFilled(l.P(kX0, 196), l.P(kX0 + 720, 197), kLine);
        const int keyRow = ListKeys(rows, g_quick);
        const int clicked = Rows(l, rows, g_quick, 228, 720, 76);
        const int done = keyRow >= 0 ? keyRow : clicked;
        if (done == 0) g_page = Page::Stage;
        else if (done == 1) g_page = Page::Garage;
        else if (done == 2) Change(physicsRow, +1);
        else if (done == 3 && car) g_page = Page::Setup;
        else if (done == 5 && weather == custom) g_page = Page::Weather;
        else if (done == 5) Change(weatherRow, +1);
        else if (done == 4) Change(damageRow, +1);
        else if (done == 6 && !start.off) c = QuickChoice::Start;
        const Row& sel = rows[g_quick.at];
        Help(l, g_quick.at == 1 && car && !carOk ? "W-Rally does not know this car: choose another in the garage." : sel.help);
        const char* enter = g_quick.at == 0 ? "CHOOSE" : g_quick.at == 1 ? "GARAGE" : g_quick.at == 3 ? "DETAILS"
                            : g_quick.at == 5 && weather == custom ? "EDIT" : g_quick.at == 6 ? "START" : "SELECT";
        if (Footer(l, {{"W S", "MOVE"}, {"A D", "CHANGE"}, {"ENTER", enter}, {"ESC", "BACK"}})) c = QuickChoice::Back;
    });
    if (KeyEsc()) c = QuickChoice::Back;

    // The rows' choices back into the settings: the physics kept per car (none kept for its default).
    if (car && physics < static_cast<int>(drivable.size()) && drivable[physics] != chosen) {
        if (drivable[physics] == &car->variants.front()) s.physics.erase(car->id);
        else s.physics[car->id] = drivable[physics]->id;
        s.setup.clear();   // (another physics: another car's setups)
    } else if (setupRow.at) {
        s.setup = setup ? setups[setup - 1].path : "";
    }
    s.damage = static_cast<uint32_t>(damage);
    s.weatherCustom = weather == custom;
    if (!s.weatherCustom) s.weather = weather && st ? st->weathers[weather - 1] : "";
    return c;
}

// ---------------------------------------------------------------- Stage

int g_stageSide = 1;          // 0 the countries, 1 the stages
int g_collection = 0;         // the country: 0 all, then the register's countries, then the stages of none known
int g_stageAt = -1;           // in the collection's list (-1: the chosen stage's)
int g_stageTop = 0;           // the first row shown
float g_stagePlate = -1;

void StagePage(const ImGuiLayer& ui, const assets::Catalog& catalog, app::Settings& s, int width, int height,
               const VideoFrame& video)
{
    // The countries (assets::TrackRegister): all, each the tracks are in, and "other" for the stages of none known.
    std::vector<std::string> groups = {"ALL"};
    for (const std::string& c : catalog.trackRegister.Countries()) groups.push_back(Upper(c));
    auto countryOf = [&](const assets::Stage& st) {
        const assets::TrackInfo* t = catalog.trackRegister.Find(st.map);
        return t ? Upper(t->country) : std::string();
    };
    bool other = false;
    for (const assets::Stage& st : catalog.stages) other |= countryOf(st).empty();
    if (other) groups.push_back("OTHER");
    const int ng = static_cast<int>(groups.size());
    g_collection = std::clamp(g_collection, 0, ng - 1);
    auto inGroup = [&](const assets::Stage& st, int g) {
        const std::string c = countryOf(st);
        return g == 0 || (other && g == ng - 1 ? c.empty() : c == groups[g]);
    };
    // The keys that change the country first: the list is the new country's from this frame on.
    if (KeyA()) g_stageSide = 0;
    if (KeyD()) g_stageSide = 1;
    const int dir = KeyW() ? -1 : KeyS() ? +1 : 0;
    if (dir && g_stageSide == 0 && g_collection + dir >= 0 && g_collection + dir < ng) g_collection += dir, g_stageAt = -1;
    std::vector<const assets::Stage*> list;
    for (const assets::Stage& st : catalog.stages)
        if (inGroup(st, g_collection)) list.push_back(&st);
    const int n = static_cast<int>(list.size());
    if (g_stageAt < 0 || g_stageAt >= n) {   // (-1: the chosen stage's place in the list, else its first)
        g_stageAt = 0;
        for (int i = 0; i < n; ++i)
            if (list[i]->key == s.stage) g_stageAt = i;
    }
    // A stage chosen: back to Quick rally - taken after the page is drawn (the list's selection holds until then).
    int chosen = -1;
    // The rows shown: as many as fit above the facts' plate (at 816); PgUp / PgDn a page of them.
    constexpr int kShown = 8;
    constexpr float kListY = 268, kRowH = 64, kColX = kX0 + 288, kColW = 420;
    if (dir && g_stageSide == 1) g_stageAt = std::clamp(g_stageAt + dir, 0, std::max(0, n - 1));
    if (const int page = ImGui::IsKeyPressed(ImGuiKey_PageUp) ? -kShown : ImGui::IsKeyPressed(ImGuiKey_PageDown) ? kShown : 0; page && n)
        g_stageSide = 1, g_stageAt = std::clamp(g_stageAt + page, 0, n - 1), g_stageTop = std::clamp(g_stageTop + page, 0, std::max(0, n - kShown));
    if (KeyEnter()) {
        if (g_stageSide == 0) g_stageSide = 1;
        else chosen = g_stageAt;
    }
    int collection = g_collection;   // a country clicked: from the next frame

    g_stageTop = std::clamp(g_stageTop, std::max(0, g_stageAt - kShown + 1), std::max(0, std::min(g_stageAt, n - kShown)));
    FullScreen("Stage", ui, width, height, [&](const Look& l) {
        const float k = l.k;
        Backdrop(l.d, l.W, l.H, k, kSurfaceBase, video, true);
        Header(l, "W-RALLY \xC2\xB7 QUICK RALLY", "STAGE");
        l.d->AddRectFilled(l.P(kX0, 196), l.P(kX0 + 708, 197), kLine);
        const ImGuiIO& io = ImGui::GetIO();
        const bool moved = io.MouseDelta.x || io.MouseDelta.y, click = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        const float ease = Ease();

        // The countries
        Tracked(l.d, l.text, 17 * k, l.P(kX0 + 12, 228), g_stageSide == 0 ? kInk : kInkMuted, "COUNTRY", 0.16f);
        for (int i = 0; i < ng; ++i) {
            const float y = kListY + i * 56, mid = (y + 26) * k;
            const bool on = i == g_collection;
            const bool hot = ImGui::IsMouseHoveringRect(l.P(kX0, y), l.P(kX0 + 260, y + 52), false);
            if (hot && click) collection = i, g_stageSide = 0;
            if (on) Plate(l.d, l.P(kX0, y), 260 * k, 52 * k, 10 * k, g_stageSide == 0 ? kSelect : kSelectIdle);
            Marker(l.d, (kX0 + 10) * k, mid, 10 * k, 28 * k, on && g_stageSide == 0 ? 1.0f : 0.0f, k);
            int count = 0;
            for (const assets::Stage& st : catalog.stages) count += inGroup(st, i);
            l.d->PushClipRect(l.P(kX0, y), l.P(kX0 + 214, y + 52), true);
            Tracked(l.d, l.item, 22 * k, ImVec2((kX0 + 36) * k, mid - 11 * k), on ? kInk : kInkSecondary, groups[i].c_str(), 0.02f);
            l.d->PopClipRect();
            const std::string cnt = std::to_string(count);
            const float cw = Tracked(nullptr, l.text, 16 * k, ImVec2(), 0, cnt.c_str(), 0.08f);
            Tracked(l.d, l.text, 16 * k, ImVec2((kX0 + 244) * k - cw, mid - 8 * k), kInkMuted, cnt.c_str(), 0.08f);
        }

        // The stages, scrolled to the selected one
        Tracked(l.d, l.text, 17 * k, l.P(kColX + 12, 228), g_stageSide == 1 ? kInk : kInkMuted,
                (std::string("STAGES \xC2\xB7 ") + std::to_string(n)).c_str(), 0.16f);
        if (n > kShown) {   // (the page keys by the list they move: the footer is full)
            const char* pages = "PGUP PGDN: PAGE";
            const float pw = Tracked(nullptr, l.text, 13 * k, ImVec2(), 0, pages, 0.16f);
            Tracked(l.d, l.text, 13 * k, ImVec2((kColX + kColW) * k - pw, 230 * k), kInkMuted, pages, 0.16f);
        }
        if (n) {
            const float target = kListY + (g_stageAt - g_stageTop) * (kRowH + 4);
            g_stagePlate = g_stagePlate < 0 ? target : g_stagePlate + (target - g_stagePlate) * ease;
            Plate(l.d, l.P(kColX, g_stagePlate), kColW * k, kRowH * k, 12 * k, g_stageSide == 1 ? kSelect : kSelectIdle);
        }
        for (int i = g_stageTop; i < std::min(n, g_stageTop + kShown); ++i) {
            const assets::Stage& st = *list[i];
            const float y = kListY + (i - g_stageTop) * (kRowH + 4), mid = (y + kRowH * 0.5f) * k;
            const bool on = i == g_stageAt;
            const bool hot = ImGui::IsMouseHoveringRect(l.P(kColX, y), l.P(kColX + kColW, y + kRowH), false);
            if (hot && moved) g_stageAt = i, g_stageSide = 1;
            if (hot && click) chosen = i;
            Marker(l.d, (kColX + 12) * k, mid, 12 * k, 34 * k, on && g_stageSide == 1 ? 1.0f : 0.0f, k);
            l.d->PushClipRect(l.P(kColX, y), l.P(kColX + kColW - (st.key == s.stage ? 100 : 20), y + kRowH), true);
            Tracked(l.d, l.item, 22 * k, ImVec2((kColX + 40) * k, mid - 20 * k), on ? kInk : kInkSecondary, Upper(st.title).c_str(), 0.02f);
            l.d->PopClipRect();
            const assets::TrackInfo* track = catalog.trackRegister.Find(st.map);
            std::string facts = countryOf(st).empty() ? std::string(st.imported ? "RBR" : ".NGG") : countryOf(st);
            if (track && !track->surface.empty()) facts += " \xC2\xB7 " + Upper(track->surface);
            facts += " \xC2\xB7 " + std::to_string(st.weathers.size()) + " WEATHERS";
            Tracked(l.d, l.text, 14 * l.k, ImVec2((kColX + 40) * k, mid + 8 * k), kInkMuted, facts.c_str(), 0.16f);
            if (st.key == s.stage) {
                const float cw = Tracked(nullptr, l.text, 13 * k, ImVec2(), 0, "CHOSEN", 0.16f);
                Tracked(l.d, l.text, 13 * k, ImVec2((kColX + kColW - 24) * k - cw, mid - 7 * k), kSignal, "CHOSEN", 0.16f);
            }
        }
        if (n > kShown) {   // where the shown rows are in the list
            const float top = kListY, h = kShown * (kRowH + 4) - 4, x = kColX + kColW + 10;
            l.d->AddRectFilled(l.P(x, top), l.P(x + 3, top + h), kLine);
            const float a = top + h * g_stageTop / n, b = top + h * std::min(n, g_stageTop + kShown) / n;
            l.d->AddRectFilled(l.P(x, a), l.P(x + 3, b), kInkSecondary);
        }

        // The selected stage's facts
        if (n) {
            const assets::Stage& st = *list[g_stageAt];
            const float y = 900 - 84;
            Plate(l.d, l.P(kX0, y), 680 * k, 80 * k, 12 * k, kSurfaceRaised);
            float x = kX0 + 24;
            const assets::TrackInfo* track = catalog.trackRegister.Find(st.map);
            const std::string map = st.map >= 0 ? std::to_string(st.map) : "-";
            const std::string length = track && track->lengthKm > 0 ? Fmt("%.1f KM", track->lengthKm) : "-";
            const std::pair<const char*, std::string> facts[4] = {
                {"SURFACE", track && !track->surface.empty() ? Upper(track->surface) : "-"}, {"LENGTH", length}, {"MAP", map},
                {"FILE", Upper(FileName(st.ngg))}};
            for (const auto& [label, value] : facts) {
                Tracked(l.d, l.text, 13 * k, l.P(x, y + 16), kInkMuted, label, 0.16f);
                l.d->PushClipRect(l.P(x, y), l.P(kX0 + 660, y + 80), true);
                const float vw = Tracked(l.d, l.item, 22 * k, l.P(x, y + 38), kInk, value.c_str(), 0.02f);
                l.d->PopClipRect();
                x += std::max(110.0f, vw / k + 40);
            }
        }
        if (Footer(l, {{"W S", "MOVE"}, {"A D", "COUNTRY / STAGE"}, {"ENTER", "CHOOSE"}, {"ESC", "BACK"}})) g_page = Page::Quick;
    });
    if (collection != g_collection) g_collection = collection, g_stageAt = -1, g_stageTop = 0;
    if (chosen >= 0 && chosen < n) {
        if (list[chosen]->key != s.stage) s.weather.clear(), s.weatherCustom = false;
        s.stage = list[chosen]->key;
        g_page = Page::Quick;
    }
    if (KeyEsc()) g_page = Page::Quick;
    if (g_page != Page::Stage) g_stageAt = -1, g_stagePlate = -1;   // (opened again: on the chosen stage)
}

// ---------------------------------------------------------------- Garage

int g_garageAt = -1;
float g_garageScroll = -1;
// The showroom under the page (game.cpp renders it): the car shown, its turntable's turn - slowly by itself, by the
// mouse dragged over the car (which holds it still a while).
std::string g_showCar;
float g_turn = 0.6f, g_turnHeld = 0;
float g_showX = 0.6f, g_showY = 0.45f;   // where the car's middle goes (0..1 of the window)

int g_garageClass = 0;   // the class tab: 0 all, then the register's classes
bool g_garageFromMain = false;   // opened from the main menu (ui::OpenGarage): leaving it is back there

void GaragePage(const ImGuiLayer& ui, const assets::Catalog& catalog, app::Settings& s, int width, int height,
                const VideoFrame& video)
{
    // The class tabs (Q / E): all, then the classes the known cars have.
    std::vector<std::string> classes = {"ALL"};
    for (const std::string& c : catalog.carRegister.Classes()) classes.push_back(c);
    const int nc = static_cast<int>(classes.size());
    g_garageClass = std::clamp(g_garageClass, 0, nc - 1);
    int cls = g_garageClass;
    if (ImGui::IsKeyPressed(ImGuiKey_Q, false) && cls > 0) --cls;
    if (ImGui::IsKeyPressed(ImGuiKey_E, false) && cls < nc - 1) ++cls;
    if (cls != g_garageClass) g_garageClass = cls, g_garageAt = -1, g_garageScroll = -1;

    // The class's cars (all: every car, the unusable ones too).
    std::vector<int> shown;
    for (int i = 0; i < static_cast<int>(catalog.cars.size()); ++i) {
        const assets::CarInfo* info = catalog.carRegister.Find(catalog.cars[i].folder);
        if (g_garageClass == 0 || (info && info->known && info->carClass == classes[g_garageClass])) shown.push_back(i);
    }
    const int n = static_cast<int>(shown.size());
    if (g_garageAt < 0 || g_garageAt >= n) {
        g_garageAt = 0;
        for (int i = 0; i < n; ++i)
            if (!_stricmp(catalog.cars[shown[i]].folder.c_str(), s.car.c_str())) g_garageAt = i;
    }
    auto infoOf = [&](int i) { return catalog.carRegister.Find(catalog.cars[shown[i]].folder); };
    auto usable = [&](int i) {   // known to W-Rally, and with physics of its own
        const assets::CarInfo* info = infoOf(i);
        return info && info->known && assets::Catalog::Drivable(catalog.cars[shown[i]]);
    };
    // A car chosen: back to Quick rally - taken after the page is drawn (the selection holds until then). Only a usable
    // one.
    int chosen = -1;
    auto choose = [&](int i) {
        if (usable(i)) chosen = i;
    };
    if (n && (KeyA() || KeyW())) g_garageAt = (g_garageAt + n - 1) % n;
    if (n && (KeyD() || KeyS())) g_garageAt = (g_garageAt + 1) % n;
    if (n && KeyEnter()) choose(g_garageAt);
    // The wheel steps along the carousel (down: the next car), stopping at its ends rather than wrapping.
    if (const float wheel = ImGui::GetIO().MouseWheel; n && wheel != 0)
        g_garageAt = std::clamp(g_garageAt + (wheel < 0 ? 1 : -1), 0, n - 1);

    // The turntable: dragged by the mouse above the carousel, else turning by itself (after a pause once dragged).
    {
        const ImGuiIO& io = ImGui::GetIO();
        const float carouselTop = (1080 - 150 - 132 - 24) * std::min(height / 1080.0f, width / 1280.0f);
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f) && io.MouseClickedPos[0].y < carouselTop)
            g_turn += io.MouseDelta.x * 0.008f, g_turnHeld = 2.5f;
        else if ((g_turnHeld -= io.DeltaTime) <= 0)
            g_turn += io.DeltaTime * 0.22f;
        g_turn = std::remainder(g_turn, 6.2831853f);
    }
    g_showCar = n ? catalog.cars[shown[g_garageAt]].folder : "";
    {   // between the column (from the left edge) and the signal stripe (from the right), over the carousel
        const float k = std::min(height / 1080.0f, width / 1280.0f);
        g_showX = (760 * k + width - 360 * k) * 0.5f / width, g_showY = 470 * k / height;
    }

    FullScreen("Garage", ui, width, height, [&](const Look& l) {
        const float k = l.k;
        // The showroom shows through: no ground, no livery (faded out: the car is the picture), darkened to the left
        // behind the column and to the foot behind the carousel.
        Backdrop(l.d, l.W, l.H, k, IM_COL32(10, 10, 11, 0), video, false, false);
        const ImU32 shade = IM_COL32(10, 10, 11, 235), clear = IM_COL32(10, 10, 11, 0);
        l.d->AddRectFilledMultiColor(ImVec2(0, 0), l.P(900, 1080), shade, clear, clear, shade);
        l.d->AddRectFilledMultiColor(ImVec2(0, l.H - 420 * k), ImVec2(l.W, l.H), clear, clear, shade, shade);
        Header(l, "W-RALLY \xC2\xB7 QUICK RALLY", "GARAGE");

        // The class tabs between their keys
        float tx = kX0 * k;
        tx += KeyHint(l.d, l.text, k, ImVec2(tx, 196 * k), "Q", "") + 22 * k;
        for (int t = 0; t < nc; ++t) {
            const std::string label = Upper(classes[t]);
            const float w = Tracked(nullptr, l.item, 22 * k, ImVec2(), 0, label.c_str(), 0.02f);
            Tracked(l.d, l.item, 22 * k, ImVec2(tx, 200 * k), t == g_garageClass ? kInk : kInkMuted, label.c_str(), 0.02f);
            if (t == g_garageClass) {
                const float by = 234 * k, bh = 4 * k, lean = bh * 0.5f * kTanPlate;
                l.d->AddQuadFilled(ImVec2(tx + lean, by), ImVec2(tx + w + lean, by), ImVec2(tx + w - lean, by + bh), ImVec2(tx - lean, by + bh), kSignal);
            }
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsMouseHoveringRect(ImVec2(tx - 4 * k, 190 * k), ImVec2(tx + w + 4 * k, 242 * k), false))
                g_garageClass = t, g_garageAt = -1, g_garageScroll = -1;
            tx += w + 22 * k;
        }
        KeyHint(l.d, l.text, k, ImVec2(tx - 6 * k, 196 * k), "E", "");
        Tracked(l.d, l.text, 14 * k, l.P(kX0, 252), kInkMuted, (std::to_string(n) + (n == 1 ? " CAR" : " CARS")).c_str(), 0.16f);

        if (!n) {
            l.d->AddText(l.text, 23 * k, l.P(kX0, 300), kInkSecondary, "No cars found: see Settings for the RBR folder.");
        } else {
            const assets::Car& car = catalog.cars[shown[g_garageAt]];
            const assets::CarInfo* info = infoOf(g_garageAt);
            const bool ok = usable(g_garageAt);
            if (info && !info->carClass.empty()) Tracked(l.d, l.text, 17 * k, l.P(kX0, 300), kInkMuted, Upper(info->carClass).c_str(), 0.16f);
            l.d->AddText(l.mark, 52 * k, l.P(kX0, 324), kInk, Upper(car.name).c_str(), nullptr, 0);
            auto num = [](float v, const char* unit) { return v > 0 ? std::to_string(static_cast<int>(std::lround(v))) + unit : std::string("-"); };
            const std::string maker = info && !info->maker.empty() ? Upper(info->maker) + (info->year ? " \xC2\xB7 " + std::to_string(info->year) : "") : "-";
            const std::pair<const char*, std::string> specs[6] = {
                {"MAKER", maker},
                {"DRIVE", info && !info->drive.empty() ? Upper(info->drive) : "-"},
                {"POWER", num(info ? info->powerHp : 0, " HP")},
                {"WEIGHT", num(info ? info->weightKg : 0, " KG")},
                {"PHYSICS", Upper(car.physicsFrom)},
                {"KNOWN FROM", info ? Upper(info->from) : "-"}};
            for (int i = 0; i < 6; ++i) {
                const float x = kX0 + float(i % 2) * 300, y = 410 + float(i / 2) * 70;
                Tracked(l.d, l.text, 13 * k, l.P(x, y), kInkMuted, specs[i].first, 0.16f);
                l.d->PushClipRect(l.P(x, y), l.P(x + 280, y + 60), true);
                Tracked(l.d, l.item, 24 * k, l.P(x, y + 20), kInk, specs[i].second.c_str(), 0.02f);
                l.d->PopClipRect();
            }
            if (info && !info->error.empty()) l.d->AddText(l.text, 18 * k, l.P(kX0, 626), kSignal, info->error.c_str(), nullptr, 600 * k);

            // A car nothing knows, or one without physics: over the showroom, big, why it cannot be driven.
            if (!ok) {
                const float cx = l.W * g_showX, cy = l.H * g_showY;
                const char* big = "NOT USABLE BY W-RALLY";
                const float bw = l.mark->CalcTextSizeA(64 * k, FLT_MAX, 0, big).x;
                l.d->AddRectFilled(ImVec2(cx - bw * 0.5f - 40 * k, cy - 70 * k), ImVec2(cx + bw * 0.5f + 40 * k, cy + 76 * k), IM_COL32(10, 10, 11, 210));
                l.d->AddText(l.mark, 64 * k, ImVec2(cx - bw * 0.5f, cy - 52 * k), kSignal, big);
                const char* why = !info || !info->known
                                      ? "Neither W-Rally's car list nor a car.json in its folder says what it is or how it drives."
                                      : "It has no physics: no physics folder of its own, no Cars.ini slot, no \"physics\" in its car.json.";
                const float ww = l.text->CalcTextSizeA(20 * k, FLT_MAX, 0, why).x;
                l.d->AddText(l.text, 20 * k, ImVec2(cx - ww * 0.5f, cy + 26 * k), kInkSecondary, why);
            }

            // The carousel along the foot, eased to keep the chosen card in view.
            const float cardW = 180, cardH = 116, gap = 12, y = 1080 - 150 - 132;
            const float span = 1920 - 2 * kX0 - 2 * 60, perRow = std::floor((span + gap) / (cardW + gap));
            const float target = std::clamp(g_garageAt - std::floor(perRow / 2), 0.0f, std::max(0.0f, n - perRow));
            g_garageScroll = g_garageScroll < 0 ? target : g_garageScroll + (target - g_garageScroll) * Ease();
            const float x0 = kX0 + 60;
            l.d->PushClipRect(l.P(x0 - 4, y - 20), l.P(x0 + span + 4, y + 140), true);
            const bool click = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
            for (int i = 0; i < n; ++i) {
                const float x = x0 + (i - g_garageScroll) * (cardW + gap);
                if (x + cardW < x0 - 200 || x > x0 + span + 200) continue;
                const bool on = i == g_garageAt, use = usable(i);
                const float h = on ? 132.0f : cardH, top = y + (132 - h);
                Plate(l.d, l.P(x, top), cardW * k, h * k, 12 * k, on ? kSurfaceOverlay : kSurfaceRaised);
                if (on) l.d->AddRectFilled(l.P(x, top + h - 4), l.P(x + cardW - 1, top + h), use ? kSignal : kLineStrong);
                const std::string name = Upper(catalog.cars[shown[i]].name);
                l.d->PushClipRect(l.P(x, top), l.P(x + cardW - 14, top + h), true);
                const ImU32 ink = use ? (on ? kInk : kInkSecondary) : Alpha(kInkMuted, 0.7f);
                l.d->AddText(l.item, 18 * k, l.P(x + 14, top + h - 58), ink, name.c_str(), nullptr, (cardW - 28) * k);
                if (!use) Tracked(l.d, l.text, 12 * k, l.P(x + 14, top + 12), kSignal, "NOT USABLE", 0.16f);
                l.d->PopClipRect();
                if (click && ImGui::IsMouseHoveringRect(l.P(x, top), l.P(x + cardW, top + h), false)) {
                    if (on) choose(i);
                    else g_garageAt = i;
                }
            }
            l.d->PopClipRect();
            Chevron(l.d, l.P(kX0 + 22, y + 66), 26 * k, false, kInkSecondary);
            Chevron(l.d, l.P(1920 - kX0 - 22, y + 66), 26 * k, true, kInkSecondary);
        }
        if (Footer(l, {{"Q E", "CLASS"}, {"A D", "CAR"}, {"ENTER", "CHOOSE"}, {"ESC", "BACK"}})) g_page = Page::Quick;
    });
    if (chosen >= 0 && chosen < n) {
        const assets::Car& car = catalog.cars[shown[chosen]];
        if (_stricmp(car.folder.c_str(), s.car.c_str())) s.setup.clear();
        s.car = car.folder;
        g_page = Page::Quick;
    }
    if (KeyEsc()) g_page = Page::Quick;
    if (g_page != Page::Garage) g_garageAt = -1, g_garageScroll = -1, g_showCar.clear();   // (opened again: on the chosen car)
}

// ---------------------------------------------------------------- Car setup

int g_setupTab = 0;
List g_setupList[7];

// A setup file's values (the CarSetup form's sections), read once per file.
struct SetupValues {
    bool read = false;
    std::string error;
    phys::data::LspNode root;
    const phys::data::LspNode* form = nullptr;
    // The number after `key` in `section` (NaN: not there).
    float Get(const char* section, const char* key) const
    {
        const phys::data::LspNode* sec = form ? form->After(section) : nullptr;
        if (!sec || !sec->IsList()) return NAN;
        const std::vector<float> v = sec->Numbers(key);
        return v.empty() ? NAN : v[0];
    }
};

// A setup's values: `path` a VFS path (a slot car's are physics.rbz's: no file on disk).
const SetupValues& ReadSetup(const assets::Catalog& catalog, const std::string& path)
{
    static std::map<std::string, SetupValues> cache;
    SetupValues& v = cache[path];
    if (v.read) return v;
    v.read = true;
    try {
        std::vector<uint8_t> bytes;
        if (!catalog.Content() || !catalog.Content()->Read(path, bytes)) throw std::runtime_error("cannot open " + path);
        v.root = phys::data::ParseLsp(std::string(bytes.begin(), bytes.end()));
        // The form: (("CarSetup" Car (...) Drive (...) ...)) - the list whose first item is "CarSetup", a level or two in.
        auto find = [&](auto&& self, const phys::data::LspNode& n, int depth) -> const phys::data::LspNode* {
            if (!n.IsList() || depth > 3) return nullptr;
            if (!n.items.empty() && n.items[0].kind == phys::data::LspNode::String && n.items[0].text == "CarSetup") return &n;
            for (const phys::data::LspNode& c : n.items)
                if (const phys::data::LspNode* f = self(self, c, depth + 1)) return f;
            return nullptr;
        };
        v.form = find(find, v.root, 0);
        if (!v.form) v.error = "no CarSetup in it";
    } catch (const std::exception& e) {
        v.error = e.what();
    }
    return v;
}

std::string Fmt(const char* format, float x)
{
    if (std::isnan(x)) return "-";
    char t[48];
    std::snprintf(t, sizeof t, format, x);
    return t;
}

std::string FmtSpread(float x) { return Fmt("%+.1f\xC2\xB0 OUT", x); }
std::string FmtPitch(float x) { return Fmt("%+.2f\xC2\xB0 UP", x); }

void SetupPage(const ImGuiLayer& ui, const assets::Catalog& catalog, app::Settings& s, int width, int height,
               const VideoFrame& video)
{
    static const char* const kTabs[7] = {"TYRES", "SUSPENSION", "DAMPERS", "DIFFERENTIALS", "BRAKES", "STEERING", "LIGHTS"};
    const assets::Car* car = catalog.FindCar(s.car);
    if (!car) return void(g_page = Page::Quick);
    if (ImGui::IsKeyPressed(ImGuiKey_Q, false)) g_setupTab = (g_setupTab + 6) % 7;
    if (ImGui::IsKeyPressed(ImGuiKey_E, false)) g_setupTab = (g_setupTab + 1) % 7;

    std::string setupName = "DEFAULT";
    for (const assets::Setup& su : catalog.SetupsFor(*car, catalog.PhysicsFor(*car, s)))
        if (su.path == s.setup) setupName = Upper(su.name);
    const SetupValues* v = s.setup.empty() ? nullptr : &ReadSetup(catalog, s.setup);
    constexpr float kRad = 57.2957795f;

    std::vector<Row> rows;
    auto heading = [&](const char* label) { rows.emplace_back(), rows.back().label = label; };
    auto value = [&](const char* label, const std::string& shown, const char* help) {
        rows.emplace_back();
        Row& r = rows.back();
        r.kind = Row::Value, r.label = label, r.shown = shown, r.help = help;
    };
    auto get = [&](const char* section, const char* key, float scale) { return v ? v->Get(section, key) * scale : NAN; };
    const int tab = g_setupTab;
    // The car's lamps (Settings::lamps): its ramps, the ramp's aim.
    struct Layouts {
        std::vector<std::string> keys, shown;
        size_t first = 0;
    };
    static std::map<std::string, Layouts> layoutsOf;
    int layout = 0;
    app::Settings::Lamps& lamp = s.lamps[car->folder];
    Layouts& ls = layoutsOf[car->folder];
    if (tab == 6) {
        if (ls.keys.empty()) {
            const float noCog[3] = {};
            const assets::CarLamps lamps = assets::LoadCarLamps(car->sgc, noCog);
            for (const assets::LampLayout& la : lamps.layouts) {
                ls.keys.push_back(la.name);
                ls.shown.push_back(Upper(la.name != "ramp" ? la.name : "its own ramp (" + std::to_string(la.lamps.size()) + " pods)"));
            }
            ls.first = lamps.defaultLayout;
        }
        layout = static_cast<int>(ls.first);
        for (size_t i = 0; i < ls.keys.size(); ++i)
            if (ls.keys[i] == lamp.layout) layout = static_cast<int>(i);
        rows.emplace_back();
        Row& ramp = rows.back();
        ramp.kind = Row::Choice, ramp.label = "RAMP", ramp.at = &layout, ramp.options = ls.shown;
        ramp.help = "The lamps on the car's nose: its own ramp where its model places one, or pods.";
        rows.emplace_back();
        Row& spread = rows.back();
        spread.kind = Row::Slider, spread.label = "SPREAD", spread.value = &lamp.spread, spread.min = -5, spread.max = 10,
        spread.step = 0.5f, spread.format = FmtSpread, spread.help = "Each pod turned further out than the one inside it, degrees.";
        rows.emplace_back();
        Row& pitch = rows.back();
        pitch.kind = Row::Slider, pitch.label = "AIM", pitch.value = &lamp.pitch, pitch.min = -3, pitch.max = 3, pitch.step = 0.25f,
        pitch.format = FmtPitch, pitch.help = "The ramp aimed up or down, degrees.";
    } else if (!v) {
        value("THE DEFAULT SETUP", "THE SURFACE'S", "The default setup is the car's own for the stage's surface, read as the stage loads.");
    } else if (!v->error.empty()) {
        value("THE SETUP", "UNREADABLE", "");
        rows.back().help = v->error;
    } else if (tab <= 2) {   // front | rear: the left wheels' (a setup is the same both sides)
        auto axles = [&](const char* label, const char* format, const char* front, const char* rear, const char* key, float scale,
                         const char* help) {
            value(label, Fmt(format, get(front, key, scale)), help);
            rows.back().shown2 = Fmt(format, get(rear, key, scale));
        };
        if (tab == 0) {
            axles("PRESSURE", "%.2f BAR", "TyreLF", "TyreLB", "Pressure", 1e-5f, "The tyres' pressure.");
        } else if (tab == 1) {
            axles("SPRING RATE", "%.0f N/MM", "SpringDamperLF", "SpringDamperLB", "SpringStiffness", 1e-3f, "The spring's stiffness.");
            axles("HELPER SPRING", "%.0f N/MM", "SpringDamperLF", "SpringDamperLB", "HelperSpringStiffness", 1e-3f, "The helper spring's stiffness.");
            axles("PLATFORM HEIGHT", "%.0f MM", "WheelLF", "WheelLB", "StrutPlatformHeight", 1e3f, "Where the spring sits on the strut: the ride height.");
            axles("CAMBER", "%.1f\xC2\xB0", "WheelLF", "WheelLB", "WheelAxisInclination", kRad, "The wheel's lean.");
            value("ANTI-ROLL BAR", Fmt("%.0f N/MM", get("Car", "FrontRollBarStiffness", 1e-3f)), "The anti-roll bars' stiffness.");
            rows.back().shown2 = Fmt("%.0f N/MM", get("Car", "RearRollBarStiffness", 1e-3f));
        } else {
            axles("BUMP", "%.0f NS/M", "SpringDamperLF", "SpringDamperLB", "DampingBump", 1, "The damper against compression.");
            axles("FAST BUMP", "%.0f NS/M", "SpringDamperLF", "SpringDamperLB", "DampingBumpHighSpeed", 1,
                  "Against fast compression: kerbs, rocks, landings.");
            axles("REBOUND", "%.0f NS/M", "SpringDamperLF", "SpringDamperLB", "DampingRebound", 1, "The damper against extension.");
        }
    } else if (tab == 3) {
        value("FRONT", Fmt("%.0f NM", get("Drive", "FrontDiffMaxTorque", 1)), "The front differential's locking torque.");
        value("CENTRE", Fmt("%.0f NM", get("Drive", "CenterDiffMaxTorque", 1)), "The centre differential's locking torque.");
        value("REAR", Fmt("%.0f NM", get("Drive", "RearDiffMaxTorque", 1)), "The rear differential's locking torque.");
        value("HANDBRAKE RELEASE", Fmt("%.2f", get("VehicleControlUnit", "CenterDiffHandbrakeRelease", 1)),
              "How far the centre differential frees with the handbrake.");
    } else if (tab == 4) {
        const float f = get("Drive", "MaxBrakePressureFront", 1), r = get("Drive", "MaxBrakePressureRear", 1);
        value("FRONT PRESSURE", Fmt("%.1f MPA", f * 1e-6f), "The front brakes' most pressure.");
        value("REAR PRESSURE", Fmt("%.1f MPA", r * 1e-6f), "The rear brakes' most pressure.");
        value("BIAS", Fmt("%.0f %% FRONT", f / (f + r) * 100), "The braking toward the front.");
    } else if (tab == 5) {
        value("STEERING LOCK", Fmt("%.1f\xC2\xB0", get("Car", "MaxSteeringLock", kRad)), "How far the front wheels turn.");
        value("STEERING ROD", Fmt("%.1f MM", get("WheelLF", "SteeringRodLength", 1e3f)), "The steering rod's length: the toe.");
    }

    FullScreen("Car setup", ui, width, height, [&](const Look& l) {
        const float k = l.k;
        Backdrop(l.d, l.W, l.H, k, kSurfaceBase, video, true);
        Header(l, ("W-RALLY \xC2\xB7 QUICK RALLY \xC2\xB7 " + Upper(car->name)).c_str(), "CAR SETUP");
        const float tw = Tracked(nullptr, l.mark, 48 * k, ImVec2(), 0, "CAR SETUP", 0);
        Tracked(l.d, l.item, 20 * k, ImVec2((kX0 + 24) * k + tw, 128 * k), kInkSecondary, ("FROM: " + setupName).c_str(), 0.02f);
        if (tab != 6) Tracked(l.d, l.text, 14 * k, ImVec2((kX0 + 24) * k + tw, 156 * k), kInkMuted, "VIEW ONLY: THE EDITOR COMES LATER", 0.16f);

        // The tabs between their keys
        float tx = kX0 * k;
        tx += KeyHint(l.d, l.text, k, ImVec2(tx, 206 * k), "Q", "") + 24 * k;
        for (int t = 0; t < 7; ++t) {
            const float w = Tracked(nullptr, l.item, 22 * k, ImVec2(), 0, kTabs[t], 0.02f);
            Tracked(l.d, l.item, 22 * k, ImVec2(tx, 210 * k), t == tab ? kInk : kInkMuted, kTabs[t], 0.02f);
            if (t == tab) {
                const float by = 244 * k, bh = 4 * k, lean = bh * 0.5f * kTanPlate;
                l.d->AddQuadFilled(ImVec2(tx + lean, by), ImVec2(tx + w + lean, by), ImVec2(tx + w - lean, by + bh), ImVec2(tx - lean, by + bh), kSignal);
            }
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsMouseHoveringRect(ImVec2(tx - 4 * k, 200 * k), ImVec2(tx + w + 4 * k, 252 * k), false))
                g_setupTab = t;
            tx += w + 22 * k;
        }
        KeyHint(l.d, l.text, k, ImVec2(tx - 6 * k, 206 * k), "E", "");
        l.d->AddRectFilled(l.P(kX0, 262), l.P(kX0 + 740, 263), kLine);

        List& list = g_setupList[tab];
        ListKeys(rows, list);
        const bool axles = v && v->error.empty() && tab <= 2;
        if (axles) {   // the columns' heads over the values
            const float xr = kX0 + 740 - 24;
            for (int col = 0; col < 2; ++col) {
                const char* head = col ? "REAR" : "FRONT";
                const float hw = Tracked(nullptr, l.text, 14 * k, ImVec2(), 0, head, 0.16f);
                Tracked(l.d, l.text, 14 * k, ImVec2((xr - (col ? 0 : 180)) * k - hw, 282 * k), kInkMuted, head, 0.16f);
            }
        }
        Rows(l, rows, list, axles ? 308.0f : 280.0f, 740, 54);
        if (!rows.empty()) Help(l, rows[list.at].help);
        if (Footer(l, {{"Q E", "PART"}, {"W S", "MOVE"}, {"A D", "CHANGE"}, {"ESC", "BACK"}})) g_page = Page::Quick;
    });
    if (tab == 6 && !ls.keys.empty()) lamp.layout = ls.keys[std::clamp(layout, 0, static_cast<int>(ls.keys.size()) - 1)];
    if (KeyEsc()) g_page = Page::Quick;
}

// ---------------------------------------------------------------- Weather

List g_weather;

std::string FmtHour(float h)
{
    if (h < 0) return "THE PRESET'S";
    const int m = static_cast<int>(std::lround(h * 60));
    char t[16];
    std::snprintf(t, sizeof t, "%02d:%02d", (m / 60) % 24, m % 60);
    return t;
}
std::string FmtShare(float x) { return x <= 0.001f ? "NONE" : Fmt("%.0f %%", x * 100); }

void WeatherPage(const ImGuiLayer& ui, const assets::Catalog& catalog, app::Settings& s, int width, int height,
                 const VideoFrame& video)
{
    const assets::Stage* st = catalog.FindStage(s.stage);
    std::vector<Row> rows;
    auto add = [&](Row::Kind kind, const char* label, const char* help) -> Row& {
        rows.emplace_back();
        rows.back().kind = kind, rows.back().label = label, rows.back().help = help;
        return rows.back();
    };
    int base = 0;
    Row& from = add(Row::Choice, "STARTS FROM", "The preset the custom weather starts from: its sun and its light, its clouds, fog and rain changed below.");
    from.options = {"STAGE DEFAULT"};
    if (st)
        for (size_t i = 0; i < st->weathers.size(); ++i) {
            from.options.push_back(Upper(st->weathers[i]));
            if (!_stricmp(st->weathers[i].c_str(), s.weather.c_str())) base = static_cast<int>(i) + 1;
        }
    from.at = &base;
    const bool sky = st && st->weathersDrawn;   // the .ngg has presets to change
    add(Row::Heading, "SKY", "");
    float hour = -0.25f;
    if (int h = 0, m = 0; std::sscanf(s.time.c_str(), "%d:%d", &h, &m) == 2) hour = h + m / 60.0f;
    Row& time = add(Row::Slider, "TIME OF DAY", "The sun, the moon and the stars from the time and the season's date, at the stage's place. "
                                                "Left of midnight: the preset's own sun.");
    time.value = &hour, time.min = -0.25f, time.max = 23.75f, time.step = 0.25f, time.format = FmtHour;
    int season = static_cast<int>(std::min(s.season, 3u));
    Row& seasonRow = add(Row::Choice, "SEASON", "The date: how high the sun climbs and when it sets.");
    seasonRow.at = &season, seasonRow.options.assign(std::begin(kSeasons), std::end(kSeasons));
    const char* noSky = "This stage's .ngg has no weather presets to start from: its own sky is drawn.";
    Row& clouds = add(Row::Slider, "CLOUDS", sky ? "How much of the sky is cloud." : noSky);
    clouds.value = &s.clouds, clouds.step = 0.05f, clouds.format = FmtShare, clouds.off = !sky;
    Row& fog = add(Row::Slider, "FOG", sky ? "How thick the fog: none to about 150 m." : noSky);
    fog.value = &s.fog, fog.step = 0.05f, fog.format = FmtShare, fog.off = !sky;
    Row& rain = add(Row::Slider, "RAIN", sky ? "How hard it rains." : noSky);
    rain.value = &s.rain, rain.step = 0.05f, rain.format = FmtShare, rain.off = !sky;
    add(Row::Heading, "THE ROAD", "");
    int road = static_cast<int>(std::min(s.road, 3u));
    Row& roadRow = add(Row::Choice, "ROAD", "The road's surface: as the preset leaves it, or dry, damp or wet - the grip and the puddles.");
    roadRow.at = &road, roadRow.options.assign(std::begin(kRoads), std::end(kRoads));

    FullScreen("Weather", ui, width, height, [&](const Look& l) {
        Backdrop(l.d, l.W, l.H, l.k, kSurfaceBase, video, true);
        Header(l, ("W-RALLY \xC2\xB7 QUICK RALLY \xC2\xB7 " + (st ? Upper(st->title) : std::string("NO STAGE"))).c_str(), "WEATHER");
        l.d->AddRectFilled(l.P(kX0, 196), l.P(kX0 + 720, 197), kLine);
        ListKeys(rows, g_weather);
        Rows(l, rows, g_weather, 208, 720, 52);
        Help(l, rows[g_weather.at].help);
        if (Footer(l, {{"W S", "MOVE"}, {"A D", "CHANGE"}, {"ESC", "BACK"}})) g_page = Page::Quick;
    });
    s.weather = base && st ? st->weathers[base - 1] : "";
    s.time = FmtHour(hour);
    if (hour < 0) s.time.clear();
    s.season = static_cast<uint32_t>(season);
    s.road = static_cast<uint32_t>(road);
    if (KeyEsc()) g_page = Page::Quick;
}

} // namespace

void OpenGarage() { g_page = Page::Garage, g_garageFromMain = true; }

ShowroomView Showroom()
{
    ShowroomView v;
    if (g_page == Page::Garage) v.car = g_showCar;
    v.turn = g_turn;
    v.centreX = g_showX, v.centreY = g_showY;
    return v;
}

QuickChoice QuickRally(const ImGuiLayer& ui, const assets::Catalog& catalog, app::Settings& s, int width, int height,
                       const VideoFrame& video)
{
    // (A key that opens or closes a page was pressed in the frame before the page's: a press counts once, so it is not
    // the new page's too.)
    switch (g_page) {
    case Page::Quick: return QuickPage(ui, catalog, s, width, height, video);
    case Page::Stage: StagePage(ui, catalog, s, width, height, video); break;
    case Page::Garage:
        GaragePage(ui, catalog, s, width, height, video);
        if (g_garageFromMain && g_page != Page::Garage) {   // opened from the main menu: back to it
            g_garageFromMain = false, g_page = Page::Quick;
            return QuickChoice::Back;
        }
        break;
    case Page::Setup: SetupPage(ui, catalog, s, width, height, video); break;
    case Page::Weather: WeatherPage(ui, catalog, s, width, height, video); break;
    }
    return QuickChoice::None;
}

} // namespace wrally::ui
