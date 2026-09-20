#pragma once

// Cross-target geometry stream ABI.
//
// Vulkan carries a real device address in the handle. D3D12 carries
// ((descriptorIndex + 1) << 32) | byteOffset; zero remains the caller's
// missing-stream sentinel. Keep the target split here so callers retain the
// same byte-offset load interface on both backends.

#if defined(DXIL)
import bindless;
#endif

public uint geometryDescriptorIndex(uint64_t handle)
{
#if defined(DXIL)
    return (uint)(handle >> 32) - 1u;
#else
    return 0u;
#endif
}

public uint geometryByteOffset(uint64_t handle, uint byteOffset)
{
#if defined(DXIL)
    return (uint)handle + byteOffset;
#else
    return byteOffset;
#endif
}

public uint geometryLoad(uint64_t handle, uint byteOffset)
{
#if defined(DXIL)
    return gGeometryBuffers[NonUniformResourceIndex(geometryDescriptorIndex(handle))]
        .Load(geometryByteOffset(handle, byteOffset));
#else
    uint* stream = (uint*)handle;
    return stream[byteOffset / 4u];
#endif
}

public uint3 geometryLoad3(uint64_t handle, uint byteOffset)
{
#if defined(DXIL)
    return gGeometryBuffers[NonUniformResourceIndex(geometryDescriptorIndex(handle))]
        .Load3(geometryByteOffset(handle, byteOffset));
#else
    uint3* stream = (uint3*)handle;
    return stream[byteOffset / 12u];
#endif
}

public uint4 geometryLoad4(uint64_t handle, uint byteOffset)
{
#if defined(DXIL)
    return gGeometryBuffers[NonUniformResourceIndex(geometryDescriptorIndex(handle))]
        .Load4(geometryByteOffset(handle, byteOffset));
#else
    uint4* stream = (uint4*)handle;
    return stream[byteOffset / 16u];
#endif
}
