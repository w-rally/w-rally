#include "astronomy.h"

#include <cmath>

namespace ngg {
namespace core {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRad = kPi / 180.0;

double Wrap360(double a)
{
    a = std::fmod(a, 360.0);
    return a < 0.0 ? a + 360.0 : a;
}

double Centuries(double jd)
{
    return (jd - 2451545.0) / 36525.0;
}

// The ecliptic's obliquity, mean (Meeus 22.2) plus the main nutation term (25.8's correction).
double Obliquity(double T)
{
    const double eps0 = 23.0 + 26.0 / 60.0 + (21.448 - 46.8150 * T - 0.00059 * T * T + 0.001813 * T * T * T) / 3600.0;
    const double omega = 125.04 - 1934.136 * T;
    return eps0 + 0.00256 * std::cos(omega * kRad);
}

void EclipticToEquatorial(double lonDeg, double latDeg, double epsDeg, double& raDeg, double& decDeg)
{
    const double l = lonDeg * kRad, b = latDeg * kRad, e = epsDeg * kRad;
    raDeg = Wrap360(std::atan2(std::sin(l) * std::cos(e) - std::tan(b) * std::sin(e), std::cos(l)) / kRad);
    decDeg = std::asin(std::sin(b) * std::cos(e) + std::cos(b) * std::sin(e) * std::sin(l)) / kRad;
}

// Equatorial to the local horizon: elevation, and azimuth from north through east.
void Horizon(double raDeg, double decDeg, double lstDeg, double latDeg, double& elevDeg, double& azDeg)
{
    const double H = (lstDeg - raDeg) * kRad, d = decDeg * kRad, p = latDeg * kRad;
    elevDeg = std::asin(std::sin(p) * std::sin(d) + std::cos(p) * std::cos(d) * std::cos(H)) / kRad;
    azDeg = Wrap360(std::atan2(-std::cos(d) * std::sin(H), std::sin(d) * std::cos(p) - std::cos(d) * std::cos(H) * std::sin(p)) / kRad);
}

// Elevation and azimuth to a world direction (x east, y up, z north, north turned by northDeg).
void ToWorld(double elevDeg, double azDeg, double northDeg, float out[3])
{
    const double h = elevDeg * kRad, A = azDeg * kRad, n = northDeg * kRad;
    const double east = std::cos(h) * std::sin(A), north = std::cos(h) * std::cos(A), up = std::sin(h);
    // North is (sin n, 0, cos n) in the world, east (cos n, 0, -sin n).
    out[0] = static_cast<float>(east * std::cos(n) + north * std::sin(n));
    out[1] = static_cast<float>(up);
    out[2] = static_cast<float>(-east * std::sin(n) + north * std::cos(n));
}

// The moon's fundamental arguments, degrees (Meeus 47.1-47.5, 47.6).
struct MoonArgs
{
    double Lp, D, M, Mp, F, E;
};

MoonArgs Arguments(double T)
{
    MoonArgs a;
    const double T2 = T * T, T3 = T2 * T, T4 = T3 * T;
    a.Lp = Wrap360(218.3164477 + 481267.88123421 * T - 0.0015786 * T2 + T3 / 538841.0 - T4 / 65194000.0);
    a.D = Wrap360(297.8501921 + 445267.1114034 * T - 0.0018819 * T2 + T3 / 545868.0 - T4 / 113065000.0);
    a.M = Wrap360(357.5291092 + 35999.0502909 * T - 0.0001536 * T2 + T3 / 24490000.0);
    a.Mp = Wrap360(134.9633964 + 477198.8675055 * T + 0.0087414 * T2 + T3 / 69699.0 - T4 / 14712000.0);
    a.F = Wrap360(93.2720950 + 483202.0175233 * T - 0.0036539 * T2 - T3 / 3526000.0 + T4 / 863310000.0);
    a.E = 1.0 - 0.002516 * T - 0.0000074 * T2;
    return a;
}

// Meeus table 47.A (longitude in 1e-6 degrees, distance in 1e-3 km) and 47.B (latitude in 1e-6 degrees): the terms
// above ~0.004 degrees, which is all a sky can show.
struct Term
{
    signed char D, M, Mp, F;
    double coeff;
};

constexpr Term kLon[] = {
    {0, 0, 1, 0, 6288774}, {2, 0, -1, 0, 1274027}, {2, 0, 0, 0, 658314}, {0, 0, 2, 0, 213618},
    {0, 1, 0, 0, -185116}, {0, 0, 0, 2, -114332}, {2, 0, -2, 0, 58793}, {2, -1, -1, 0, 57066},
    {2, 0, 1, 0, 53322}, {2, -1, 0, 0, 45758}, {0, 1, -1, 0, -40923}, {1, 0, 0, 0, -34720},
    {0, 1, 1, 0, -30383}, {2, 0, 0, -2, 15327}, {0, 0, 1, 2, -12528}, {0, 0, 1, -2, 10980},
    {4, 0, -1, 0, 10675}, {0, 0, 3, 0, 10034}, {4, 0, -2, 0, 8548}, {2, 1, -1, 0, -7888},
    {2, 1, 0, 0, -6766}, {1, 0, -1, 0, -5163}, {1, 1, 0, 0, 4987}, {2, -1, 1, 0, 4036},
    {2, 0, 2, 0, 3994}, {4, 0, 0, 0, 3861}, {2, 0, -3, 0, 3665},
};
constexpr Term kDist[] = {
    {0, 0, 1, 0, -20905355}, {2, 0, -1, 0, -3699111}, {2, 0, 0, 0, -2955968}, {0, 0, 2, 0, -569925},
    {0, 1, 0, 0, 48888}, {0, 0, 0, 2, -3149}, {2, 0, -2, 0, 246158}, {2, -1, -1, 0, -152138},
    {2, 0, 1, 0, -170733}, {2, -1, 0, 0, -204586}, {0, 1, -1, 0, -129620}, {1, 0, 0, 0, 108743},
    {0, 1, 1, 0, 104755}, {2, 0, 0, -2, 10321}, {0, 0, 1, -2, 79661}, {4, 0, -1, 0, -34782},
    {0, 0, 3, 0, -23210}, {4, 0, -2, 0, -21636},
};
constexpr Term kLat[] = {
    {0, 0, 0, 1, 5128122}, {0, 0, 1, 1, 280602}, {0, 0, 1, -1, 277693}, {2, 0, 0, -1, 173237},
    {2, 0, -1, 1, 55413}, {2, 0, -1, -1, 46271}, {2, 0, 0, 1, 32573}, {0, 0, 2, 1, 17198},
    {2, 0, 1, -1, 9266}, {0, 0, 2, -1, 8822}, {2, -1, 0, -1, 8216}, {2, 0, -2, -1, 4324},
    {2, 0, 1, 1, 4200}, {2, 1, 0, -1, -3359},
};

// A term's argument, and its eccentricity factor (E for |M| = 1, E^2 for |M| = 2).
double Arg(const Term& t, const MoonArgs& a, double& eFactor)
{
    eFactor = t.M == 0 ? 1.0 : (t.M == 1 || t.M == -1) ? a.E : a.E * a.E;
    return (t.D * a.D + t.M * a.M + t.Mp * a.Mp + t.F * a.F) * kRad;
}

} // namespace

double JulianDay(int year, int month, double day)
{
    // Meeus 7.1, Gregorian.
    if (month <= 2) {
        year -= 1;
        month += 12;
    }
    const int A = year / 100;
    const int B = 2 - A + A / 4;
    return std::floor(365.25 * (year + 4716)) + std::floor(30.6001 * (month + 1)) + day + B - 1524.5;
}

double GreenwichSiderealDeg(double jd)
{
    const double T = Centuries(jd);
    return Wrap360(280.46061837 + 360.98564736629 * (jd - 2451545.0) + 0.000387933 * T * T - T * T * T / 38710000.0);
}

void SunEquatorial(double jd, double& raDeg, double& decDeg)
{
    // Meeus ch. 25, low accuracy: apparent position.
    const double T = Centuries(jd);
    const double L0 = Wrap360(280.46646 + 36000.76983 * T + 0.0003032 * T * T);
    const double M = Wrap360(357.52911 + 35999.05029 * T - 0.0001537 * T * T);
    const double C = (1.914602 - 0.004817 * T - 0.000014 * T * T) * std::sin(M * kRad) +
                     (0.019993 - 0.000101 * T) * std::sin(2.0 * M * kRad) + 0.000289 * std::sin(3.0 * M * kRad);
    const double omega = 125.04 - 1934.136 * T;
    const double lambda = L0 + C - 0.00569 - 0.00478 * std::sin(omega * kRad);
    EclipticToEquatorial(lambda, 0.0, Obliquity(T), raDeg, decDeg);
}

void MoonEcliptic(double jd, double& lonDeg, double& latDeg, double& distanceKm)
{
    const double T = Centuries(jd);
    const MoonArgs a = Arguments(T);
    double sl = 0.0, sr = 0.0, sb = 0.0;
    for (const Term& t : kLon) {
        double f;
        const double x = Arg(t, a, f);
        sl += t.coeff * f * std::sin(x);
    }
    for (const Term& t : kDist) {
        double f;
        const double x = Arg(t, a, f);
        sr += t.coeff * f * std::cos(x);
    }
    for (const Term& t : kLat) {
        double f;
        const double x = Arg(t, a, f);
        sb += t.coeff * f * std::sin(x);
    }
    // The additive terms (Venus, Jupiter, the earth's flattening).
    const double A1 = Wrap360(119.75 + 131.849 * T), A2 = Wrap360(53.09 + 479264.290 * T), A3 = Wrap360(313.45 + 481266.484 * T);
    sl += 3958.0 * std::sin(A1 * kRad) + 1962.0 * std::sin((a.Lp - a.F) * kRad) + 318.0 * std::sin(A2 * kRad);
    sb += -2235.0 * std::sin(a.Lp * kRad) + 382.0 * std::sin(A3 * kRad) + 175.0 * std::sin((A1 - a.F) * kRad) +
          175.0 * std::sin((A1 + a.F) * kRad) + 127.0 * std::sin((a.Lp - a.Mp) * kRad) - 115.0 * std::sin((a.Lp + a.Mp) * kRad);
    lonDeg = Wrap360(a.Lp + sl / 1e6);
    latDeg = sb / 1e6;
    distanceKm = 385000.56 + sr / 1000.0;
}

double MoonIlluminatedFraction(double jd)
{
    // Meeus 48.4 (the phase angle from the mean elements) and 48.1.
    const MoonArgs a = Arguments(Centuries(jd));
    const double i = 180.0 - a.D - 6.289 * std::sin(a.Mp * kRad) + 2.100 * std::sin(a.M * kRad) -
                     1.274 * std::sin((2.0 * a.D - a.Mp) * kRad) - 0.658 * std::sin(2.0 * a.D * kRad) -
                     0.214 * std::sin(2.0 * a.Mp * kRad) - 0.110 * std::sin(a.D * kRad);
    return (1.0 + std::cos(i * kRad)) / 2.0;
}

SkyBodies ComputeSkyBodies(const SkyTime& t)
{
    SkyBodies b{};
    const double jd = JulianDay(t.year, t.month, t.day + (t.localHour - t.utcOffset) / 24.0);
    const double lst = Wrap360(GreenwichSiderealDeg(jd) + t.longitude);
    b.localSiderealDeg = lst;

    double ra, dec, elev, az;
    SunEquatorial(jd, ra, dec);
    Horizon(ra, dec, lst, t.latitude, elev, az);
    b.sunElevationDeg = static_cast<float>(elev);
    b.sunAzimuthDeg = static_cast<float>(az);
    ToWorld(elev, az, t.northDeg, b.sunTowards);

    double lon, lat, dist;
    MoonEcliptic(jd, lon, lat, dist);
    EclipticToEquatorial(lon, lat, Obliquity(Centuries(jd)), ra, dec);
    Horizon(ra, dec, lst, t.latitude, elev, az);
    // Topocentric: seen from the surface, not the earth's centre, the moon stands lower by up to its parallax (~0.95
    // degrees at the horizon).
    const double parallax = std::asin(6378.14 / dist) / kRad;
    elev -= parallax * std::cos(elev * kRad);
    b.moonElevationDeg = static_cast<float>(elev);
    b.moonAzimuthDeg = static_cast<float>(az);
    ToWorld(elev, az, t.northDeg, b.moonTowards);
    b.moonIlluminated = static_cast<float>(MoonIlluminatedFraction(jd));
    return b;
}

} // namespace core
} // namespace ngg
