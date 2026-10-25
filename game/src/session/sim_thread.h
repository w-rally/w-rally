#pragma once
// wrally::session::SimThread - the session on a thread of its own (docs/design.md 3.3): the physics, the stage clock,
// the driver layer and the recording step there while the main thread pumps the window, reads the keys and draws.
//
// Each frame the main thread Posts its FrameInput and takes the Latest snapshot. Inputs not yet stepped merge - their
// times add up, their presses accumulate, the keys are the newest - so a slow step never loses a key press or a moment
// of time.
//
// LOCKSTEP (--step): Post waits until that frame's steps are done, so every frame draws its own steps exactly as the
// single-threaded game did - the same ticks, the same records, the same pictures, run to run. Otherwise a frame draws
// the newest snapshot there is - usually the previous frame's steps, while this frame's run beside the drawing: at
// most a frame later, and the physics no longer waits for the GPU.
//
// The physics' records reach the bus on this thread (the audio module takes them there: AudioApi::submitRecord).

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#include "session/session.h"

namespace wrally::session {

class SimThread {
public:
    ~SimThread() { Stop(); }

    // Runs `session` (opened, connected, started) from now on; nothing else may touch it until Stop.
    void Start(Session& session, bool lockstep);
    // This frame's input; lockstep: returns once it is stepped.
    void Post(const FrameInput& in);
    // The newest snapshot.
    void Latest(Snapshot& out);
    // The thread finished (the inputs posted so far stepped) and joined.
    void Stop();

private:
    void Run();

    Session* m_session = nullptr;
    bool m_lockstep = false;
    std::thread m_thread;
    std::mutex m_lock;
    std::condition_variable m_changed;
    FrameInput m_pending;
    bool m_hasPending = false;
    bool m_stop = false;
    uint64_t m_posted = 0, m_done = 0;   // inputs posted / stepped
    Snapshot m_latest;
};

} // namespace wrally::session
