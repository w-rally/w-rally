// The editor's selection outline, labels and overlay lines (core/scene.cpp, Scene::InitSelection).

cbuffer SelCB : register(b0)
{
    row_major float4x4 gMvp;      // object transform * view * reverse-Z proj
    float4             gColour;
    uint               gPass;     // 0 = passed depth, 1 = occluded
    uint               gScreenSpace;  // labels: anchor.xy is PIXELS, not world
    float2             gTextOffset;   // labels: pixel shift, for the shadow pass
};

RWStructuredBuffer<uint> gCount : register(u0);

float4 VSSel(float3 pos : POSITION) : SV_Position
{
    return mul(float4(pos, 1.0), gMvp);
}

// [earlydepthstencil] IS LOAD-BEARING, not an optimisation.
//
// A pixel shader that writes a UAV has early depth-stencil turned OFF by
// default, because the hardware cannot know the shader will not discard or
// write oDepth. The depth test then runs AFTER the shader - which still
// suppresses the colour write, so the picture is correct either way, but the
// shader has already run and its InterlockedAdd has already landed. The
// counter would be measuring RASTERISED pixels and calling them VISIBLE ones.
//
// Measured, before the attribute went on: both counters reported exactly
// 25,932 - every wire pixel counted twice, once per pass, with the depth test
// contributing nothing. Two identical numbers out of two complementary depth
// functions is the shape of this bug.
//
// Safe here because this shader does neither of the things that make early-Z
// wrong: no discard, no SV_Depth. Depth WRITES are already masked off.
[earlydepthstencil]
float4 PSSel() : SV_Target
{
    InterlockedAdd(gCount[gPass], 1);
    return gColour;
}

// Overlay lines carry their own colour per vertex, so the editor picks colours
// by writing data rather than by calling a state setter. That is the whole
// difference between this and the immediate-mode D3D9 model it replaces.
struct OvOut
{
    float4 pos : SV_Position;
    float4 col : COLOR0;
};

OvOut VSOverlay(float3 pos : POSITION, float4 col : COLOR0)
{
    OvOut o;
    o.pos = mul(float4(pos, 1.0), gMvp);
    o.col = col;
    return o;
}

[earlydepthstencil]
float4 PSOverlay(OvOut i) : SV_Target
{
    InterlockedAdd(gCount[gPass], 1);
    // gColour.a is the pass fade: 1 where the line is in front, less where it
    // is behind something. The rgb stays the caller's.
    return float4(i.col.rgb, i.col.a * gColour.a);
}

// ------------------------------------------------------------------- labels
Texture2D    gFont     : register(t0);
SamplerState gFontSamp : register(s0);

struct LabOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
    float4 col : COLOR0;
};

LabOut VSLabel(float3 anchor : POSITION, float2 off : TEXCOORD0,
               float2 uv : TEXCOORD1, float4 col : COLOR0)
{
    LabOut o;
    float4 clip;

    // gColour.xy carries 2/width and -2/height throughout; the sign flips y
    // because pixel space counts down and NDC counts up.
    if (gScreenSpace != 0)
    {
        // SCREEN SPACE: the anchor is already in pixels from the top-left, so
        // anchor and offset add in the same units and one quad builder serves
        // both modes. w = 1 means no perspective divide to cancel, and z = 0
        // puts it at the near plane - always drawn with the no-depth pipeline,
        // because a HUD that a hillside can occlude is not a HUD.
        float2 px = anchor.xy + off + gTextOffset;
        clip = float4(px.x * gColour.x - 1.0, px.y * gColour.y + 1.0, 0.0, 1.0);
    }
    else
    {
        clip = mul(float4(anchor, 1.0), gMvp);

        // The offset is in PIXELS. Multiplying by clip.w cancels the perspective
        // divide that follows, so a label keeps the same size on screen however
        // far away its anchor is -- which is the whole point of a label.
        clip.xy += (off + gTextOffset) * gColour.xy * clip.w;
    }

    o.pos = clip;
    o.uv = uv;
    o.col = col;
    return o;
}

[earlydepthstencil]
float4 PSLabel(LabOut i) : SV_Target
{
    float a = gFont.Sample(gFontSamp, i.uv).a;

    // Count only what is actually drawn. Putting the InterlockedAdd before the
    // discard would count the transparent parts of every glyph cell, turning the
    // instrument into a measure of quad area rather than of visible text.
    if (a < 0.02) discard;

    InterlockedAdd(gCount[gPass], 1);
    // gColour.w tints the glyph: 1 leaves the caller's colour, 0 makes it black
    // for the shadow pass. A tint rather than a second colour constant, because
    // the shadow only ever needs to be darker, never a different hue.
    return float4(i.col.rgb * gColour.w, i.col.a * a * gColour.z);
}
