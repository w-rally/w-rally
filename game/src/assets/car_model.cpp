#include "assets/car_model.h"

#include <windows.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>

namespace wrally::assets {

namespace {

std::vector<uint8_t> ReadFile(const std::string& path)
{
    std::vector<uint8_t> d;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return d;
    std::fseek(f, 0, SEEK_END);
    d.resize(static_cast<size_t>(std::ftell(f)));
    std::fseek(f, 0, SEEK_SET);
    d.resize(std::fread(d.data(), 1, d.size(), f));
    std::fclose(f);
    return d;
}

std::string Lower(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

struct Cur {
    const uint8_t* p;
    size_t at, end;
    void Need(size_t n) const
    {
        if (at + n > end) throw std::runtime_error(".sgc: a mesh record is truncated");
    }
    uint32_t U32()
    {
        Need(4);
        uint32_t v;
        std::memcpy(&v, p + at, 4);
        at += 4;
        return v;
    }
    void F(float* out, int n)
    {
        Need(4 * size_t(n));
        std::memcpy(out, p + at, 4 * size_t(n));
        at += 4 * size_t(n);
    }
    std::string Str()
    {
        const size_t from = at;
        while (at < end && p[at]) ++at;
        if (at == end) throw std::runtime_error(".sgc: a name runs past its record");
        return std::string(reinterpret_cast<const char*>(p + from), at++ - from);
    }
};

bool Starts(const std::string& s, const char* prefix) { return s.compare(0, std::strlen(prefix), prefix) == 0; }

// A mesh's lamp group (LampMeshGroup), by its name with any _lit taken off; -1 not a lamp's.
int LampGroupOf(std::string t)
{
    if (t.size() > 4 && t.compare(t.size() - 4, 4, "_lit") == 0) t.resize(t.size() - 4);
    if (t == "e_light_lf" || t == "e_light_rf") return kLampHead;
    if (t == "e_light_lb" || t == "e_light_rb") return kLampTail;
    if (Starts(t, "e_light_brake_") || t == "e_light_lit_c") return kLampBrake;   // lit_c: the high centre brake lamp
    if (Starts(t, "e_light_reverse_")) return kLampReverse;
    if (t == "e_extralight" || t == "e_ramp_light") return kLampRamp;            // the pods (the i20's, the 207's)
    if (t == "e_bonnet_ramp" || t == "e_ramp" || t == "e_extralight_glass" || t == "e_ramp_light_glass") return kLampRampHousing;
    return -1;
}

// Which exterior meshes are drawn.
bool Drawn(const std::string& n, const std::string& surface)
{
    if (!Starts(n, "e_")) return false;
    const std::string t = n.substr(0, n.find_last_not_of(' ') + 1);   // a name may end in a space ("e_light_brake_rb_lit ")
    // A lit twin: a lamp's (switched with it), else not drawn (a hot brake disc, ...).
    if (t.size() > 4 && t.compare(t.size() - 4, 4, "_lit") == 0) return LampGroupOf(t) >= 0;
    for (const char* s : {"tarmac", "gravel", "snow"})
        if (n.find(std::string("_") + s) != std::string::npos && surface != s) return false;   // another surface's wheels
    return true;
}

// <car>_shaders.ini: sections and their keys.
using Ini = std::map<std::string, std::map<std::string, std::string>>;
Ini ReadIni(const std::string& path)
{
    Ini ini;
    const std::vector<uint8_t> d = ReadFile(path);
    std::string section, line;
    auto flush = [&] {
        auto trim = [](std::string s) {
            const size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
            return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
        };
        line = trim(line);
        if (line.size() > 2 && line.front() == '[' && line.back() == ']') section = line.substr(1, line.size() - 2);
        else if (const size_t eq = line.find('='); eq != std::string::npos) ini[section][trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
        line.clear();
    };
    for (uint8_t c : d) {
        if (c == '\n') flush();
        else line.push_back(static_cast<char>(c));
    }
    flush();
    return ini;
}

} // namespace

CarModel ReadCarModel(const std::string& sgcPath, const std::string& surface)
{
    const std::vector<uint8_t> d = ReadFile(sgcPath);
    if (d.empty()) throw std::runtime_error("cannot read " + sgcPath);
    const std::string dir = sgcPath.substr(0, sgcPath.find_last_of("/\\") + 1);
    std::string base = sgcPath.substr(dir.size());
    base = base.substr(0, base.find_last_of('.'));
    const Ini ini = ReadIni(dir + base + "_shaders.ini");
    // <car>.ini: per mesh ([<name>]), its groups' materials (FaceBlock<g>_Material) - what the game draws, over the
    // .sgc's own names (often a placeholder: "Default Material", "09___Default").
    const Ini carIni = ReadIni(dir + base + ".ini");

    CarModel model;
    std::map<std::string, std::vector<std::string>> stageNames;   // the .sgc materials' texture names

    // The node tree: every named record's id, parent and matrix (meshes and helpers: after the 14-float local
    // transform; transform nodes, kind 19, right after the ids - only where nothing else names the id).
    struct Node {
        std::string name;
        uint32_t parent = 0xffffffffu;
        float m[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    };
    std::map<uint32_t, Node> nodes;
    for (int pass = 0; pass < 2; ++pass)
        for (size_t at = 0; at + 20 <= d.size(); ++at) {
            uint32_t h[5];
            std::memcpy(h, d.data() + at, 20);
            if (h[0] != 0xc || h[3] != 1 || h[4] == 0 || at + 20 + h[4] > d.size()) continue;
            const bool mat = (h[1] == 0 && (h[2] == 1 || h[2] == 3)) || (h[1] == 3 && h[2] == 13) || (h[1] == 2 && h[2] == 11);
            if (pass == 0 ? !mat : !(h[1] == 19)) continue;
            try {
                Cur c{d.data(), at + 20, at + 20 + h[4]};
                Node n;
                n.name = c.Str();
                const uint32_t id = c.U32();
                n.parent = c.U32();
                if (pass == 0) c.at += 14 * 4;
                c.F(n.m, 12);
                if (pass == 0 || !nodes.count(id)) nodes[id] = n;
            } catch (const std::exception&) {
            }
        }
    auto apply = [](const float* m, const float* v, float* out) {
        float r[3];
        for (int k = 0; k < 3; ++k) r[k] = m[4 * k] * v[0] + m[4 * k + 1] * v[1] + m[4 * k + 2] * v[2] + m[4 * k + 3];
        std::memcpy(out, r, 12);
    };
    auto turn = [](const float* m, const float* v, float* out) {
        float r[3];
        for (int k = 0; k < 3; ++k) r[k] = m[4 * k] * v[0] + m[4 * k + 1] * v[1] + m[4 * k + 2] * v[2];
        std::memcpy(out, r, 12);
    };
    // a = a then b (b applied after a): the composed rows
    auto then = [](const float* a, const float* b, float* out) {
        float r[12];
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) r[4 * i + j] = b[4 * i] * a[j] + b[4 * i + 1] * a[4 + j] + b[4 * i + 2] * a[8 + j];
            r[4 * i + 3] = b[4 * i] * a[3] + b[4 * i + 1] * a[7] + b[4 * i + 2] * a[11] + b[4 * i + 3];
        }
        std::memcpy(out, r, 48);
    };
    static const char* const kWheels[4] = {"lf", "rf", "lb", "rb"};
    // A node's matrix to the car's frame, or to its hub's when it hangs under a Point_e_wheel_<w> (*wheel = w).
    auto frame = [&](uint32_t id, float* out, int* wheel) {
        float m[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        *wheel = -1;
        for (int guard = 0; guard < 64; ++guard) {
            const auto it = nodes.find(id);
            if (it == nodes.end() || it->second.parent == 0xffffffffu) break;   // the root: left out
            for (int w = 0; w < 4; ++w)
                if (it->second.name == std::string("Point_e_wheel_") + kWheels[w]) *wheel = w;
            if (*wheel >= 0) break;
            then(m, it->second.m, m);
            id = it->second.parent;
        }
        std::memcpy(out, m, 48);
    };
    for (size_t at = 0; at + 20 <= d.size(); ++at) {
        uint32_t h[5];
        std::memcpy(h, d.data() + at, 20);
        if (h[0] != 0xc || h[3] != 1 || h[4] == 0 || at + 20 + h[4] > d.size()) continue;
        Cur c{d.data(), at + 20, at + 20 + h[4]};
        if (h[1] == 5 && h[2] == 18) {   // a material: name, stages {name, 0x20 bytes}
            try {
                const std::string name = c.Str();
                const uint32_t n = c.U32();
                std::vector<std::string> stages;
                for (uint32_t k = 0; k < n && k < 8; ++k) {
                    stages.push_back(c.Str());
                    c.at += 0x20;
                }
                stageNames[name] = stages;
            } catch (const std::exception&) {
            }
            continue;
        }
        if (h[1] != 0 || (h[2] != 1 && h[2] != 3)) continue;
        const std::string name = c.Str();
        if (!Drawn(name, surface)) {
            at += 19 + h[4];
            continue;
        }
        const uint32_t id = c.U32();
        c.U32();
        float local[14], world[12], bounds[6];
        c.F(local, 14);
        c.F(world, 12);
        c.F(bounds, 6);
        // Where it goes: a wheel on its hub (turned about y on the right), a part under a hub in the hub's frame, the
        // rest in the car's frame.
        int wheel = -1;
        bool isWheel = false;
        for (int w = 0; w < 4; ++w)
            if (Starts(name, "e_wheel_") && name.size() > 3 && name.compare(name.size() - 3, 3, std::string("_") + kWheels[w]) == 0) {
                wheel = w;
                isWheel = true;
            }
        if (isWheel) {
            const float right[12] = {-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0}, left[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
            std::memcpy(world, wheel == 1 || wheel == 3 ? right : left, 48);
        } else {
            frame(id, world, &wheel);
        }
        const uint32_t nv = c.U32(), flags = c.U32();
        if (!(flags & 1) || (flags >> 24)) {   // no positions, or the other vertex layout: not drawn
            at += 19 + h[4];
            continue;
        }
        std::vector<CarVertex> verts(nv);
        for (uint32_t k = 0; k < nv; ++k) {
            float pn[6];
            c.F(pn, 6);
            apply(world, pn, verts[k].pos);   // to the car's (or the hub's) frame; normals turned alike
            turn(world, pn + 3, verts[k].normal);
        }
        if (flags & 4)
            for (uint32_t k = 0; k < nv; ++k) c.F(verts[k].uv, 2);
        if (flags & 0x10) {
            c.Need(4 * size_t(nv));
            c.at += 4 * size_t(nv);
        }
        const uint32_t nt = c.U32();
        c.Need(6 * size_t(nt));
        std::vector<uint32_t> idx(3 * size_t(nt));
        for (size_t k = 0; k < idx.size(); ++k) {
            uint16_t v;
            std::memcpy(&v, d.data() + c.at + 2 * k, 2);
            idx[k] = v;
        }
        c.at += 6 * size_t(nt);
        // The names above it in the node tree (a grille under its bumper goes where the bumper goes).
        std::vector<std::string> parents;
        if (const auto self = nodes.find(id); self != nodes.end())
            for (uint32_t p = self->second.parent, guard = 0; guard < 64; ++guard) {
                const auto it = nodes.find(p);
                if (it == nodes.end() || it->second.parent == 0xffffffffu) break;   // the root: left out
                parents.push_back(it->second.name);
                p = it->second.parent;
            }
        const uint32_t ng = c.U32();
        for (uint32_t g = 0; g < ng; ++g) {
            CarPart part;
            part.name = name;
            {
                const std::string t = name.substr(0, name.find_last_not_of(' ') + 1);
                part.lampGroup = LampGroupOf(t);
                part.lit = part.lampGroup >= 0 && t.size() > 4 && t.compare(t.size() - 4, 4, "_lit") == 0;
            }
            part.parents = parents;
            part.wheel = wheel;
            part.spins = wheel >= 0 && (Starts(name, "e_wheel_") || Starts(name, "e_brakedisc_"));
            part.material = c.Str();
            if (const auto sec = carIni.find(name); sec != carIni.end())
                if (const auto fb = sec->second.find("FaceBlock" + std::to_string(g) + "_Material"); fb != sec->second.end() && !fb->second.empty())
                    part.material = fb->second;
            const uint32_t tris = c.U32(), first = c.U32();
            c.U32();
            c.U32();
            for (size_t k = first; k < first + 3 * size_t(tris) && k < idx.size(); ++k) part.indices.push_back(idx[k]);
            part.vertices = verts;   // (each group keeps the mesh's vertices; its indices pick)
            if (!part.indices.empty()) model.parts.push_back(std::move(part));
        }
        at += 19 + h[4];
    }

    // The materials used: a texture through the shaders.ini (<material>_diffuse_bitmap, or <material>::..._diffuse_bitmap),
    // else the .sgc's texture name (or one in the textures folder ending as it does); without one, the ini's Diffuse.
    std::map<std::string, size_t> done;
    std::vector<std::string> files;   // the textures folder
    {
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((dir + "textures\\*.dds").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do files.push_back(fd.cFileName);
            while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
    auto textureFile = [&](const std::string& material) -> std::string {
        auto bitmap = [&](const std::string& section) -> std::string {
            const auto s = ini.find(section);
            if (s == ini.end()) return "";
            const auto f = s->second.find("Filename");
            return f == s->second.end() ? "" : f->second;
        };
        if (std::string f = bitmap(material + "_diffuse_bitmap"); !f.empty()) return f;
        for (const auto& [section, keys] : ini)
            if (Starts(section, (material + "::").c_str()) && section.size() > 15 &&
                section.compare(section.size() - 15, 15, "_diffuse_bitmap") == 0)
                if (const auto f = keys.find("Filename"); f != keys.end()) return f->second;
        const auto st = stageNames.find(material);
        if (st == stageNames.end() || st->second.empty()) return "";
        const std::string want = Lower(st->second[0]);
        for (const std::string& f : files)
            if (Lower(f) == want) return f;
        // a renamed model: the file whose name ends as the .sgc's does after its first two words (i20_wrc_16_x.dds ->
        // ..._x.dds)
        size_t cut = want.find('_');
        while (cut != std::string::npos) {
            const std::string tail = want.substr(cut);
            std::string hit;
            int hits = 0;
            for (const std::string& f : files)
                if (Lower(f).size() >= tail.size() && Lower(f).compare(Lower(f).size() - tail.size(), tail.size(), tail) == 0) ++hits, hit = f;
            if (hits == 1) return hit;
            cut = want.find('_', cut + 1);
        }
        return "";
    };
    for (const CarPart& p : model.parts) {
        if (done.count(p.material)) continue;
        done[p.material] = model.materials.size();
        CarTexture t;
        t.material = p.material;
        const std::string file = textureFile(p.material);
        if (!file.empty()) t.dds = ReadFile(dir + "textures/" + file);
        if (const auto s = ini.find(p.material); s != ini.end()) {
            if (const auto df = s->second.find("Diffuse"); df != s->second.end())
                std::sscanf(df->second.c_str(), "%f %f %f", &t.diffuse[0], &t.diffuse[1], &t.diffuse[2]);
            if (const auto op = s->second.find("OpacityChannel"); op != s->second.end()) t.transparent = op->second == "true";
        }
        if (Lower(file).find("transparent") != std::string::npos || Lower(p.material).find("transparent") != std::string::npos) t.transparent = true;
        model.materials.push_back(std::move(t));
    }
    return model;
}

} // namespace wrally::assets
