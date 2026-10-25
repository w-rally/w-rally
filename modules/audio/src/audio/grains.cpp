#include "audio/grains.h"

#include <algorithm>
#include <cmath>

namespace audio {

namespace {

constexpr float kPi = 3.14159265f;
constexpr float kRate = 48000;

} // namespace

std::vector<GrainLoop> CutGrains(const std::vector<EngineLoop>& loops, uint32_t cylinders)
{
    std::vector<GrainLoop> out;
    for (const EngineLoop& l : loops) {
        GrainLoop g;
        g.sample = &l.sample;
        g.rpm = l.rpm;
        g.load = l.load;
        const std::vector<float>& x = l.sample.data;
        const size_t n = x.size();
        const float p0 = l.sample.rate * 120 / (l.rpm * float(cylinders));   // samples between firings
        if (n < size_t(8 * p0) || p0 < 8) {
            out.push_back(g);
            continue;
        }
        // The envelope: |x| smoothed both ways (no lag) over an eighth of a period.
        std::vector<float> e(n);
        const float a = std::exp(-8.0f / p0);
        float s = 0;
        for (size_t i = 0; i < n; ++i) e[i] = s = a * s + (1 - a) * std::fabs(x[i]);
        s = e[n - 1];
        for (size_t i = n; i-- > 0;) e[i] = s = a * s + (1 - a) * e[i];
        double mean = 0;
        for (float v : e) mean += v;
        mean /= double(n);
        std::vector<float> c(n);
        double energy = 0;
        for (size_t i = 0; i < n; ++i) c[i] = e[i] - float(mean), energy += double(c[i]) * c[i];

        // The period: the envelope's autocorrelation's best lag near the label's.
        auto corr = [&](size_t lag) {
            double sum = 0;
            for (size_t i = 0; i + lag < n; ++i) sum += double(c[i]) * c[i + lag];
            return sum / (energy * double(n - lag) / double(n) + 1e-20);
        };
        const size_t lo = static_cast<size_t>(p0 * 0.93f), hi = static_cast<size_t>(p0 * 1.07f) + 1;
        size_t best = static_cast<size_t>(p0 + 0.5f);
        double bestCorr = -2;
        std::vector<double> r(hi + 2, 0.0);
        for (size_t lag = lo > 1 ? lo - 1 : 1; lag <= hi + 1; ++lag) r[lag] = corr(lag);
        for (size_t lag = lo; lag <= hi; ++lag)
            if (r[lag] > bestCorr) bestCorr = r[lag], best = lag;
        const double d = r[best - 1] - 2 * r[best] + r[best + 1];   // the parabola through the best and its neighbours
        g.period = static_cast<float>(best + (d < 0 ? 0.5 * (r[best - 1] - r[best + 1]) / d : 0.0));
        g.periodicity = static_cast<float>(bestCorr);

        // The pulses' place in the period: the envelope folded over it, its peak.
        constexpr int kBins = 96;
        double fold[kBins] = {};
        for (size_t i = 0; i < n; ++i) fold[int(std::fmod(double(i), double(g.period)) / g.period * kBins) % kBins] += e[i];
        const int peak = static_cast<int>(std::max_element(fold, fold + kBins) - fold);

        // The marks: the first at the envelope's peak in the period (the fold's), then each next one where a period of
        // the signal around it is most like the period around the last (the normalized cross-correlation, within a tenth
        // of a period of a period on) - so consecutive grains line up in shape and overlap cleanly. (The envelope's peak
        // alone wanders +-17 % on a real recording's noise: the grains then do not line up, heard as dropouts.)
        const float before = (GrainEngine::kAttack + 0.1f) * g.period, after = (GrainEngine::kTail + 0.1f) * g.period;
        const long half = static_cast<long>(g.period / 2), reach = std::max(1L, static_cast<long>(g.period / 10));
        auto like = [&](long a, long b) {   // the normalized correlation of the periods around a and b
            double ab = 0, aa = 0, bb = 0;
            for (long i = -half; i < half; ++i) {
                const double u = x[size_t(a + i)], v = x[size_t(b + i)];
                ab += u * v, aa += u * u, bb += v * v;
            }
            return ab / std::sqrt(aa * bb + 1e-20);
        };
        long m = static_cast<long>((peak + 0.5) / kBins * g.period);
        while (m < half || float(m) < before) m += static_cast<long>(g.period);
        for (; float(m) + after < float(n) && m + half + reach < long(n);) {
            g.marks.push_back(static_cast<uint32_t>(m));
            const long guess = m + static_cast<long>(g.period + 0.5f);
            long next = guess;
            double nextLike = -2;
            for (long cand = guess - reach; cand <= guess + reach; ++cand)
                if (cand + half < long(n))
                    if (const double sim = like(m, cand); sim > nextLike) nextLike = sim, next = cand;
            m = next;
        }
        // A grain far quieter than the loop's usual one (a silent lead-in, a dropout) is no firing; one far brighter (its
        // share of high frequencies - the first difference's energy over the energy - over 2.5 x the median: a click, a
        // stone, another noise in the recording) is no engine. Both dropped (each would come back at every pass).
        if (!g.marks.empty()) {
            std::vector<float> level, bright;
            for (uint32_t k : g.marks) {
                double sum = 0, diff = 0;
                const size_t from = static_cast<size_t>(float(k) - before), to = static_cast<size_t>(float(k) + after);
                for (size_t i = from; i < to; ++i) sum += double(x[i]) * x[i], diff += i > from ? double(x[i] - x[i - 1]) * (x[i] - x[i - 1]) : 0.0;
                level.push_back(static_cast<float>(std::sqrt(sum / double(to - from))));
                bright.push_back(static_cast<float>(diff / (sum + 1e-20)));
            }
            auto middle = [](std::vector<float> v) {
                std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
                return v[v.size() / 2];
            };
            const float floor = 0.5f * middle(level), ceiling = 2.5f * middle(bright);
            std::vector<uint32_t> kept;
            for (size_t i = 0; i < g.marks.size(); ++i)
                if (level[i] >= floor && bright[i] <= ceiling) kept.push_back(g.marks[i]);
            g.marks = std::move(kept);
            // The slow level around each grain, against the loop's median of it.
            const long reachSlow = static_cast<long>(0.25f * l.sample.rate);
            std::vector<double> sq(n + 1, 0.0);
            for (size_t i = 0; i < n; ++i) sq[i + 1] = sq[i] + double(x[i]) * x[i];
            std::vector<float> slow;
            for (uint32_t k : g.marks) {
                const size_t from = static_cast<size_t>(std::max(0L, long(k) - reachSlow)), to = std::min(n, size_t(k) + size_t(reachSlow));
                slow.push_back(static_cast<float>(std::sqrt((sq[to] - sq[from]) / double(to - from))));
            }
            std::vector<float> s2 = slow;
            std::nth_element(s2.begin(), s2.begin() + s2.size() / 2, s2.end());
            const float median = s2.empty() ? 0.0f : s2[s2.size() / 2];
            for (float around : slow) g.gains.push_back(std::clamp(median / std::max(around, 1e-6f), 0.25f, 4.0f));
        }
        out.push_back(std::move(g));
    }
    return out;
}

void GrainEngine::Set(const std::vector<GrainLoop>* loops)
{
    m_loops = loops;
    for (std::vector<float>& r : m_rpm) r.clear();
    m_cursor.assign(loops ? loops->size() : 0, 0);
    for (Voice& v : m_voices) v.loop = nullptr;
    if (!loops) return;
    for (const GrainLoop& g : *loops)
        if (!g.marks.empty()) m_rpm[g.load > 0.5f].push_back(g.rpm);
    for (std::vector<float>& r : m_rpm) {
        std::sort(r.begin(), r.end());
        r.erase(std::unique(r.begin(), r.end()), r.end());
    }
}

void GrainEngine::Fire(float rpm, float load, float onGain, float offGain, float firingSpacing, float rpmBlend, float delay)
{
    if (!m_loops) return;
    // The loops' rpm points around the revs, off and on the throttle each (a recording's ramps give them at different
    // rpm), and the revs' place between them.
    size_t at[2] = {};
    float x[2] = {};
    for (int s = 0; s < 2; ++s) {
        const std::vector<float>& R = m_rpm[s];
        if (R.empty()) continue;
        size_t i = 0;
        while (i + 1 < R.size() && R[i + 1] <= rpm) ++i;
        float u = i + 1 < R.size() && rpm > R[i] ? std::clamp((rpm - R[i]) / (R[i + 1] - R[i]), 0.0f, 1.0f) : 0.0f;
        if (rpmBlend <= 0) u = u < 0.5f ? 0.0f : 1.0f;
        else if (rpmBlend < 1) u = std::clamp((u - (0.5f - rpmBlend / 2)) / rpmBlend, 0.0f, 1.0f);
        at[s] = i, x[s] = u;
    }
    for (size_t k = 0; k < m_loops->size(); ++k) {
        const GrainLoop& g = (*m_loops)[k];
        if (g.marks.empty()) continue;
        // Equal-power weights: two recordings' grains add by power, not as one pulse (straight weights lost up to 3 dB
        // halfway between two rpm points, ~1.8 dB on the average - measured against the Rally1 preview).
        const int s = g.load > 0.5f;
        const std::vector<float>& R = m_rpm[s];
        const size_t i = at[s];
        const float lo = 1 - x[s], hi = x[s], norm = 1 / std::sqrt(lo * lo + hi * hi);
        float w = g.rpm == R[i] ? lo * norm : (i + 1 < R.size() && g.rpm == R[i + 1] ? hi * norm : 0.0f);
        const float on = load, off = 1 - load, loadNorm = 1 / std::sqrt(on * on + off * off);
        w *= g.load > 0.5f ? on * loadNorm * onGain : off * loadNorm * offGain;
        if (w < 1e-3f) continue;
        // Half the time on in the recording's order, half a jump to a random grain at least 8 away: grains played in
        // order round a short loop repeat its small irregularities every pass - heard as a noise once a pass (the
        // user, about once a second on a 1.75 s idle); jumping, no sequence comes back.
        m_random = m_random * 1664525u + 1013904223u;
        if (float(m_random >> 8) / float(1 << 24) < jumps && g.marks.size() > 16) {
            m_random = m_random * 1664525u + 1013904223u;
            m_cursor[k] += 8 + (m_random >> 8) % static_cast<uint32_t>(g.marks.size() - 16);
        }
        const size_t pick = m_cursor[k]++ % g.marks.size();
        const float mark = static_cast<float>(g.marks[pick]);
        w *= g.gains.empty() ? 1.0f : g.gains[pick];
        // The grain's span is a share of the firings' spacing when they come closer than the loop's own (revs above
        // its rpm): the quiet between the recording's pulses shrinks with the period, as a faster engine's does -
        // a span of the loop's period would lay its valleys over the next grain's (heard as gaps with sharp-pulsed
        // recordings). Further apart, the loop's period, its tail read slower (below).
        const float spacing = firingSpacing * g.sample->rate / kRate, unit = std::min(g.period, spacing);
        Voice v;
        v.loop = &g;
        v.attack = v.fade = 2 * 0.1f * unit;
        v.start = v.position = mark - (kAttack + 0.1f) * unit;
        v.end = mark + (kTail + 0.1f) * unit;
        v.mark = mark;
        // After the mark the grain should last until the next firing's grain has faded in.
        const float want = spacing - (kAttack + 0.1f) * unit + 0.2f * unit;
        v.stretch = std::clamp((kTail + 0.1f) * unit / std::max(want, 1.0f), kMinStretch, 1.0f);
        v.gain = w;
        v.wait = static_cast<uint32_t>(std::max(0.0f, delay));
        Voice* slot = &m_voices[0];   // a free one, else the one furthest through
        for (Voice& o : m_voices) {
            if (!o.loop) {
                slot = &o;
                break;
            }
            if ((o.position - o.start) / (o.end - o.start) > (slot->position - slot->start) / (slot->end - slot->start)) slot = &o;
        }
        *slot = v;
    }
}

float GrainEngine::Next()
{
    float sum = 0;
    for (Voice& v : m_voices) {
        if (!v.loop) continue;
        if (v.wait) {
            --v.wait;
            continue;
        }
        const std::vector<float>& d = v.loop->sample->data;
        const double t = v.position - v.start, left = v.end - v.position;
        // The ends faded (sine-squared halves: two grains crossfading sum to one).
        float w = 1;
        if (t < v.attack) w = std::sin(0.5f * kPi * float(t / v.attack)), w *= w;
        else if (left < v.fade) w = std::sin(0.5f * kPi * float(left / v.fade)), w *= w;
        const size_t i = static_cast<size_t>(v.position);
        const float f = static_cast<float>(v.position - double(i));
        sum += (d[i] + (d[i + 1] - d[i]) * f) * w * v.gain;
        v.position += v.loop->sample->rate / kRate * (v.position > v.mark ? v.stretch : 1.0f);
        if (v.position + 1 >= v.end) v.loop = nullptr;
    }
    return sum;
}

} // namespace audio
