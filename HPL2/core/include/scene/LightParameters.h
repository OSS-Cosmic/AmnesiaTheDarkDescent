#ifndef HPL_LIGHT_PARAMETERS_H
#define HPL_LIGHT_PARAMETERS_H

#include "graphics/RendererMask.h"

namespace hpl {

    //------------------------------------------------------------------------
    // Legacy / Overdrive light schema. Legacy elements (PointLight, SpotLight,
    // BoxLight) carry only the retail Radius; Overdrive elements
    // (Re_PointLight, Re_SpotLight, Re_AreaLight) carry
    // Intensity, Radius (reach) and SourceRadius.

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
        bool mbOverdrive = false;
    };

    // Maps an XML element name to its light shape and model. "AreaLight" is
    // accepted as the Overdrive area light (there is no legacy area light).
    cLightElementInfo GetLightElementInfo(const char *asTag);

    // RendererMask used when the element has none: legacy lights load on both
    // backends (promoted on Overdrive), Overdrive lights only on Overdrive.
    unsigned int GetDefaultLightRendererMask(const cLightElementInfo &aInfo);

    struct cLegacyLightInput
    {
        bool mbHasRadius = false; float mfRadius = 1.0f;
        bool mbHasFlickerOffRadius = false; float mfFlickerOffRadius = 0.0f;
        bool mbHasRendererMask = false; unsigned int mlRendererMask = kRendererMaskAll;
    };

    struct cLegacyLightParameters
    {
        float mfRadius = 1.0f;
        float mfFlickerOffRadius = 0.0f;
        unsigned int mlRendererMask = kRendererMaskAll;
    };

    struct cOverdriveLightInput
    {
        bool mbHasIntensity = false; float mfIntensity = 1.0f;
        bool mbHasRadius = false; float mfRadius = 0.0f;
        bool mbHasSourceRadius = false; float mfSourceRadius = 0.0f;
        bool mbHasFlickerOffIntensity = false; float mfFlickerOffIntensity = 0.0f;
        // Lit diffuse colour in sRGB space; used to derive the reach when
        // Radius is absent.
        float mfRed = 1.0f; float mfGreen = 1.0f; float mfBlue = 1.0f;
        bool mbHasRendererMask = false; unsigned int mlRendererMask = kRendererMaskOverdrive;
    };

    struct cOverdriveLightParameters
    {
        float mfIntensity = 1.0f;
        float mfRadius = 0.0f;
        float mfSourceRadius = 0.0f;
        float mfFlickerOffIntensity = 0.0f;
        unsigned int mlRendererMask = kRendererMaskOverdrive;
        // An Overdrive light never loads on Standard; set when an authored
        // mask included the Standard bit so the loader can warn.
        bool mbStrippedStandardBit = false;
    };

    // The authored Radius is kept verbatim.
    cLegacyLightParameters ResolveLegacyLightParameters(const cLegacyLightInput &aInput,
                                                        float afDefaultRadius = 1.0f);

    // A missing Radius is derived from the intensity and lit colour.
    cOverdriveLightParameters ResolveOverdriveLightParameters(const cOverdriveLightInput &aInput);

    // Overdrive values for a legacy light loaded on Overdrive: intensity is the
    // retail Radius and the reach is derived from it, as the dual-value light
    // did. The Standard bit is dropped from the mask.
    cOverdriveLightParameters PromoteLegacyLightParameters(const cLegacyLightParameters &aLegacy,
                                                           float afRed, float afGreen, float afBlue);

    // Whether a legacy element loads as the Overdrive class. Box lights are
    // Standard only.
    bool ShouldPromoteLegacyLight(const cLightElementInfo &aInfo, unsigned int alMask,
                                  bool abOverdriveBackend);

    // reach^2 = maxLinear * intensity / radianceFloor - sourceRadius^2, and its
    // inverse. Colours are sRGB.
    float DeriveLightReach(float afIntensity, float afRed, float afGreen, float afBlue);
    float DeriveLightIntensityForReach(float afReach, float afRed, float afGreen, float afBlue);
}

#endif
