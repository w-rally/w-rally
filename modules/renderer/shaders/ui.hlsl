// The game's UI pass (module/ui.cpp): textured, coloured quads.

cbuffer Screen : register(b0) { float2 scale; float2 offset; };   // pixels -> clip: p * scale + offset
Texture2D tex : register(t0);
SamplerState samp : register(s0);
struct VsIn { float2 pos : POSITION; float2 uv : TEXCOORD; float4 colour : COLOR; };
struct PsIn { float4 pos : SV_Position; float2 uv : TEXCOORD; float4 colour : COLOR; };
PsIn VSMain(VsIn v)
{
    PsIn o;
    o.pos = float4(v.pos * scale + offset, 0, 1);
    o.uv = v.uv;
    o.colour = v.colour;
    return o;
}
float4 PSMain(PsIn i) : SV_Target { return i.colour * tex.Sample(samp, i.uv); }
