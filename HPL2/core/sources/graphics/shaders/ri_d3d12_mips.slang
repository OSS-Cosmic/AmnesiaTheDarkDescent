// Private D3D12 mip generator.  The two entry points are compiled separately
// so the runtime can create ordinary compute PSOs (rather than a library PSO).

struct MipConstants {
    uint4 sourceSize;
    uint4 destinationSize;
};

ConstantBuffer<MipConstants> mipConstants : register(b0);

float srgbToLinear(float x) {
    return x <= 0.04045 ? x / 12.92 : pow((x + 0.055) / 1.055, 2.4);
}
float linearToSrgb(float x) {
    return x <= 0.0031308 ? x * 12.92 : 1.055 * pow(x, 1.0 / 2.4) - 0.055;
}
float3 decode(float3 x) {
    return mipConstants.destinationSize.w != 0 ? float3(srgbToLinear(x.r), srgbToLinear(x.g), srgbToLinear(x.b)) : x;
}
float3 encode(float3 x) {
    return mipConstants.destinationSize.w != 0 ? float3(linearToSrgb(x.r), linearToSrgb(x.g), linearToSrgb(x.b)) : x;
}

float4 filter4(float4 a, float4 b, float4 c, float4 d, float2 f) {
    float3 x = lerp(decode(a.rgb), decode(b.rgb), f.x);
    float3 y = lerp(decode(c.rgb), decode(d.rgb), f.x);
    return float4(encode(lerp(x, y, f.y)), (a.a + b.a + c.a + d.a) * 0.25);
}

Texture2DArray<float4> source2D : register(t0);
RWTexture2DArray<float4> destination2D : register(u0, space1);

[shader("compute")]
[numthreads(8, 8, 1)]
void mip2DArray(uint3 id : SV_DispatchThreadID) {
    if (id.x >= mipConstants.destinationSize.x || id.y >= mipConstants.destinationSize.y ||
        id.z >= mipConstants.destinationSize.z) return;
    float2 p = (float2(id.xy) + 0.5) * float2(mipConstants.sourceSize.xy) /
               float2(mipConstants.destinationSize.xy) - 0.5;
    int2 b = int2(floor(p));
    float2 f = frac(p);
    int2 hi = int2(mipConstants.sourceSize.xy) - 1;
    b = clamp(b, 0, hi);
    int2 bx = clamp(b + int2(1, 0), 0, hi);
    int2 by = clamp(b + int2(0, 1), 0, hi);
    int2 bxy = clamp(b + 1, 0, hi);
    destination2D[id] = filter4(source2D.Load(int4(b, id.z, 0)),
                                 source2D.Load(int4(bx, id.z, 0)),
                                 source2D.Load(int4(by, id.z, 0)),
                                 source2D.Load(int4(bxy, id.z, 0)), f);
}

Texture3D<float4> source3D : register(t0);
RWTexture3D<float4> destination3D : register(u0, space1);

[shader("compute")]
[numthreads(8, 8, 1)]
void mip3D(uint3 id : SV_DispatchThreadID) {
    if (id.x >= mipConstants.destinationSize.x || id.y >= mipConstants.destinationSize.y ||
        id.z >= mipConstants.destinationSize.z) return;
    float3 p = (float3(id) + 0.5) * float3(mipConstants.sourceSize.xyz) /
               float3(mipConstants.destinationSize.xyz) - 0.5;
    int3 b = clamp(int3(floor(p)), 0, int3(mipConstants.sourceSize.xyz) - 1);
    int3 hi = int3(mipConstants.sourceSize.xyz) - 1;
    int3 x = clamp(b + int3(1, 0, 0), 0, hi);
    int3 y = clamp(b + int3(0, 1, 0), 0, hi);
    int3 z = clamp(b + int3(0, 0, 1), 0, hi);
    int3 xy = clamp(b + int3(1, 1, 0), 0, hi);
    int3 xz = clamp(b + int3(1, 0, 1), 0, hi);
    int3 yz = clamp(b + int3(0, 1, 1), 0, hi);
    int3 xyz = clamp(b + int3(1, 1, 1), 0, hi);
    float3 f = frac(p);
    float4 c0 = filter4(source3D.Load(int4(b, 0)), source3D.Load(int4(x, 0)),
                         source3D.Load(int4(y, 0)), source3D.Load(int4(xy, 0)), f.xy);
    float4 c1 = filter4(source3D.Load(int4(z, 0)), source3D.Load(int4(xz, 0)),
                         source3D.Load(int4(yz, 0)), source3D.Load(int4(xyz, 0)), f.xy);
    float4 result = c0 * (1.0 - f.z) + c1 * f.z;
    // filter4 already encoded RGB; decode the interpolated result once more so
    // the z interpolation is also performed in linear space.
    result.rgb = encode(lerp(decode(c0.rgb), decode(c1.rgb), f.z));
    destination3D[id] = result;
}
