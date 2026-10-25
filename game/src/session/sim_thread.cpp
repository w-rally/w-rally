#include "session/sim_thread.h"

#include <utility>

namespace wrally::session {

void SimThread::Start(Session& session, bool lockstep)
{
    m_session = &session;
    m_lockstep = lockstep;
    session.Snap(m_latest);
    m_thread = std::thread([this] { Run(); });
}

void SimThread::Post(const FrameInput& in)
{
    std::unique_lock<std::mutex> lock(m_lock);
    if (m_hasPending) {   // not stepped yet: merged into the next
        m_pending.seconds += in.seconds;
        m_pending.presses |= in.presses;
        const double seconds = m_pending.seconds;
        const uint32_t presses = m_pending.presses;
        m_pending = in;
        m_pending.seconds = seconds;
        m_pending.presses = presses;
    } else {
        m_pending = in;
        m_hasPending = true;
    }
    const uint64_t mine = ++m_posted;
    m_changed.notify_all();
    if (m_lockstep) m_changed.wait(lock, [&] { return m_done >= mine || !m_thread.joinable(); });
}

void SimThread::Latest(Snapshot& out)
{
    std::lock_guard<std::mutex> lock(m_lock);
    out = m_latest;
}

void SimThread::Run()
{
    Snapshot snap;
    for (;;) {
        FrameInput in;
        uint64_t upTo;
        {
            std::unique_lock<std::mutex> lock(m_lock);
            m_changed.wait(lock, [&] { return m_hasPending || m_stop; });
            if (!m_hasPending) return;   // stopped, nothing left
            in = m_pending;
            m_hasPending = false;
            upTo = m_posted;
        }
        m_session->Advance(in);
        m_session->Snap(snap);
        {
            std::lock_guard<std::mutex> lock(m_lock);
            std::swap(m_latest, snap);
            m_done = upTo;
        }
        m_changed.notify_all();
    }
}

void SimThread::Stop()
{
    if (!m_thread.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(m_lock);
        m_stop = true;
    }
    m_changed.notify_all();
    m_thread.join();
}

} // namespace wrally::session
