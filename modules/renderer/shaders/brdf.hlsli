// The scene's BRDF: the clear coat's constants, GGX (Trowbridge-Reitz, height-correlated Smith, Schlick), specular
// anti-aliasing and the G-buffer's roughness packing. Included by scene.hlsl at its place in the source.
// CLEAR COAT (MaterialDesc::clearCoat): car paint's lacquer - a smooth dielectric film over the base layer. Its own
// GGX lobe (f0 0.04, this gloss: a polished finish, 0.02) reflects the light and the environment sharply, and
// what it reflects does not reach the base below: the base's diffuse and specular are scaled by 1 - coat x F. The
// same normal for both layers (RBR's paint has no normal map). The deferred local lights know only coated or not
// (GBUF kind 3).
static const float kCoatRoughness = 0.02;
static const float kCoatF0 = 0.04;

// Standard GGX: Trowbridge-Reitz distribution, height-correlated Smith
// visibility, Schlick Fresnel. Nothing bespoke here on purpose - the value of a
// standard BRDF is that an authored glTF material looks the same in Blender.
float3 FresnelSchlick(float3 f0, float vdh)
{
    return f0 + (1.0 - f0) * pow(saturate(1.0 - vdh), 5.0);
}
float DistributionGGX(float ndh, float a)
{
    float a2 = a * a;
    float d  = ndh * ndh * (a2 - 1.0) + 1.0;
    return a2 / max(1e-6, 3.14159265 * d * d);
}
float VisSmith(float ndv, float ndl, float a)
{
    float a2 = a * a;
    float lv = ndl * sqrt(ndv * ndv * (1.0 - a2) + a2);
    float ll = ndv * sqrt(ndl * ndl * (1.0 - a2) + a2);
    return 0.5 / max(1e-6, lv + ll);
}

// SPECULAR ANTI-ALIASING (Kaplanyan et al. 2016, Tokuyoshi and Kaplanyan 2019/2021, "Stable geometric specular
// antialiasing with projected-space NDF filtering"): where the shading normal turns within a pixel - a curved tube, a
// wheel's rim, a crease, a body panel going edge-on - a lobe narrower than that turn is a highlight smaller than the
// pixel, hit by one sample and missed by the next: a sparkle that jumps as the car or the camera moves - the clear
// coat's lobe (alpha 0.002 at its floor) most of all, worst at night, when the exposure lifts every highlight. In the
// Garage at night it took a quarter of the car's isolated bright pixels off (the turntable turning). The
// pixel's spread of normals, from their screen derivatives, widens the lobe (alpha^2 + kernel): flat panels keep the
// mirror (no spread, no change - the day's reflections stay as sharp), the curved ones get the highlight's true
// footprint. kSpecAAVariance is the pixel filter's variance in screen space, kSpecAAKappa caps the widening (an
// alpha^2: alpha <= 0.42).
static const float kSpecAAVariance = 0.25;
static const float kSpecAAKappa = 0.18;
float SpecAAKernel(float3 N)
{
    const float3 dx = ddx(N), dy = ddy(N);
    return min(2.0 * kSpecAAVariance * (dot(dx, dx) + dot(dy, dy)), kSpecAAKappa);
}
// An alpha (roughness^2) widened by the kernel.
float SpecAAAlpha(float a, float kernel) { return sqrt(a * a + kernel); }
// A coated surface's G-buffer roughness (the R10G10B10A2_UNORM target's 10 bits, GBUF kind 3): the base's widened
// roughness in the upper 5 bits, the kernel in the lower 5 as sqrt(kernel / kappa) - no spread is exactly 0, so a
// flat panel's coat stays the mirror it was. Integers up to 1023 survive UNORM's round trip exactly.
float GbufRoughPack(float rough, float kernel)
{
    const float hi = round(saturate(rough) * 31.0), lo = round(sqrt(saturate(kernel / kSpecAAKappa)) * 31.0);
    return (hi * 32.0 + lo) / 1023.0;
}
void GbufRoughUnpack(float packed, out float rough, out float kernel)
{
    const float v = round(packed * 1023.0), hi = floor(v / 32.0), lo = v - hi * 32.0;
    rough = hi / 31.0;
    kernel = (lo / 31.0) * (lo / 31.0) * kSpecAAKappa;
}
