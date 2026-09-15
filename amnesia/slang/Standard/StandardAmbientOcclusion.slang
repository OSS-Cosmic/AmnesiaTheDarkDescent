#pragma once

// Standard ambient occlusion: constants and helpers shared by the AO compute
// passes, ported from enhanced's hbao_courseAO.comp.fsl.
//
// View space used by the AO math is enhanced's +Z-forward space: positive
// linear depth along the view axis, x right, y up. Standard's view normals are
// right-handed (-Z forward), so the coarse pass flips normal.z before use.

enum : uint32_t {
  kStandardAOPreparedDepthBinding = 12
};

// Must match cStandardAmbientOcclusionPass::Render's AOConstants (all 4-byte
// scalars, so std140 packs them without padding).
public struct AOConstants {
  public float tanHalfFovX;
  public float tanHalfFovY;
  public float2 ndcToViewMul;
  public float2 ndcToViewAdd;
  public float zNear;
  public float zFar;
  public uint2 viewportDim;
  public uint2 quarterDim;
  public float2 viewportTexel;
};

public static const float RADIUS = 1.3f;
public static const float NEG_INV_RADIUS_SQUARED = -1.0f / (RADIUS * RADIUS);
public static const float VIEW_COS_BIAS = 0.3f;
public static const float SMALL_SCALE_AO_AMOUNT = 1.0f;
public static const float LARGE_SCALE_AO_AMOUNT = 1.0f;
public static const uint NUM_DIRECTIONS = 9;
public static const uint NUM_STEPS = 10;

// (cos(rand), sin(rand), rand)
public static const float3 JITTER[32] = {
    float3(0.99119387704715, 0.1324186471167839, 0.8789781044196688),
    float3(0.9951085281046705, 0.09878773857800473, 0.6717369021173125),
    float3(0.9619403782691786, 0.2732594164074674, 0.031370069190888694),
    float3(0.9932972716170069, 0.1155877597248517, 0.12694944822347798),
    float3(0.9530320708614084, 0.30286939744651575, 0.8635004448099001),
    float3(0.99119387704715, 0.1324186471167839, 0.8789781044196688),
    float3(0.9837813593746079, 0.1793717841441309, 0.9096760935635397),
    float3(0.9522979598421211, 0.3051697817290138, 0.422689892507499),
    float3(0.9332713936288961, 0.3591719725061772, 0.37136344164461),
    float3(0.9593267156654712, 0.28229816260560403, 0.690268942677605),
    float3(0.9807385601406718, 0.19532505382874432, 0.4389850019224203),
    float3(0.9938594170665236, 0.1106501654232378, 0.7244546322162781),
    float3(0.9951085281046705, 0.09878773857800473, 0.6717369021173125),
    float3(0.9619403782691786, 0.2732594164074674, 0.031370069190888694),
    float3(0.9932972716170069, 0.1155877597248517, 0.12694944822347798),
    float3(0.9738096326160937, 0.227364903677126, 0.8709658361988128),
    float3(0.9690078195547853, 0.24703005007828605, 0.5452133789889245),
    float3(0.9738096326160937, 0.227364903677126, 0.8709658361988128),
    float3(0.9690078195547853, 0.24703005007828605, 0.5452133789889245),
    float3(0.9947149244060405, 0.10267530941704105, 0.1653413480379453),
    float3(0.9901008912383048, 0.14035749060564778, 0.5441034027939922),
    float3(0.9947149244060405, 0.10267530941704105, 0.1653413480379453),
    float3(0.9901008912383048, 0.14035749060564778, 0.5441034027939922),
    float3(0.9431490407173596, 0.3323701054456075, 0.7981023606982642),
    float3(0.9530320708614084, 0.30286939744651575, 0.8635004448099001),
    float3(0.9837813593746079, 0.1793717841441309, 0.9096760935635397),
    float3(0.9522979598421211, 0.3051697817290138, 0.422689892507499),
    float3(0.9332713936288961, 0.3591719725061772, 0.37136344164461),
    float3(0.9431490407173596, 0.3323701054456075, 0.7981023606982642),
    float3(0.9593267156654712, 0.28229816260560403, 0.690268942677605),
    float3(0.9807385601406718, 0.19532505382874432, 0.4389850019224203),
    float3(0.9938594170665236, 0.1106501654232378, 0.7244546322162781)
};

// uv in [0,1] (top-left origin) and positive linear depth -> +Z-forward view position.
public float3 ComputeViewspacePosition(AOConstants c, float2 uv, float viewDepth)
{
    return float3((uv * c.ndcToViewMul + c.ndcToViewAdd) * viewDepth, viewDepth);
}

public float ComputeAO(float3 viewPosKernelCenter, float3 viewNormalKernelCenter,
                       float3 viewSamplePos, float negInvR2)
{
    float3 dir = viewSamplePos - viewPosKernelCenter;
    float dirLenSq = dot(dir, dir);
    float viewCos = dot(viewNormalKernelCenter, dir) * rsqrt(max(dirLenSq, 1e-8f));
    float falloff = dirLenSq * negInvR2 + 1.0f;
    return saturate(viewCos - VIEW_COS_BIAS) * saturate(falloff);
}

// RotationCosSin is (cos(alpha), sin(alpha)).
public float2 RotateDirection(float2 v, float2 rotationCosSin)
{
    return float2(v.x * rotationCosSin.x - v.y * rotationCosSin.y,
                  v.x * rotationCosSin.y + v.y * rotationCosSin.x);
}
