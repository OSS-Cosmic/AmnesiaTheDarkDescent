#pragma once

// RT-free bindless declarations used only by the Standard environment pass.
// Do not import bindless.slang here: that module also declares the shared TLAS
// and would make this otherwise raster-only shader advertise ray tracing.
#include "Constants.h"
import SceneTypes;
import PerFrame.resource;

#if defined(DXIL)
// D3D12 uses the same explicit register spaces as the engine-global bindless
// module. Import the canonical declarations so DXIL does not contain two
// differently-named resources that overlap the same registers.
import bindless;
#else
[vk::binding(kBindingTexturesCube, 0)]
public TextureCube gEnvironmentTexturesCube[];

[vk::binding(kBindingMaterialSampler, 0)]
public SamplerState gEnvironmentSampler;
#endif

public float4 sampleEnvironmentCube(uint idx, float3 dir, float lodBase)
{
    if (idx == kInvalidTextureIndex)
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    // This is an explicit mip level, not a ray-cone footprint. A zero sky
    // level must not become log2(textureSize), which selects the blurriest mip.
#if defined(DXIL)
    return gTexturesCube[NonUniformResourceIndex(idx)].SampleLevel(
        gMaterialSampler, dir, lodBase);
#else
    return gEnvironmentTexturesCube[NonUniformResourceIndex(idx)].SampleLevel(
        gEnvironmentSampler, dir, lodBase);
#endif
}
