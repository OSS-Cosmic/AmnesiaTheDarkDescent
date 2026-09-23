/// Copyright © 2009-2020 Frictional Games
/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Slang port of amnesia/glsl/gui.frag. Pairs with gui.vert.slang — the
// vertex stage exports the same GuiPass UBO layout, so this file
// redeclares it at the same set / binding rather than `import`-ing it
// (small enough that the duplication is cheaper than a shared header).
//
// Bindings (cGuiSet::Render configures these via DescriptorBindingID):
//   set 0 binding 0 : ConstantBuffer<GuiPass> pass
//   set 0 binding 1 : SamplerState           diffuseSampler
//   set 0 binding 2 : Texture2D<float4>      diffuseMap

struct GuiPass
{
    float4x4 mvp;
    float4   clipPlane[4];
    int      textureConfig;
};

[vk::binding(0, 0)]
ConstantBuffer<GuiPass> pass;

[vk::binding(1, 0)]
SamplerState diffuseSampler;

[vk::binding(2, 0)]
Texture2D<float4> diffuseMap;

// textureConfig bitfield (mirrors gui.resource.glsl helpers).
static bool HasDiffuse(int cfg)    { return (cfg & (1 << 0)) != 0; }
static bool HasClipPlanes(int cfg) { return (cfg & (1 << 1)) != 0; }

struct PSIn
{
    [vk::location(0)] float2 v_texcoord : TEXCOORD0;
    [vk::location(1)] float4 v_color : COLOR0;
    // Screen-space (pre-MVP) position so the clip-plane test below stays
    // in the same space as the planes the host writes into pass.clipPlane
    // (cGuiSet::Render uses screen-space planes).
    [vk::location(2)] float4 v_position : TEXCOORD1;
};

struct PSOut
{
    [vk::location(0)] float4 out_color : SV_Target0;
};

[shader("fragment")]
PSOut psMain(PSIn pin)
{
    if (HasClipPlanes(pass.textureConfig))
    {
        for (int i = 0; i < 4; ++i)
        {
            float d = dot(pass.clipPlane[i], float4(pin.v_position.xyz, 1.0f));
            if (d < 0.0f)
                discard;
        }
    }

    float4 color = pin.v_color;
    if (HasDiffuse(pass.textureConfig))
    {
        color *= diffuseMap.Sample(diffuseSampler, pin.v_texcoord);
    }

    PSOut o;
    o.out_color = color;
    return o;
}
