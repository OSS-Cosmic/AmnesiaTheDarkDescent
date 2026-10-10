// Retail AMFP display-space effect equations, shared with GPU regression tests.
public float3 legacyOverlay(float3 base, float3 blend)
{
    return lerp(2.0f*base*blend, 1.0f-2.0f*(1.0f-base)*(1.0f-blend), step(0.5f,base));
}

public float legacyInfectionMask(float gradient, float threshold, float infection, float exponent)
{
    return pow(saturate(1.0f+gradient-threshold+infection), exponent>0.0f ? exponent : 1.0f);
}

public float2 legacyWaterDistortion(float2 normalXY, float scale, float viewZ, float aspect)
{
    return normalXY*float2(1.0f,-aspect)*scale/min(viewZ,-1.0e-3f);
}

public float3 legacyWaterColor(float3 scene, float3 tint, float3 reflected, float fresnel)
{
    return tint*scene+reflected*saturate(fresnel);
}

// Both retail games use this tangent-space light when reflections are absent.
// Returns display-space diffuse and specular factors respectively.
public float2 legacyWaterFallback(float3 tangentNormal)
{
    float ndotl = max(dot(normalize(float3(0.5f)), tangentNormal), 0.0f);
    return float2(ndotl * 0.5f + 0.5f, pow(ndotl, 16.0f));
}
