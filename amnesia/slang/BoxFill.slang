#pragma once

// Legacy box lights as a flat global fill. The base game lit every surface
// inside a box with albedo × colour in display space (deferred_light_box_frag.glsl);
// the Standard renderer reproduces that per box light (boxFillContribution),
// and the ray-traced composite applies a linear-space version as its only
// ambient/fill contribution (evaluateBoxFill).

#include "HostDefinitions.h"
#include "Constants.h"
import SceneTypes;
import PerFrame.resource;
import bindless;
import Utils.Color.ColorHelpers;

// One box's contribution to a surface of linear albedo `materialColor`.
// `color` is the raw authored display-space RGB.
public float3 boxFillContribution(float3 materialColor, float3 color)
{
    return sRGBToLinear(max(linearToSRGB(max(materialColor, 0.0f)) * color, 0.0f))
         / kSceneExposure;
}

public bool boxFillContains(float3 boxMin, float3 boxMax, float3 position)
{
    return all(position >= boxMin) && all(position <= boxMax);
}

// Walk gBoxLights in apply order: a Replace box discards what earlier boxes
// added, an Add box accumulates. Unlike Standard's display-space term this is
// plain linear albedo × linear colour, consistent with the ray-traced lights.
// Returns scene-linear radiance before scaling.
public float3 evaluateBoxFill(float3 albedo, float3 position)
{
    float3 fill = float3(0.0f);
    if (!all(isfinite(position)))
        return fill;
    uint count = gPerFrame.boxLightCount;
    for (uint i = 0u; i < count; ++i)
    {
        BoxFillLight box = gBoxLights[i];
        if (!boxFillContains(box.boxMin, box.boxMax, position))
            continue;
        float3 contrib = max(albedo, 0.0f) * box.color;
        fill = box.blendFunc == 0u ? contrib : fill + contrib;
    }
    return fill;
}
