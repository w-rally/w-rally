#pragma once
// The stage's drive line, natively (CSigDriveLine, core/driveline.cpp; the .trk's block (3,20)): the sampled racing line as
// the keys of a Hermite spline, each with its distance from the start. The car's integration projects its position onto
// it (the parameter near the last one) and the stage timing reads the distance there.
#include <cstdint>
#include <vector>

#include "num/math.h"
#include "num/x87.h"
#include "native/records.h"

namespace phys::native {

class DriveLine {
public:
    // The samples (the line's count of them, then one zero sample: the spline's last key reads one past it, as the
    // classic's array beyond its count).
    std::vector<DriveLineSample> samples;
    uint32_t Count() const { return samples.empty() ? 0 : static_cast<uint32_t>(samples.size() - 1); }

    void Load(const DriveLineSample* s, uint32_t n);
    // The distance along the line at parameter t (DriveLineDistance, 0x475ea0); past the end the classic's assertion: 0.
    float Distance(float t) const;
    // The parameter of the point on the line nearest p, searched around t (DriveLineProject, 0x50b300).
    num::X87 Project(const float* p, float t) const;
    // Where a reset puts a car at parameter t (CarResetPoint, 0x4e5530): the curve's point (HermiteKey4 at the index by
    // cvttss2si, the fraction on the x87) and the line's direction there (DrivelineDirection 0x50a920: the two samples'
    // lerped; Normalize3Approx4).
    void ResetPoint(float t, num::Vec4& point, num::Vec4& dir) const;

private:
    void Key(int32_t index, float t, num::Vec4& out) const;   // HermiteKey4: the curve between keys index, index + 1
    void KeyAt(float x, num::Vec4& out) const;                // HermiteKeyAt: at parameter x
    const DriveLineSample& Sample(uint32_t j) const;      // past the count: the assertion, then sample 0
};

} // namespace phys::native
