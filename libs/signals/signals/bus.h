#pragma once
// signals - how the game's modules talk: the physics emits records (signals/car.h) on a Bus, the others (the audio, the
// renderer's side, the HUD, the particles) connect to the records they want. A module includes this and the record
// headers, never another module - as RBR's CSignalManager kept its physics, graphics and sound apart.
//
// A record is a plain struct (no pointers into a module's memory, fixed layout) with a `static constexpr uint32_t
// kKind` (Kind('E','n','g','T')) and a Stamp first. Emit calls the record's listeners at once, on the emitting thread
// (the physics' for the car's records); a listener on another thread copies what it needs into its own queue there.
// Listeners may connect, disconnect and emit (other records) from inside a listener.

#include <algorithm>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace signals {

constexpr uint32_t Kind(char a, char b, char c, char d)
{
    return uint32_t(uint8_t(a)) | uint32_t(uint8_t(b)) << 8 | uint32_t(uint8_t(c)) << 16 | uint32_t(uint8_t(d)) << 24;
}

// What every record starts with: the car it is about (0 the player's) and the physics tick it was emitted at.
struct Stamp {
    uint32_t car = 0;
    uint32_t tick = 0;
};

class Bus {
public:
    using Connection = uint64_t;   // 0: none

    template <class T> Connection Connect(std::function<void(const T&)> f)
    {
        const Connection id = ++m_next;
        m_lists[T::kKind].push_back({id, [f = std::move(f)](const void* r) { f(*static_cast<const T*>(r)); }});
        return id;
    }

    void Disconnect(Connection id)
    {
        for (auto& [kind, list] : m_lists)
            for (Listener& l : list)
                if (l.id == id) {
                    l.call = nullptr;   // dropped from the list once no emit runs
                    m_dirty = true;
                }
        Compact();
    }

    template <class T> void Emit(const T& r)
    {
        const auto it = m_lists.find(T::kKind);
        if (it == m_lists.end()) return;
        ++m_depth;
        // By index: a listener connecting appends (it hears the next emit, not this one).
        for (size_t i = 0, n = it->second.size(); i < n; ++i)
            if (const auto& call = it->second[i].call) call(&r);
        --m_depth;
        Compact();
    }

    template <class T> bool Heard() const
    {
        const auto it = m_lists.find(T::kKind);
        return it != m_lists.end() && !it->second.empty();
    }

private:
    struct Listener {
        Connection id;
        std::function<void(const void*)> call;
    };

    void Compact()
    {
        if (m_depth || !m_dirty) return;
        for (auto& [kind, list] : m_lists)
            list.erase(std::remove_if(list.begin(), list.end(), [](const Listener& l) { return !l.call; }), list.end());
        m_dirty = false;
    }

    std::unordered_map<uint32_t, std::vector<Listener>> m_lists;
    Connection m_next = 0;
    int m_depth = 0;
    bool m_dirty = false;
};

} // namespace signals
