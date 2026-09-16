#include "scene/LightParameters.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace hpl {

    namespace {
        constexpr float kLightRadianceFloor = 0.005f;
        constexpr float kPointLightSourceRadiusSq = 0.25f;

        float FiniteOr(float value, float fallback)
        {
            if(std::isfinite(value)) return value;
            return value > 0.0f ? std::numeric_limits<float>::max() : fallback;
        }

        float SrgbToLinear(float value)
        {
            if(!std::isfinite(value)) return value > 0.0f ? 1.0f : 0.0f;
            return value <= 0.04045f ? value / 12.92f
                                     : std::pow((value + 0.055f) / 1.055f, 2.4f);
        }

        float DeriveReach(float intensity, float red, float green, float blue)
        {
            const float safeIntensity = FiniteOr(intensity, 0.0f);
            const float maxChannel = std::max(SrgbToLinear(red),
                                      std::max(SrgbToLinear(green), SrgbToLinear(blue)));
            const float reachSq = maxChannel > 0.0f
                ? maxChannel * safeIntensity / kLightRadianceFloor
                    - kPointLightSourceRadiusSq
                : 0.0f;
            if(reachSq > 0.0f)
            {
                const double safeReachSq = std::min(static_cast<double>(reachSq),
                    static_cast<double>(std::numeric_limits<float>::max()));
                return static_cast<float>(std::sqrt(safeReachSq));
            }
            return std::isfinite(safeIntensity) ? safeIntensity : 0.0f;
        }
    }

    float DeriveLightReach(float afIntensity, float afRed, float afGreen, float afBlue)
    {
        return DeriveReach(afIntensity, afRed, afGreen, afBlue);
    }

    float DeriveLightIntensityForReach(float afReach, float afRed, float afGreen, float afBlue)
    {
        if(!(afReach > 0.0f)) return 0.0f;
        const float maxChannel = std::max(SrgbToLinear(afRed),
                                  std::max(SrgbToLinear(afGreen), SrgbToLinear(afBlue)));
        // No colour to solve against: any intensity leaves the light black, so
        // hand back the reach, matching DeriveReach's degenerate fallback.
        if(!(maxChannel > 0.0f)) return afReach;
        return (afReach * afReach + kPointLightSourceRadiusSq) * kLightRadianceFloor / maxChannel;
    }

    cLightElementInfo GetLightElementInfo(const char *asTag)
    {
        struct Entry { const char *mpTag; eLightElementShape mShape; bool mbOverdrive; };
        static constexpr Entry kEntries[] = {
            {"PointLight", eLightElementShape_Point, false},
            {"SpotLight", eLightElementShape_Spot, false},
            {"BoxLight", eLightElementShape_Box, false},
            {"Re_PointLight", eLightElementShape_Point, true},
            {"Re_SpotLight", eLightElementShape_Spot, true},
            {"Re_AreaLight", eLightElementShape_Area, true},
            // Area lights never had a legacy class; the pre-split element name
            // keeps loading as the Overdrive area light.
            {"AreaLight", eLightElementShape_Area, true},
        };
        cLightElementInfo info;
        if(asTag == nullptr) return info;
        for(const Entry &entry : kEntries)
        {
            if(std::strcmp(asTag, entry.mpTag) == 0)
            {
                info.mbValid = true;
                info.mShape = entry.mShape;
                info.mbOverdrive = entry.mbOverdrive;
                return info;
            }
        }
        return info;
    }

    unsigned int GetDefaultLightRendererMask(const cLightElementInfo &aInfo)
    {
        return aInfo.mbOverdrive ? kRendererMaskOverdrive : kRendererMaskAll;
    }

    cLegacyLightParameters ResolveLegacyLightParameters(const cLegacyLightInput &aInput,
                                                        float afDefaultRadius)
    {
        cLegacyLightParameters result;
        // Authored retail values are kept verbatim.
        result.mfRadius = aInput.mbHasRadius ? aInput.mfRadius : afDefaultRadius;
        result.mfFlickerOffRadius = aInput.mbHasFlickerOffRadius ? aInput.mfFlickerOffRadius : 0.0f;
        result.mlRendererMask = aInput.mbHasRendererMask
            ? SanitizeRendererMask(aInput.mlRendererMask) : kRendererMaskAll;
        return result;
    }

    cOverdriveLightParameters ResolveOverdriveLightParameters(const cOverdriveLightInput &aInput)
    {
        cOverdriveLightParameters result;
        result.mfIntensity = FiniteOr(aInput.mbHasIntensity ? aInput.mfIntensity : 1.0f, 0.0f);
        result.mfRadius = aInput.mbHasRadius
            ? FiniteOr(aInput.mfRadius, 0.0f)
            : DeriveReach(result.mfIntensity, aInput.mfRed, aInput.mfGreen, aInput.mfBlue);
        result.mfSourceRadius = aInput.mbHasSourceRadius ? FiniteOr(aInput.mfSourceRadius, 0.0f) : 0.0f;
        result.mfFlickerOffIntensity = aInput.mbHasFlickerOffIntensity
            ? FiniteOr(aInput.mfFlickerOffIntensity, 0.0f) : 0.0f;
        if(aInput.mbHasRendererMask)
        {
            const unsigned int mask = SanitizeRendererMask(aInput.mlRendererMask);
            result.mbStrippedStandardBit = (mask & kRendererMaskStandard) != 0u;
            result.mlRendererMask = mask & kRendererMaskOverdrive;
        }
        return result;
    }

    cOverdriveLightParameters PromoteLegacyLightParameters(const cLegacyLightParameters &aLegacy,
                                                           float afRed, float afGreen, float afBlue)
    {
        cOverdriveLightParameters result;
        result.mfIntensity = FiniteOr(aLegacy.mfRadius, 0.0f);
        result.mfRadius = DeriveReach(result.mfIntensity, afRed, afGreen, afBlue);
        result.mfSourceRadius = 0.0f;
        result.mfFlickerOffIntensity = FiniteOr(aLegacy.mfFlickerOffRadius, 0.0f);
        result.mlRendererMask = SanitizeRendererMask(aLegacy.mlRendererMask) & kRendererMaskOverdrive;
        return result;
    }

    bool ShouldPromoteLegacyLight(const cLightElementInfo &aInfo, unsigned int alMask,
                                  bool abOverdriveBackend)
    {
        return aInfo.mbValid && !aInfo.mbOverdrive && aInfo.mShape != eLightElementShape_Box &&
               abOverdriveBackend && (SanitizeRendererMask(alMask) & kRendererMaskOverdrive) != 0u;
    }
}
