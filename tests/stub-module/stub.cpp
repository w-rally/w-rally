// wstub.dll - a module that does nothing, for testing the handshake (ModuleHost, module-info).
//
//   physics: the game's version; InitWInterface checks the header as every module must, fills ModuleInfo and a table
//            whose calls all answer ErrNotSupported.
//   audio:   ONE MORE than the game's version - the incompatible case, which the game must list and never initialise.

#include <cstring>

#include "wrally/iface/audio.h"
#include "wrally/iface/physics.h"

using namespace wrally::iface;

namespace {

struct PhysicsModule_ {
    HostServices host;
};

const char* WRALLY_CALL LastError(PhysicsModule*) { return "the stub module does nothing"; }
void WRALLY_CALL Destroy(PhysicsModule* m) { delete reinterpret_cast<PhysicsModule_*>(m); }
Status WRALLY_CALL CreateWorld(PhysicsModule*, const WorldDesc*, World** out)
{
    if (out) *out = nullptr;
    return Status::ErrNotSupported;
}
void WRALLY_CALL DestroyWorld(World*) {}
const char* WRALLY_CALL WorldError(World*) { return "no world"; }
float WRALLY_CALL Dt(World*) { return 0; }
uint32_t WRALLY_CALL Tick(World*) { return 0; }
Status WRALLY_CALL SetControls(World*, uint32_t, const Controls*) { return Status::ErrNotSupported; }
Status WRALLY_CALL Step(World*, uint32_t) { return Status::ErrNotSupported; }
Status WRALLY_CALL GetCarState(World*, uint32_t, CarState*) { return Status::ErrNotSupported; }
Status WRALLY_CALL CenterOfGravity(World*, uint32_t, float*) { return Status::ErrNotSupported; }
uint32_t WRALLY_CALL Bodies(World*, BodyState*, uint32_t) { return 0; }
uint32_t WRALLY_CALL Water(World*, Box*, uint32_t) { return 0; }
Status WRALLY_CALL ResetCar(World*, uint32_t, const ResetDesc*) { return Status::ErrNotSupported; }
Status WRALLY_CALL SaveState(World*, Blob*) { return Status::ErrNotSupported; }
void WRALLY_CALL ReleaseState(World*, Blob*) {}
Status WRALLY_CALL LoadState(World*, const void*, uint64_t) { return Status::ErrNotSupported; }

uint32_t WRALLY_CALL CarParts(World*, uint32_t, PartState*, uint32_t) { return 0; }

const PhysicsApi kApi = {&Destroy, &LastError, &CreateWorld, &DestroyWorld, &WorldError, &Dt, &Tick,
                         &SetControls, &Step, &GetCarState, &CenterOfGravity, &Bodies, &Water, &ResetCar,
                         &SaveState, &ReleaseState, &LoadState, &CarParts};

bool HeaderIs(const StructHeader* h, ModuleKind kind, uint32_t ver, uint32_t size)
{
    return h && h->kind == kind && h->interfaceVer == ver && h->size == size;
}

} // namespace

WRALLY_EXPORT uint32_t WRALLY_CALL GetWInterfaceVer(ModuleKind kind)
{
    switch (kind) {
    case ModuleKind::Physics: return kPhysicsInterfaceVer;
    case ModuleKind::Audio: return kAudioInterfaceVer + 1;
    default: return 0;
    }
}

WRALLY_EXPORT Status WRALLY_CALL InitWInterface(uint32_t gameVersion, const StructHeader* in, StructHeader* out)
{
    if (!in || !out) return Status::ErrInvalidArg;
    if (in->kind != ModuleKind::Physics || out->kind != ModuleKind::Physics) return Status::ErrKind;
    if (in->interfaceVer != kPhysicsInterfaceVer || out->interfaceVer != kPhysicsInterfaceVer) return Status::ErrVersion;
    if (!HeaderIs(in, ModuleKind::Physics, kPhysicsInterfaceVer, sizeof(PhysicsInput)) ||
        !HeaderIs(out, ModuleKind::Physics, kPhysicsInterfaceVer, sizeof(PhysicsOutput)))
        return Status::ErrStructSize;
    if (gameVersion < MakeVersion(0, 1, 0)) return Status::ErrGameVersion;

    const auto& input = *reinterpret_cast<const PhysicsInput*>(in);
    auto& output = *reinterpret_cast<PhysicsOutput*>(out);
    auto* m = new PhysicsModule_{input.host};
    if (m->host.log) m->host.log(m->host.ctx, LogLevel::Info, "stub physics initialised");

    output.info = {};
    std::strcpy(output.info.id, "stub");
    std::strcpy(output.info.name, "Stub physics (tests)");
    output.info.moduleVersion = MakeVersion(0, 0, 1);
    output.info.minGameVersion = MakeVersion(0, 1, 0);
    output.info.buildHash = 0x57554253ull;   // 'STUB'
    output.info.caps = 0;
    output.api = &kApi;
    output.instance = reinterpret_cast<PhysicsModule*>(m);
    return Status::Ok;
}
