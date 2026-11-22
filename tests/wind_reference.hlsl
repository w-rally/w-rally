// wind-reference (tests/wind_reference.cpp): the renderer's wind (modules/renderer/shaders/wind.hlsli) on each row of the
// golden table, one thread per row.
#include "../modules/renderer/shaders/wind.hlsli"

struct Row
{
    float3 base;
    float3 v;        // pos - base
    float3 nrm;
    float2 attr;
    float  h, height, bend, t;
    float4 wind;     // direction x, z; speed; gustiness
    float  pad;
};
StructuredBuffer<Row>     gRows : register(t0);
RWStructuredBuffer<float4> gOut : register(u0);
// The row count as a root constant: the rows are a root SRV, which carries no size, so GetDimensions on it is undefined
// (0 on NVIDIA, where every thread then returned and the test read back the zeroed output).
cbuffer Count : register(b0) { uint gRowCount; };

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gRowCount) return;
    const Row r = gRows[id.x];
    WindFrame f;
    f.wind = r.wind;
    f.t = r.t;
    gOut[id.x] = float4(WindBend(r.base, r.v, r.nrm, r.attr, r.h, r.height, r.bend, f), WindPhase(r.base));
}
