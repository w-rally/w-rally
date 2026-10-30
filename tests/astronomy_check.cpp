// astronomy-check: the renderer's sky clockwork (modules/renderer/src/core/astronomy.*) against Jean Meeus's worked
// examples in "Astronomical Algorithms", and against geometry no algorithm can argue with: the noon sun at the summer
// solstice stands 90 - latitude + 23.44 degrees high, due south.
//
// Exit code 0 when every value is within its tolerance.

#include <cmath>
#include <cstdio>

#include "astronomy.h"

namespace {

int g_failed = 0;

void Check(const char* what, double got, double want, double tol)
{
    const bool ok = std::fabs(got - want) <= tol;
    std::printf("%s  %-46s %14.6f  want %14.6f  (tol %g)\n", ok ? "PASS" : "FAIL", what, got, want, tol);
    g_failed += !ok;
}

} // namespace

int main()
{
    using namespace ngg::core;

    // Meeus 7.a and 7.b; 2000 January 1.5 is J2000.0 by definition.
    Check("JD 1957 Oct 4.81 (7.a)", JulianDay(1957, 10, 4.81), 2436116.31, 1e-6);
    Check("JD 2000 Jan 1.5", JulianDay(2000, 1, 1.5), 2451545.0, 1e-9);
    // Meeus 12.a: 1987 April 10, 0h UT: 13h10m46.3668s.
    Check("GMST 1987 Apr 10 0h (12.a), deg", GreenwichSiderealDeg(JulianDay(1987, 4, 10.0)), 197.693195, 1e-5);

    // Meeus 25.a: the sun on 1992 October 13.0 TD, apparent: 13h13m31.4s, -7°47'06".
    double ra, dec;
    SunEquatorial(JulianDay(1992, 10, 13.0), ra, dec);
    Check("sun RA 1992 Oct 13.0 (25.a), deg", ra, 198.38083, 0.01);
    Check("sun dec 1992 Oct 13.0 (25.a), deg", dec, -7.78507, 0.01);

    // Meeus 47.a: the moon on 1992 April 12.0 TD (geometric longitude, before nutation).
    double lon, lat, dist;
    MoonEcliptic(JulianDay(1992, 4, 12.0), lon, lat, dist);
    Check("moon longitude 1992 Apr 12.0 (47.a), deg", lon, 133.162655, 0.05);
    Check("moon latitude 1992 Apr 12.0 (47.a), deg", lat, -3.229126, 0.02);
    Check("moon distance 1992 Apr 12.0 (47.a), km", dist, 368409.7, 50.0);
    // Meeus 48.a: its illuminated fraction then.
    Check("moon lit fraction 1992 Apr 12.0 (48.a)", MoonIlluminatedFraction(JulianDay(1992, 4, 12.0)), 0.6786, 0.01);

    // The summer solstice of 2026 at 50 N: the day's highest sun, found by sampling the day minute by minute.
    SkyTime t;
    t.year = 2026, t.month = 6, t.day = 21, t.latitude = 50.0, t.longitude = 14.4, t.utcOffset = 2.0;
    float best = -90.0f, bestAz = 0.0f, bestHour = 0.0f, towards[3] = {};
    for (int m = 0; m < 24 * 60; ++m) {
        t.localHour = m / 60.0;
        const SkyBodies b = ComputeSkyBodies(t);
        if (b.sunElevationDeg > best) {
            best = b.sunElevationDeg, bestAz = b.sunAzimuthDeg, bestHour = static_cast<float>(t.localHour);
            for (int k = 0; k < 3; ++k) towards[k] = b.sunTowards[k];
        }
    }
    Check("solstice noon sun at 50 N, elevation deg", best, 90.0 - 50.0 + 23.44, 0.1);
    Check("  ... its azimuth, deg (due south)", bestAz, 180.0, 0.5);
    // Solar noon at 14.4 E on CEST: 12:00 + 2 h - 14.4/15 h + the equation of time (+1.7 min on June 21) ~ 13:04.
    Check("  ... local time, hours", bestHour, 13.07, 0.05);
    // World axes: x east, y up, z north - due south is -z.
    Check("  ... towards, world x (east)", towards[0], 0.0, 0.01);
    Check("  ... towards, world z (north)", towards[2], -std::cos(best * 3.14159265358979 / 180.0), 0.01);

    // Solar midnight at the same place: the sun's lowest, latitude + declination - 90 (it never gets darker than that
    // at 50 N in June: astronomical twilight all night).
    t.localHour = 1.07;
    const SkyBodies night = ComputeSkyBodies(t);
    Check("solstice midnight sun at 50 N, elevation deg", night.sunElevationDeg, 50.0 + 23.44 - 90.0, 0.2);
    Check("  ... its azimuth, deg (due north)", std::fmod(night.sunAzimuthDeg + 180.0, 360.0), 180.0, 0.5);

    std::printf("%s\n", g_failed ? "astronomy-check: FAILED" : "astronomy-check: all good");
    return g_failed ? 1 : 0;
}
