#pragma once
// ---------------------------------------------------------------------------
// The sky's clockwork (NGG design 5.4, phase N1): where the sun and the moon stand for a date, a time and a place,
// and how much of the moon is lit. Night in WRally is any stage at any hour, so the sun is no longer a direction
// copied from a preset - it follows from when and where.
//
// The algorithms are Jean Meeus's, "Astronomical Algorithms" (2nd ed.): the sun's low-accuracy method (ch. 25,
// ~0.01 degrees), the moon from the main terms of ch. 47 (~0.02 degrees in longitude), its illuminated fraction
// (ch. 48), sidereal time (ch. 12). A sky needs a tenth of a degree at most - the sun's disc is half a degree wide -
// so dynamical time is taken as universal time (69 s apart in 2026) and refraction is left out.
//
// World axes: x EAST, y UP, z NORTH - before the stage's own `northDeg` turns them: north lies at northDeg from +z
// towards +x (RBR's stages carry no orientation; 0 until a stage says otherwise).
// ---------------------------------------------------------------------------

namespace ngg {
namespace core {

struct SkyTime
{
    int    year = 2026, month = 6, day = 21;
    double localHour = 12.0;      // 0..24, the stage's clock
    double utcOffset = 1.0;       // hours: local = UTC + utcOffset
    double latitude = 50.0;       // degrees, north positive
    double longitude = 14.4;      // degrees, east positive
    double northDeg = 0.0;        // where north lies in the world, degrees from +z towards +x
};

struct SkyBodies
{
    float  sunTowards[3];         // unit, world: from the ground towards the sun
    float  sunElevationDeg, sunAzimuthDeg;     // azimuth from north through east
    float  moonTowards[3];        // unit, world (topocentric: the moon's parallax is near a degree)
    float  moonElevationDeg, moonAzimuthDeg;
    float  moonIlluminated;       // the lit fraction of its disc, 0 new .. 1 full
    double localSiderealDeg;      // the stars' rotation: local sidereal time, degrees
};

SkyBodies ComputeSkyBodies(const SkyTime& t);

// The parts, for checking against Meeus's worked examples.
double JulianDay(int year, int month, double dayWithFraction);
double GreenwichSiderealDeg(double jd);                         // mean, degrees 0..360
void   SunEquatorial(double jd, double& raDeg, double& decDeg);  // apparent, low accuracy
void   MoonEcliptic(double jd, double& lonDeg, double& latDeg, double& distanceKm);
double MoonIlluminatedFraction(double jd);

} // namespace core
} // namespace ngg
