public static const float3 kFallbackColor = float3(1.0f, 0.5f, 0.25f);

public float3 applyFallbackColor(float3 color)
{
    return color * kFallbackColor;
}

public uint sharedThreadValue(uint value)
{
    return value;
}
