// The stage clock and the stage helpers of sim/sim.h (StageClock, StageMap, StageSurface, SurfaceTyreType), apart from
// the classic Sim they shared a file with in plugin-rbr.
#include "sim/sim.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace phys::sim {

void StageClock::SetMarks(float finish, std::vector<float> checkpoints)
{
    m_finish = finish;
    m_checkpoints = std::move(checkpoints);
    std::sort(m_checkpoints.begin(), m_checkpoints.end());
}

void StageClock::Reset(float countdown)
{
    m_state = State::Countdown;
    m_countdown = countdown;
    m_time = m_message = m_splitShown = 0;
    m_falseStart = false;
    m_splits.clear();
}

StageClock::Event StageClock::Advance(float dt, float speed, float distance)
{
    if (m_message > 0) m_message -= dt;
    if (m_splitShown > 0) m_splitShown -= dt;
    if (m_state == State::Countdown) {
        const float before = m_countdown;
        m_countdown -= dt;
        if (m_countdown > 0 && m_countdown < 4.0f && speed > 1.0f) {   // 0x579590: moving in the last 4 s
            m_time = m_countdown + kFalseStartPenalty;
            m_countdown = 0;
            m_falseStart = true;
            m_message = 3.0f;
            m_state = State::Running;
            return kFalseStart;
        }
        if (m_countdown < 0 && before >= 0) {   // GO: the race time from here
            m_state = State::Running;
            return kGo;
        }
        return kNone;
    }
    if (m_countdown > -2.0f) m_countdown -= dt;   // GO shown down to -2
    if (m_state != State::Running) return kNone;
    m_time += dt;
    if (m_splits.size() < m_checkpoints.size() && distance >= m_checkpoints[m_splits.size()]) {
        m_splits.push_back(m_time);
        m_splitShown = 5.0f;
        return kSplit;
    }
    if (m_finish >= 0 && m_splits.size() == m_checkpoints.size() && distance >= m_finish) {
        m_state = State::Finished;
        return kFinish;
    }
    return kNone;
}

std::string StageClock::Text(float t)
{
    char b[32];
    const int cs = static_cast<int>(std::lround(std::max(0.0f, t) * 100.0f));
    std::snprintf(b, sizeof b, "%d:%02d.%02d", cs / 6000, cs / 100 % 60, cs % 100);
    return b;
}

std::string StageClock::Shown() const
{
    if (m_state == State::Finished) return "FINISH  " + Text(m_time);
    if (m_splitShown > 0 && !m_splits.empty()) return "split " + std::to_string(m_splits.size()) + "  " + Text(m_splits.back());
    if (m_falseStart) return m_message > 0 ? "FALSE START" : "";
    if (m_countdown > 3.0f || m_countdown <= -2.0f) return "";
    if (m_countdown > 2.0f) return "3";
    if (m_countdown > 1.0f) return "2";
    if (m_countdown > 0.0f) return "1";
    return "GO";
}

const char* const kTyreTypeNames[7] = {"tarmac_dry", "tarmac_intermediate", "tarmac_wet", "gravel_dry",
                                       "gravel_intermediate", "gravel_wet", "snow"};

int StageMap(const std::string& stage)
{
    const size_t slash = stage.find_last_of("/\\");
    const char* s = stage.c_str() + (slash == std::string::npos ? 0 : slash + 1);
    if (_strnicmp(s, "track-", 6)) return -1;
    char* end;
    const long map = std::strtol(s + 6, &end, 10);
    return end == s + 6 ? -1 : static_cast<int>(map);
}

std::string StageSurface(const std::string& game, int map, std::string* from)
{
    static const char* const kSurfaces[3] = {"tarmac", "gravel", "snow"};
    char section[32], value[32];
    std::snprintf(section, sizeof section, "Map%02d", map);
    const std::string ini = game + "/Maps/Tracks.ini";
    if (map >= 0 && GetPrivateProfileStringA(section, "Surface", "", value, sizeof value, ini.c_str())) {
        const unsigned long v = std::strtoul(value, nullptr, 10);
        if (v < 3) {
            if (from) *from = "Maps/Tracks.ini [" + std::string(section) + "] Surface=" + value;
            return kSurfaces[v];
        }
    }
    if (from) *from = "the game's map table";
    if (map >= 10 && map <= 18) return "snow";
    if ((map >= 41 && map <= 49) || map == 73 || map == 85 || map == 86 || map == 94) return "tarmac";
    return "gravel";
}

uint32_t SurfaceTyreType(const std::string& surface, int32_t wetness)
{
    const uint32_t w = wetness < 0 ? 0 : wetness > 2 ? 2 : static_cast<uint32_t>(wetness);
    return surface == "snow" ? 6 : surface == "gravel" ? 3 + w : w;
}

} // namespace phys::sim
