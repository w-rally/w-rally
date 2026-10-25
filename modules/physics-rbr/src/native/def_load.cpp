#include "native/defs.h"

#include <cstdlib>
#include <cstring>

#include "data/lsp.h"
#include "native/setup.h"

namespace phys::native {

namespace defkeys {
namespace {

// A value into a native member (the Tweak's kind decides which of v / u holds it).
void Put(float& m, const float* v, const uint32_t*) { m = v[0]; }
void Put(uint32_t& m, const float*, const uint32_t* u) { m = u[0]; }
void Put(int32_t& m, const float*, const uint32_t* u) { m = static_cast<int32_t>(u[0]); }
void Put(bool& m, const float*, const uint32_t* u) { m = u[0] != 0; }
void Put(Vec4& m, const float* v, const uint32_t*) { m.x = v[0], m.y = v[1], m.z = v[2]; }   // (CVector: w left)
float FromBits(uint32_t w)
{
    float f;
    std::memcpy(&f, &w, 4);
    return f;
}

// A constructor's value (its words).
void Default(float& m, uint32_t w) { std::memcpy(&m, &w, 4); }
void Default(uint32_t& m, uint32_t w) { m = w; }
void Default(int32_t& m, uint32_t w) { m = static_cast<int32_t>(w); }
void Default(bool& m, uint32_t w) { m = w != 0; }
void Default(Vec4& m, uint32_t x, uint32_t y, uint32_t z, uint32_t w)
{
    const uint32_t a[4] = {x, y, z, w};
    std::memcpy(&m, a, 16);
}

} // namespace

const Key* Table::Find(const std::string& name) const
{
    for (uint32_t i = count; i-- > 0;)
        if (name == keys[i].name) return &keys[i];
    return nullptr;
}

} // namespace defkeys

#include "native/def_keys.inc"

int KindOf(const std::string& name)
{
    for (uint32_t k = 0; k < kKinds; ++k)
        if (name == kKindNames[k]) return static_cast<int>(k);
    return -1;
}

namespace {

using data::LspNode;

// A list's label: its first string or symbol.
const std::string* Label(const LspNode& n)
{
    for (const LspNode& k : n.items)
        if (k.kind == LspNode::String || k.kind == LspNode::Symbol) return &k.text;
    return nullptr;
}

// The list after the symbol `key` among n's items, or nullptr.
const LspNode* KeyList(const LspNode& n, const char* key)
{
    for (size_t i = 0; i + 1 < n.items.size(); ++i)
        if (n.items[i].kind == LspNode::Symbol && n.items[i].text == key && n.items[i + 1].IsList()) return &n.items[i + 1];
    return nullptr;
}

// The PhysicsEngine list (at any depth), or nullptr.
const LspNode* PhysicsEngine(const LspNode& n)
{
    for (const LspNode& f : n.items) {
        if (!f.IsList()) continue;
        if (const std::string* l = Label(f); l && *l == "PhysicsEngine") return &f;
        if (const LspNode* d = PhysicsEngine(f)) return d;
    }
    return nullptr;
}

// A list's keys into the def (or nested def; null: known, not kept) through its table - the game's order: "Key value
// ...", "Flags +NAME -NAME ...", "Key (sub-keys ...)".
void BindList(void* def, const defkeys::Table& t, const LspNode& list, const std::string& cls, DefsReport& r, const char* skip = nullptr)
{
    using defkeys::Tweak;
    for (size_t i = 0; i < list.items.size(); ++i) {
        const LspNode& k = list.items[i];
        if (k.kind != LspNode::Symbol) continue;
        if (skip && k.text == skip && i + 1 < list.items.size() && list.items[i + 1].IsList()) {
            ++i;
            continue;
        }
        const defkeys::Key* key = t.Find(k.text);
        if (!key) {
            r.unbound.push_back(cls + " " + k.text);
            continue;
        }
        if (key->tweak == Tweak::Flags) {
            uint32_t scratch = 0;
            uint32_t* const bits = def && key->bits ? key->bits(def) : &scratch;
            if (!(def && key->bits)) ++r.dropped;
            for (; i + 1 < list.items.size() && list.items[i + 1].kind == LspNode::Symbol &&
                   (list.items[i + 1].text[0] == '+' || list.items[i + 1].text[0] == '-');
                 ++i) {
                const std::string& f = list.items[i + 1].text;
                uint32_t b = 0;
                while (b < key->flagCount && f.compare(1, std::string::npos, key->flagNames[b]) != 0) ++b;
                if (b == key->flagCount) {
                    r.unbound.push_back(cls + " " + k.text + " " + f);
                    continue;
                }
                if (f[0] == '+') *bits |= 1u << b;
                else *bits &= ~(1u << b);
                ++r.values;
            }
            continue;
        }
        if (i + 1 < list.items.size() && list.items[i + 1].IsList()) {   // "Key (sub-keys...)"
            if (key->tweak == Tweak::Def) BindList(def && key->subDef ? key->subDef(def) : nullptr, *key->sub, list.items[i + 1], cls + "/" + k.text, r);
            else r.unbound.push_back(cls + " " + k.text + " (a list for a value)");
            ++i;
            continue;
        }
        float v[4] = {};
        uint32_t u[4] = {};
        uint32_t c = 0;
        for (size_t j = i + 1; j < list.items.size() && list.items[j].IsNumber() && c < 4; ++j, ++c) {
            v[c] = list.items[j].value;
            u[c] = static_cast<uint32_t>(std::strtol(list.items[j].text.c_str(), nullptr, 10));   // signed too (Left -16384)
        }
        const uint32_t need = key->tweak == Tweak::Vector ? 3 : key->tweak == Tweak::Def ? ~0u : 1;
        if (c < need) {
            r.unbound.push_back(cls + " " + k.text + " (its object's type)");
            continue;
        }
        if (def && key->set) {
            key->set(def, v, u);
            ++r.values;
        } else {
            ++r.dropped;
        }
    }
}

// The kind's def of the class (made with its defaults when it has none, or one of another class) and its keys.
template <typename T>
void* KindDef(std::array<std::unique_ptr<T>, kKinds>& defs, Defs& out, uint32_t k, const std::string& cls)
{
    if (!defs[k] || out.classOf[k] != cls) {
        defs[k] = std::make_unique<T>();
        defkeys::Defaults(*defs[k]);
    }
    return defs[k].get();
}

// A DefManager list: a list per class, a def per kind.
std::string LoadDefManager(const LspNode& dm, Defs& out, DefsReport& r)
{
    for (const LspNode& c : dm.items) {
        if (!c.IsList() || !Label(c)) continue;
        const std::string& cls = *Label(c);
        const defkeys::Table* t = cls == "Solid" ? &defkeys::kSolidClassKeys
                                  : cls == "Wheel" ? &defkeys::kWheelClassKeys
                                  : cls == "Car" ? &defkeys::kCarClassKeys
                                  : cls == "CarBody" ? &defkeys::kCarBodyClassKeys
                                  : cls == "HingedBody" ? &defkeys::kHingedBodyClassKeys
                                  : cls == "MountedBody" ? &defkeys::kMountedBodyClassKeys
                                  : cls == "Ground" ? &defkeys::kGroundClassKeys
                                                    : nullptr;
        if (!t) return "no def class " + cls;
        for (const LspNode& d : c.items) {
            if (!d.IsList() || !Label(d)) continue;
            const int k = KindOf(*Label(d));
            if (k < 0) return "no kind " + *Label(d);
            void* def = cls == "Solid" ? KindDef(out.solids, out, k, cls)
                        : cls == "Wheel" ? KindDef(out.wheels, out, k, cls)
                        : cls == "Car" ? KindDef(out.cars, out, k, cls)
                        : cls == "CarBody" ? KindDef(out.carBodies, out, k, cls)
                        : cls == "HingedBody" ? KindDef(out.hinged, out, k, cls)
                        : cls == "MountedBody" ? KindDef(out.mounted, out, k, cls)
                                               : nullptr;   // (the ground's def: nothing the native physics reads)
            out.classOf[k] = cls;
            BindList(def, *t, d, cls, r);
        }
    }
    return "";
}

// A file's DefManager list (PhysicsEngine's key DefManager, at any depth), or nullptr.
const LspNode* DefManagerOf(const LspNode& root)
{
    for (const LspNode& f : root.items) {
        if (!f.IsList()) continue;
        if (const std::string* l = Label(f); l && *l == "PhysicsEngine")
            if (const LspNode* d = KeyList(f, "DefManager")) return d;
        if (const LspNode* d = DefManagerOf(f)) return d;
    }
    return nullptr;
}

} // namespace

void* Defs::Of(uint32_t k, const defkeys::Table** keys) const
{
    const std::string& c = classOf[k];
    auto pick = [&](const auto& defs, const defkeys::Table& t) -> void* {
        *keys = &t;
        return defs[k].get();
    };
    if (c == "Solid" || (c.empty() && solids[k])) return pick(solids, defkeys::kSolidClassKeys);
    if (c == "Wheel") return pick(wheels, defkeys::kWheelClassKeys);
    if (c == "Car") return pick(cars, defkeys::kCarClassKeys);
    if (c == "CarBody") return pick(carBodies, defkeys::kCarBodyClassKeys);
    if (c == "HingedBody") return pick(hinged, defkeys::kHingedBodyClassKeys);
    if (c == "MountedBody") return pick(mounted, defkeys::kMountedBodyClassKeys);
    *keys = nullptr;
    return nullptr;
}

std::string LoadPhysicsLsp(const std::string& text, Defs& out, DefsReport& r)
{
    const LspNode root = data::ParseLsp(text);
    const LspNode* pe = PhysicsEngine(root);
    if (!pe) return "physics.lsp: no PhysicsEngine";
    defkeys::Defaults(out.physics);
    BindList(&out.physics, defkeys::kPhysicsConfigKeys, *pe, "PhysicsEngine", r, "DefManager");   // (the DefManager: the defs')
    const LspNode* dm = KeyList(*pe, "DefManager");
    if (!dm) return "physics.lsp: no DefManager";
    if (std::string e = LoadDefManager(*dm, out, r); !e.empty()) return e;
    for (uint32_t k = 1; k <= 32; ++k)   // the stage objects no file names: the default Solid def
        if (!out.solids[k]) {
            out.solids[k] = std::make_unique<SolidDef>();
            defkeys::Defaults(*out.solids[k]);
        }
    return "";
}

namespace {

// A file's first list - "(("label" Key (sub-keys...) ...))" - into a container (BindFile).
void BindFile(void* container, const defkeys::Table& t, const std::string& text, const char* cls, DefsReport& r)
{
    const LspNode root = data::ParseLsp(text);
    for (const LspNode& f : root.items)
        if (f.IsList()) {
            for (const LspNode& g : f.items)
                if (g.IsList()) {
                    BindList(container, t, g, cls, r);
                    return;
                }
            BindList(container, t, f, cls, r);
            return;
        }
}

} // namespace

void Defaults(CarSetup& s) { defkeys::Defaults(s); }
void Defaults(CarSetupOptions& o) { defkeys::Defaults(o); }

std::string LoadSetup(const std::string& text, CarSetup& out, DefsReport& r)
{
    BindFile(&out, defkeys::kCarSetupKeys, text, "CarSetup", r);
    return "";
}

std::string LoadSetupOptions(const std::string& text, CarSetupOptions& out, DefsReport& r)
{
    BindFile(&out, defkeys::kCarSetupOptionsKeys, text, "CarSetupOptions", r);
    return "";
}

std::string LoadDefsLsp(const std::string& text, Defs& out, DefsReport& r)
{
    const LspNode root = data::ParseLsp(text);
    const LspNode* dm = DefManagerOf(root);
    if (!dm) return "a file without a PhysicsEngine DefManager";
    return LoadDefManager(*dm, out, r);
}

} // namespace phys::native
