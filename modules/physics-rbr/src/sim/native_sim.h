#pragma once
// The physics as a library on the native physics (M6 step 2): sim.h's Physics over native/run.h's world - loaded from the
// game's files alone (no exe image, no arena), stepped natively. Its results are the classic Sim's bit for bit (the
// lockstep's, phys-native-check's run mode); the bus gets the typed records only (no ClassicSignal), and the animation
// objects (Config::anims, the spectators) are not ported: Open refuses them.

#include <memory>

#include "sim/sim.h"

namespace phys::native {
struct LoadedWorld;
struct GearChange;
}

namespace phys::sim {

class NativeSim : public Physics {
public:
    NativeSim();
    ~NativeSim() override;
    std::string Open(const Config& c) override;
    void Apply(const Control& c) override;
    void ApplyWords(const uint32_t words[8]) override;
    bool Step(uint32_t n = 1) override;
    Pose CarPose() const override;
    Dashboard Dash() const override;
    void Wheels(Wheel out[4]) const override;
    void Connect(signals::Bus* bus, uint32_t car = 0) override;
    void CenterOfGravity(float out[3]) const override;
    std::vector<Box> Water() const override;
    const std::vector<Body>& Bodies() override;
    void PoseWords(uint32_t out[13]) const override;
    float StageDistance() const override;
    float Dt() const override;
    uint32_t Tick() const override;
    const std::string& Error() const override { return m_error; }
    std::string Log() const override { return m_log; }

private:
    void EmitTicks(uint32_t ticks);
    void EmitEngineInfo();
    static void GearChanged(void* ctx, const native::GearChange& g);
    static void GearOut(void* ctx);

    std::unique_ptr<native::LoadedWorld> m_world;
    std::string m_error, m_log, m_surface;
    std::vector<Body> m_bodies;
    signals::Bus* m_bus = nullptr;
    uint32_t m_busCar = 0;
};

} // namespace phys::sim
