// The atmosphere's multiple-scattering LUT (core/atmosphere_ms.cpp).
//
// Prepended with kAtmosphereHlsl for the constants and AtmRaySphere. This is a
// DIFFERENT integral from AtmScatter - second-order, isotropic, from an
// arbitrary point - not a second copy of it.
#include "atmosphere.hlsli"

cbuffer MsCB : register(b0)
{
    float gTurbidity;
    float gGroundAlbedo;
    float gEnabled;
    float gPad;
};

RWTexture2D<float4> gOut : register(u0);

// Sun transmittance from an ARBITRARY point (AtmSunTransmittance is fixed at
// the camera). Zero when the planet is in the way.
float3 SunTransmittanceFrom(float3 p, float3 sunDir, float3 betaM)
{
    float g0, g1;
    if (AtmRaySphere(p, sunDir, kRg, g0, g1) && g0 > 0.0) return float3(0, 0, 0);
    float t0, t1;
    AtmRaySphere(p, sunDir, kRa, t0, t1);
    const int steps = 8;
    const float seg = max(t1, 0.0) / steps;
    float odR = 0.0, odM = 0.0;
    [loop] for (int i = 0; i < steps; ++i)
    {
        const float h = max(length(p + sunDir * (seg * (i + 0.5))) - kRg, 0.0);
        odR += exp(-h / kHr) * seg;
        odM += exp(-h / kHm) * seg;
    }
    return exp(-(kBetaR * odR + betaM * 1.1 * odM));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint w, h;
    gOut.GetDimensions(w, h);
    if (any(id.xy >= uint2(w, h))) return;
    if (gEnabled < 0.5) { gOut[id.xy] = float4(0, 0, 0, 1); return; }

    // Texel centres: mu_s across [-1, 1], altitude down [0, top].
    const float muS = ((id.x + 0.5) / w) * 2.0 - 1.0;
    const float alt = ((id.y + 0.5) / h) * (kRa - kRg);
    const float3 x = float3(0.0, kRg + alt, 0.0);
    const float3 sunDir = float3(sqrt(saturate(1.0 - muS * muS)), muS, 0.0);
    const float3 betaM = kBetaM * max(gTurbidity, 0.0);
    const float isoPhase = 1.0 / (4.0 * kPI);

    // 64 directions over the sphere, stratified in cos(theta) and phi - equal
    // solid angle per cell, so the sphere integral with the isotropic pdf
    // 1/(4 PI) is a plain average.
    float3 L2 = 0.0, fms = 0.0;
    const int nT = 8, nP = 8;
    [loop] for (int a = 0; a < nT; ++a)
    {
        const float ct = 1.0 - 2.0 * (a + 0.5) / nT;
        const float st = sqrt(saturate(1.0 - ct * ct));
        [loop] for (int b = 0; b < nP; ++b)
        {
            const float ph = 2.0 * kPI * (b + 0.5) / nP;
            const float3 d = float3(st * cos(ph), ct, st * sin(ph));

            float t0, t1;
            AtmRaySphere(x, d, kRa, t0, t1);
            float tMax = max(t1, 0.0);
            float g0, g1;
            const bool ground = AtmRaySphere(x, d, kRg, g0, g1) && g0 > 0.0;
            if (ground) tMax = g0;

            const int n = 20;
            const float ds = tMax / n;
            float3 tau = 0.0, Lp = 0.0, Fp = 0.0;
            [loop] for (int i = 0; i < n; ++i)
            {
                const float3 p = x + d * (ds * (i + 0.5));
                const float hh = max(length(p) - kRg, 0.0);
                const float dR = exp(-hh / kHr), dM = exp(-hh / kHm);
                const float3 sigS = kBetaR * dR + betaM * dM;
                const float3 sigT = kBetaR * dR + betaM * 1.1 * dM;
                const float3 T = exp(-(tau + sigT * ds * 0.5));   // x -> step midpoint
                Lp += T * sigS * SunTransmittanceFrom(p, sunDir, betaM) * isoPhase * ds;
                Fp += T * sigS * ds;
                tau += sigT * ds;
            }
            if (ground)
            {
                // The ground is a Lambertian reflector lit by the attenuated
                // sun: radiance albedo/PI * E. Part of the second bounce.
                const float3 pg = x + d * tMax;
                const float3 ng = normalize(pg);
                Lp += exp(-tau) * SunTransmittanceFrom(pg + ng * 1.0, sunDir, betaM)
                    * saturate(dot(ng, sunDir)) * gGroundAlbedo / kPI;
            }
            L2 += Lp;
            fms += Fp;
        }
    }
    L2  /= float(nT * nP);
    fms /= float(nT * nP);

    // Every further order is the previous one scattered by the same fraction:
    // L2 * (1 + f + f^2 + ...) = L2 / (1 - f).
    gOut[id.xy] = float4(L2 / (1.0 - min(fms, 0.99)), 1.0);
}
