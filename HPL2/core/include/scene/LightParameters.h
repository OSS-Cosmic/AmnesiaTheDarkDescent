#ifndef HPL_LIGHT_PARAMETERS_H
#define HPL_LIGHT_PARAMETERS_H

#include "graphics/RendererMask.h"

namespace hpl {

    //------------------------------------------------------------------------
    // One light element carries both tunings. The retail attributes (Radius,
    // FlickerOffRadius, DiffuseColor, ...) drive Standard; the ray-traced
    // backend reads the same attribute spelled with this prefix when the
    // element authors one, e.g. Re_Intensity, Re_Radius, Re_SourceRadius.
    //
    // Two carve-outs:
    //  * transform (WorldPos/Rotation/Scale, read by SetupWorldEntity) and
    //    ID/Name/RendererMask are never overridable;
    //  * Intensity / Radius / SourceRadius / FlickerOffIntensity are read
    //    ONLY through the prefix on shapes that have a legacy class. "Radius"
    //    on a PointLight is the retail radius (an intensity-like scalar
    //    PromoteLegacyLightParameters consumes); Re_Radius is the ray-traced
    //    reach. Same name, different quantity.
    //
    // AreaLight has no legacy class, so its unprefixed attributes already are
    // the ray-traced schema.
    constexpr const char *kLightOverrideAttributePrefix = "Re_";

    enum eLightElementShape
    {
        eLightElementShape_Point,
        eLightElementShape_Spot,
        eLightElementShape_Area,
        eLightElementShape_Box,
    };

    struct cLightElementInfo
    {
        bool mbValid = false;
        eLightElementShape mShape = eLightElementShape_Point;
        // The shape has no legacy class: its unprefixed attributes already use
        // the ray-traced schema and it never loads on Standard.
        bool mbRayTracedOnly = false;
    };

    // Maps an XML element name to its light shape and model. "AreaLight" is
    // the ray-traced area light (there is no legacy area light).
    cLightElementInfo GetLightElementInfo(const char *asTag);

    // RendererMask used when the element has none: legacy shapes load on both
    // backends whether or not they author Re_* overrides, the ray-traced-only
    // shape on that backend alone.
    unsigned int GetDefaultLightRendererMask(const cLightElementInfo &aInfo);

    struct cLegacyLightInput
    {
        bool mbHasRadius = false; float mfRadius = 1.0f;
        bool mbHasFlickerOffRadius = false; float mfFlickerOffRadius = 0.0f;
    };

    struct cLegacyLightParameters
    {
        float mfRadius = 1.0f;
        float mfFlickerOffRadius = 0.0f;
    };

    struct cRayTracedLightInput
    {
        bool mbHasIntensity = false; float mfIntensity = 1.0f;
        bool mbHasRadius = false; float mfRadius = 0.0f;
        bool mbHasSourceRadius = false; float mfSourceRadius = 0.0f;
        bool mbHasFlickerOffIntensity = false; float mfFlickerOffIntensity = 0.0f;
        // Lit diffuse colour in sRGB space; used to derive the reach when
        // Radius is absent.
        float mfRed = 1.0f; float mfGreen = 1.0f; float mfBlue = 1.0f;
    };

    struct cRayTracedLightParameters
    {
        float mfIntensity = 1.0f;
        float mfRadius = 0.0f;
        float mfSourceRadius = 0.0f;
        float mfFlickerOffIntensity = 0.0f;
    };

    // The authored Radius is kept verbatim.
    cLegacyLightParameters ResolveLegacyLightParameters(const cLegacyLightInput &aInput,
                                                        float afDefaultRadius = 1.0f);

    // A missing Radius is derived from the intensity and lit colour.
    cRayTracedLightParameters ResolveRayTracedLightParameters(const cRayTracedLightInput &aInput);

    // Ray-traced values for a legacy light that authors no Re_* photometry:
    // intensity is the retail Radius and the reach is derived from it, as the
    // dual-value light did.
    cRayTracedLightParameters PromoteLegacyLightParameters(const cLegacyLightParameters &aLegacy,
                                                           float afRed, float afGreen, float afBlue);

    // Whether the element instantiates the ray-traced light class. Box lights
    // have no ray-traced class, so a BoxLight's Re_* attributes are ignored.
    bool ShouldUseRayTracedLightClass(const cLightElementInfo &aInfo, unsigned int alMask,
                                      bool abRayTracedBackend);

    // reach = min(sqrt(maxLinear * intensity / radianceFloor - sourceRadius^2),
    //             kLightReachMaxScale * intensity), and the unclamped inverse.
    // Colours are sRGB.
    //
    // The clamp bounds light-grid occupancy (see DeriveReach in the .cpp) and
    // makes these two NOT strict inverses of each other: DeriveLightReach is the
    // derived path (author gave an intensity), while DeriveLightIntensityForReach
    // solves for an explicitly authored reach, which is taken verbatim.
    float DeriveLightReach(float afIntensity, float afRed, float afGreen, float afBlue);
    float DeriveLightIntensityForReach(float afReach, float afRed, float afGreen, float afBlue);

    //------------------------------------------------------------------------
    // Fades, flicker and scripts drive ONE normalized level that both backends
    // read, so a light dimmed to half stays half of whatever each backend
    // authored rather than snapping to one backend's numbers:
    //
    //     value = off + level * (on - off)
    //
    // Level 1 is each tuning's authored ON value and level 0 its authored
    // flicker OFF value, which is what lets the two backends keep independent
    // flicker depths while sharing one drive. With the usual off = 0 this is
    // just "level scales the authored value".
    float ResolveLightLevelValue(float afOn, float afOff, float afLevel);

    // Colour works the same way: a script that turns a lamp down means "it
    // looks like this now", so it is stored as a scale over the authored
    // colour and applied to BOTH backends' tunings. Writing one tuning's
    // colour instead would leave the other at its authored value, and every
    // lamp a script had turned off would light up again on a backend switch.
    //
    // Returns false when the request cannot be expressed as a ratio -- a
    // channel the tuning authored at zero, asked for something non-zero -- in
    // which case the caller applies the colour outright to both tunings.
    bool DeriveLightColorScale(float afAuthored, float afWanted, float *apScaleOut);

    // The inverse: which level puts this tuning at afValue. False when the
    // tuning has no range to speak of (on == off, including a light authored
    // dark) -- the caller then re-authors the value instead of scaling to it.
    bool DeriveLightLevel(float afOn, float afOff, float afValue, float *apLevelOut);
}

#endif
