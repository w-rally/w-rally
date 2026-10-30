// ---------------------------------------------------------------------------
// The atmosphere model, as HLSL source shared by the sky pass and the scene
// pass.
//
// ONE implementation on purpose. The sky behind the world and the aerial
// perspective on the world are the same scattering integral over different
// segments of the same ray; if they were written twice they would drift, and
// the seam between them - the horizon - is exactly where a disagreement shows.
// This project has already paid for a duplicated model once (Wallaby's
// fixed-function ground blend vs RBR's actual shader, design 8.1).
//
// Included as a string and prepended to both shaders rather than compiled
// separately, because D3DCompile has no include handler here and the cost of
// wiring one up exceeds the cost of a concatenation.
// ---------------------------------------------------------------------------
#pragma once

namespace ngg {
namespace core {

const char* const kAtmosphereHlsl = R"(

static const float kPI = 3.14159265;

// Linear -> sRGB. Applied in the SHADER rather than by an _SRGB render target
// format, because the game path renders into a texture the 32-bit bridge
// created and we do not control its format from here. One encode, both paths.
//
// Its absence was the reason shaded surfaces stayed stubbornly dark through
// several rounds of ambient tuning: linear 0.144 written raw into an 8-bit
// target displays as 0.144, where the correct sRGB encoding of that value is
// 0.42. Highlights barely move, shadows are crushed - which is exactly what
// "the lit side looks fine but the shadow is black" means.
float3 LinearToSrgb(float3 c)
{
    c = saturate(c);
    return (c < 0.0031308) ? c * 12.92
                           : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

// Earth-scale constants. The model is only meaningful with a real planet
// radius, which is why the camera sits on a 6360 km sphere rather than at the
// world origin - RBR's own coordinates are metres near zero.
static const float  kRg = 6360000.0;    // ground radius
static const float  kRa = 6420000.0;    // top of atmosphere
static const float  kHr = 8000.0;       // Rayleigh scale height
static const float  kHm = 1200.0;       // Mie scale height
static const float3 kBetaR = float3(5.8e-6, 13.5e-6, 33.1e-6);
static const float  kBetaM = 21e-6;
static const float  kMieG = 0.76;

// Multiple scattering, tabulated (atmosphere_ms.h, design 5.0j): Psi_ms by
// (mu_s, altitude). Register SPACE 1 so it can never collide with a consumer's
// own t0/s0 - every root signature that compiles AtmScatter binds it there.
// An all-zero table is exactly the single-scattering model.
Texture2D    gAtmMs     : register(t0, space1);
SamplerState gAtmMsSamp : register(s0, space1);

float3 AtmMultipleScattering(float altitude, float muS)
{
    const float2 uv = float2(muS * 0.5 + 0.5, saturate(altitude / (kRa - kRg)));
    return gAtmMs.SampleLevel(gAtmMsSamp, uv, 0.0).rgb;
}

// Where the camera is placed on that sphere. 500 m up: high enough that a
// stage's own altitude range does not change the sky, low enough to still read
// as ground level.
static const float kCamHeight = 500.0;

bool AtmRaySphere(float3 o, float3 d, float r, out float t0, out float t1)
{
    float b = dot(o, d);
    float c = dot(o, o) - r * r;
    float disc = b * b - c;
    t0 = 0.0; t1 = 0.0;
    if (disc < 0.0) return false;
    disc = sqrt(disc);
    t0 = -b - disc;
    t1 = -b + disc;
    return true;
}

// ---- WEATHER (design 5.0q) -------------------------------------------------
// Set once at the top of each shader from its constant buffer, so no
// atmosphere function signature had to change. Defaults are the clear,
// fog-free day the model was before these existed.
static float gAtmCloud   = 0.0;     // 0 clear .. 1 overcast
static float gAtmFogBeta = 0.0;     // ground-level fog extinction, 1/m; 0 = none
static float gAtmFogH    = 150.0;   // fog layer scale height, m
static float gAtmGround  = 0.1;     // ground albedo, for light bounced into fog

// The night's own sky (design 5.4, N1b): airglow and starlight, a radiance that is there whatever the sun and the
// moon do - what lights a moonless night. 0 by day (it is ~1e-9 of it). Brighter towards the horizon (more
// glowing layer along the line of sight: the van Rhijn effect), dimmed by cloud.
static float gAtmGlow = 0.0;
void AtmSetNight(float glow) { gAtmGlow = glow; }

// A second body lighting the sky (design 5.4): the moon's sky at dusk, the sun's twilight at night - added to the
// light's own in AtmSkyRadiance, so the sky passes from one body to the other without a jump. 0: none.
static float3 gAtmLight2Dir = float3(0.0, -1.0, 0.0);   // towards it
static float  gAtmLight2I   = 0.0;
void AtmSetLight2(float3 towards, float intensity) { gAtmLight2Dir = towards; gAtmLight2I = intensity; }

void AtmSetWeather(float cloud, float fogBeta, float fogHeight, float groundAlbedo)
{
    gAtmCloud = saturate(cloud);
    gAtmFogBeta = max(fogBeta, 0.0);
    gAtmFogH = max(fogHeight, 10.0);
    gAtmGround = saturate(groundAlbedo);
}

// The fraction of DIRECT sunlight the cloud deck lets through: nothing changes
// below ~30 % cover (gaps), almost nothing gets through at full overcast.
float AtmCloudSun() { return 1.0 - 0.98 * smoothstep(0.3, 1.0, gAtmCloud); }
// How far the sky dome has turned into the overcast one.
float AtmCloudSky() { return pow(saturate(gAtmCloud), 1.5); }

// The fog is an exponential HEIGHT layer on the camera's own ground - the eye
// is 2 m up in it - not on the model's sea level, where the camera floats
// kCamHeight above for the sky's sake. Its optical depth along a ray has a
// closed form, which is the point: the view march's steps are kilometres long
// and would step straight over a layer 150 m thick.
float AtmFogRho0() { return exp(-2.0 / gAtmFogH); }
float AtmFogOD(float3 ray, float d)
{
    if (gAtmFogBeta <= 0.0) return 0.0;
    const float mu = ray.y;
    if (mu < 1e-4) return gAtmFogBeta * AtmFogRho0() * d;           // level or downward: the dense part
    return gAtmFogBeta * AtmFogRho0() * gAtmFogH / mu * (1.0 - exp(-d * mu / gAtmFogH));
}
// Sunlight through the whole fog layer above, to a surface in it.
float AtmFogSunT(float3 sunDir)
{
    if (gAtmFogBeta <= 0.0) return 1.0;
    return exp(-gAtmFogBeta * AtmFogRho0() * gAtmFogH / max(sunDir.y, 0.02));
}

// How much sunlight survives the trip down to the camera's altitude.
//
// This is what gives a low sun its colour for free: the Rayleigh coefficient is
// ~6x larger in blue than in red, so a long slant path through the atmosphere
// removes blue first. Sunset reddening is then a consequence of the model
// rather than an artist-authored gradient - which is the entire reason for
// having a physical sky at all.
//
// A single march with no inner loop, so it is far cheaper than AtmScatter and
// affordable per pixel while an environment LUT does not exist yet.
float3 AtmSunTransmittanceAir(float3 sunDir, int steps)
{
    float3 origin = float3(0.0, kRg + kCamHeight, 0.0);

    float t0, t1;
    if (!AtmRaySphere(origin, sunDir, kRa, t0, t1)) return float3(1, 1, 1);

    // Sun below the horizon: the planet itself is in the way, so nothing direct
    // arrives. Without this the ground stays lit through the earth at night.
    float g0, g1;
    if (AtmRaySphere(origin, sunDir, kRg, g0, g1) && g0 > 0.0)
        return float3(0, 0, 0);

    float seg = max(t1, 0.0) / steps;
    float odR = 0.0, odM = 0.0;

    [loop] for (int i = 0; i < steps; ++i)
    {
        float3 p = origin + sunDir * (seg * (float(i) + 0.5));
        float h = max(length(p) - kRg, 0.0);
        odR += exp(-h / kHr) * seg;
        odM += exp(-h / kHm) * seg;
    }
    return exp(-(kBetaR * odR + kBetaM * 1.1 * odM));
}

// The direct sun at a surface: clear air, then the cloud deck, then the fog.
float3 AtmSunTransmittance(float3 sunDir, int steps)
{
    return AtmSunTransmittanceAir(sunDir, steps) * (AtmCloudSun() * AtmFogSunT(sunDir));
}

// The weather's diffuse light, as horizontal irradiances. Clear-sky diffuse is
// a fit to THIS model's own hemisphere integral with multiple scattering
// (0.064 * I * sin(elev)^0.25, within ~5 % from 10 to 50 degrees - 5.0j's
// numbers); an overcast deck passes ~30 % of the clear global irradiance as
// diffuse light (a standard figure for a thick stratus deck).
struct AtmDiffuse { float3 direct; float clearSky; float overcast; };
AtmDiffuse AtmWeatherLight(float3 sunDir, float sunIntensity)
{
    AtmDiffuse d;
    const float y = max(sunDir.y, 0.0);
    d.direct   = sunIntensity * AtmSunTransmittanceAir(sunDir, 6) * y;
    d.clearSky = 0.064 * sunIntensity * pow(y, 0.25);
    d.overcast = 0.30 * (dot(d.direct, float3(0.2126, 0.7152, 0.0722)) + d.clearSky);
    return d;
}
// CIE overcast sky: zenith three times the horizon, horizontal irradiance
// (7 PI / 9) Lz. Neutral, a touch cool.
float3 AtmOvercastRadiance(float3 ray, float overcastE)
{
    const float lz = 9.0 * overcastE / (7.0 * kPI);
    return lz * (1.0 + 2.0 * saturate(ray.y)) / 3.0 * float3(0.96, 1.0, 1.05);
}

// In-scattered light along `ray`, from the camera out to `maxDist` metres
// (use a huge value for the sky itself). `transmittance` returns how much of
// whatever lies beyond survives the trip, which is what turns this from fog
// into aerial perspective: distant geometry is both dimmed AND tinted, and
// getting only the tint is what makes cheap fog look like paint.
float3 AtmScatter(float3 ray, float3 sunDir, float maxDist, float turbidity,
                  float sunIntensity, int viewSteps, int lightSteps,
                  out float3 transmittance)
{
    transmittance = float3(1.0, 1.0, 1.0);

    float3 betaM = kBetaM * max(turbidity, 0.0);
    float3 origin = float3(0.0, kRg + kCamHeight, 0.0);

    float t0, t1;
    if (!AtmRaySphere(origin, ray, kRa, t0, t1)) return float3(0, 0, 0);
    t0 = max(t0, 0.0);

    // Stop at the planet, otherwise looking down integrates through the earth
    // and produces a bright band under the horizon.
    float g0, g1;
    if (AtmRaySphere(origin, ray, kRg, g0, g1) && g0 > 0.0)
        t1 = min(t1, g0);

    t1 = min(t1, maxDist);
    if (t1 <= t0) return float3(0, 0, 0);

    float seg = (t1 - t0) / viewSteps;
    float t = t0;

    float3 sumR = float3(0, 0, 0);
    float3 sumM = float3(0, 0, 0);
    float3 sumMs = float3(0, 0, 0);
    float3 sumIso = float3(0, 0, 0);   // path scattering, for light from an overcast deck
    float odR = 0.0, odM = 0.0;

    float mu = dot(ray, sunDir);
    float phaseR = 3.0 / (16.0 * kPI) * (1.0 + mu * mu);
    float gg = kMieG * kMieG;
    float phaseM = 3.0 / (8.0 * kPI) * ((1.0 - gg) * (1.0 + mu * mu)) /
                   ((2.0 + gg) * pow(max(1.0 + gg - 2.0 * kMieG * mu, 1e-4), 1.5));

    [loop] for (int i = 0; i < viewSteps; ++i)
    {
        float3 p = origin + ray * (t + seg * 0.5);
        float h = length(p) - kRg;
        float hr = exp(-h / kHr) * seg;
        float hm = exp(-h / kHm) * seg;
        odR += hr;
        odM += hm;

        // Every order beyond the first, as an isotropic source of strength
        // sigma_s * Psi_ms (Hillaire 2020, 5.5), attenuated back to the eye.
        // Not gated on `lit`: a point in the planet's shadow still receives
        // light scattered from the lit atmosphere around it - that is most of
        // what twilight is - and Psi_ms already falls off with mu_s.
        const float3 viewT = exp(-(kBetaR * odR + betaM * 1.1 * odM));
        sumMs += viewT * (kBetaR * hr + betaM * hm)
               * AtmMultipleScattering(max(h, 0.0), dot(p, sunDir) / length(p));
        sumIso += viewT * (kBetaR * hr + betaM * hm);

        float l0, l1;
        AtmRaySphere(p, sunDir, kRa, l0, l1);
        float lseg = l1 / lightSteps;
        float odLR = 0.0, odLM = 0.0;
        bool lit = true;

        [loop] for (int j = 0; j < lightSteps; ++j)
        {
            float3 lp = p + sunDir * (lseg * (float(j) + 0.5));
            float lh = length(lp) - kRg;
            if (lh < 0.0) { lit = false; break; }     // planet in the way
            odLR += exp(-lh / kHr) * lseg;
            odLM += exp(-lh / kHm) * lseg;
        }

        if (lit)
        {
            float3 tau = kBetaR * (odR + odLR) + betaM * 1.1 * (odM + odLM);
            float3 att = exp(-tau);
            sumR += att * hr;
            sumM += att * hm;
        }
        t += seg;
    }

    transmittance = exp(-(kBetaR * odR + betaM * 1.1 * odM));
    float3 result = sunIntensity * (sumR * kBetaR * phaseR + sumM * betaM * phaseM + sumMs);

    // ---- weather. Clear in, so a clear fog-free day returns above untouched.
    if (gAtmCloud <= 0.0 && gAtmFogBeta <= 0.0) return result;

    const AtmDiffuse L = AtmWeatherLight(sunDir, sunIntensity);
    const float sky = AtmCloudSky();

    // Clouds: the sun-lit scattering dims with the direct sun; the air is lit
    // instead by the deck's diffuse light; and a SKY ray ends on the deck.
    result = result * AtmCloudSun() + sumIso * (sky * L.overcast / kPI);
    if (maxDist >= 1e8)
        result = lerp(result, AtmOvercastRadiance(ray, L.overcast), sky);

    // Fog, analytically over everything behind it - it sits at the camera, in
    // front of the air's own scattering. Its droplets are lit by the sun that
    // reaches into the layer (forward-peaked, the Mie phase) and by a diffuse
    // field: half sky, half ground bounce.
    if (gAtmFogBeta > 0.0)
    {
        const float  od = AtmFogOD(ray, maxDist);
        const float  tf = exp(-od);
        const float  ediff = lerp(L.clearSky, L.overcast, sky);
        const float3 edir  = L.direct * AtmCloudSun() * AtmFogSunT(sunDir);
        const float3 amb   = 0.5 * (ediff / kPI + gAtmGround * (edir + ediff) / kPI);
        const float3 src   = phaseM * sunIntensity * AtmSunTransmittanceAir(sunDir, 6)
                           * AtmCloudSun() * sqrt(AtmFogSunT(sunDir)) + amb;
        result = result * tf + src * (1.0 - tf);
        transmittance *= tf;
    }
    return result;
}

// The sky along `ray`, INCLUDING the below-horizon ground blend and EXCLUDING
// the sun disc.
//
// Shared by the sky pass and the environment map for the reason at the top of
// this file: they meet at the horizon, which is exactly where two copies would
// disagree. Below the horizon there is no real surface to shade, so the ground
// is the sky mirrored and dimmed by its albedo - somewhere for ambient to come
// from where terrain does not cover the frame.
//
// THE SUN DISC IS DELIBERATELY NOT HERE. The environment map feeds a specular
// term that already has its own direct sun lobe, and a disc in both would count
// the sun twice - as a hard bright spot in every reflection.
float3 AtmSkyRadianceOf(float3 ray, float3 sunDir, float turbidity,
                        float sunIntensity, float groundAlbedo,
                        int viewSteps, int lightSteps)
{
    float3 tr;
    float3 col = AtmScatter(ray, sunDir, 1e9, turbidity, sunIntensity,
                            viewSteps, lightSteps, tr);

    if (ray.y < 0.0)
    {
        float3 utr;
        float3 up = AtmScatter(float3(ray.x, abs(ray.y), ray.z), sunDir, 1e9,
                               turbidity, sunIntensity, viewSteps, lightSteps, utr);
        col = lerp(col, up * groundAlbedo, saturate(-ray.y * 6.0));
    }
    return col;
}

float3 AtmSkyRadiance(float3 ray, float3 sunDir, float turbidity,
                      float sunIntensity, float groundAlbedo,
                      int viewSteps, int lightSteps)
{
    float3 col = AtmSkyRadianceOf(ray, sunDir, turbidity, sunIntensity, groundAlbedo, viewSteps, lightSteps);
    if (gAtmLight2I > 0.0)
        col += AtmSkyRadianceOf(ray, gAtmLight2Dir, turbidity, gAtmLight2I, groundAlbedo, viewSteps, lightSteps);
    if (gAtmGlow > 0.0)
    {
        const float3 glow = gAtmGlow * float3(0.85, 0.95, 1.0) * (1.0 + 1.5 * (1.0 - saturate(abs(ray.y))))
                          * lerp(1.0, 0.3, gAtmCloud);
        col += ray.y >= 0.0 ? glow : lerp(glow, glow * groundAlbedo, saturate(-ray.y * 6.0));
    }
    return col;
}

// Octahedral direction <-> unit square, Y as the pole axis because the runtime
// is Y-up (design 2.2). Used by the environment map: a 2D texture takes an
// ordinary mip chain, where a cube map needs per-face filtering and a seam
// policy to get the same thing.
float2 AtmOctEncode(float3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    float2 o = n.xz;
    if (n.y < 0.0) o = (1.0 - abs(float2(o.y, o.x))) * float2(o.x >= 0.0 ? 1.0 : -1.0,
                                                             o.y >= 0.0 ? 1.0 : -1.0);
    return o * 0.5 + 0.5;
}
float3 AtmOctDecode(float2 f)
{
    f = f * 2.0 - 1.0;
    float3 n = float3(f.x, 1.0 - abs(f.x) - abs(f.y), f.y);
    float t = saturate(-n.y);
    n.x += (n.x >= 0.0) ? -t : t;
    n.z += (n.z >= 0.0) ? -t : t;
    return normalize(n);
}

)";

} // namespace core
} // namespace ngg
