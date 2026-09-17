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

    float ResolveLightLevelValue(float afOn, float afOff, float afLevel)
    {
        const float fOn = FiniteOr(afOn, 0.0f);
        const float fOff = FiniteOr(afOff, 0.0f);
        const float fLevel = FiniteOr(afLevel, 0.0f);
        // The value is clamped, not the level: a fade that passes through zero
        // reads as invisible and then comes back, and a level above 1 is a
        // legitimate script overdrive. The product can still overflow when a
        // caller hands in a huge level, so the result is guarded too.
        return std::max(FiniteOr(fOff + fLevel * (fOn - fOff), 0.0f), 0.0f);
    }

    bool DeriveLightColorScale(float afAuthored, float afWanted, float *apScaleOut)
    {
        const float fAuthored = FiniteOr(afAuthored, 0.0f);
        const float fWanted = FiniteOr(afWanted, 0.0f);

        if(fAuthored > 0.0f)
        {
            if(apScaleOut) *apScaleOut = fWanted / fAuthored;
            return true;
        }
        // Authored dark and asked for dark: a zero scale says so exactly.
        if(fWanted <= 0.0f)
        {
            if(apScaleOut) *apScaleOut = 0.0f;
            return true;
        }
        return false;
    }

    bool DeriveLightLevel(float afOn, float afOff, float afValue, float *apLevelOut)
    {
        const float fOn = FiniteOr(afOn, 0.0f);
        const float fOff = FiniteOr(afOff, 0.0f);
        const float fRange = fOn - fOff;
        // No range: a light authored dark, or one whose flicker goes nowhere.
        // There is no ratio to preserve, so the caller re-authors instead.
        if(std::fabs(fRange) <= std::numeric_limits<float>::epsilon()) return false;

        if(apLevelOut) *apLevelOut = (FiniteOr(afValue, 0.0f) - fOff) / fRange;
        return true;
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
        struct Entry { const char *mpTag; eLightElementShape mShape; bool mbRayTracedOnly; };
        static constexpr Entry kEntries[] = {
            {"PointLight", eLightElementShape_Point, false},
            {"SpotLight", eLightElementShape_Spot, false},
            {"BoxLight", eLightElementShape_Box, false},
            // Area lights never had a legacy class, so their unprefixed
            // attributes are the ray-traced schema.
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
                info.mbRayTracedOnly = entry.mbRayTracedOnly;
                return info;
            }
        }
        return info;
    }

    unsigned int GetDefaultLightRendererMask(const cLightElementInfo &aInfo)
    {
        return aInfo.mbRayTracedOnly ? kRendererMaskRayTraced : kRendererMaskAll;
    }

    cLegacyLightParameters ResolveLegacyLightParameters(const cLegacyLightInput &aInput,
                                                        float afDefaultRadius)
    {
        cLegacyLightParameters result;
        // Authored retail values are kept verbatim.
        result.mfRadius = aInput.mbHasRadius ? aInput.mfRadius : afDefaultRadius;
        result.mfFlickerOffRadius = aInput.mbHasFlickerOffRadius ? aInput.mfFlickerOffRadius : 0.0f;
        return result;
    }

    cRayTracedLightParameters ResolveRayTracedLightParameters(const cRayTracedLightInput &aInput)
    {
        cRayTracedLightParameters result;
        result.mfIntensity = FiniteOr(aInput.mbHasIntensity ? aInput.mfIntensity : 1.0f, 0.0f);
        result.mfRadius = aInput.mbHasRadius
            ? FiniteOr(aInput.mfRadius, 0.0f)
            : DeriveReach(result.mfIntensity, aInput.mfRed, aInput.mfGreen, aInput.mfBlue);
        result.mfSourceRadius = aInput.mbHasSourceRadius ? FiniteOr(aInput.mfSourceRadius, 0.0f) : 0.0f;
        result.mfFlickerOffIntensity = aInput.mbHasFlickerOffIntensity
            ? FiniteOr(aInput.mfFlickerOffIntensity, 0.0f) : 0.0f;
        return result;
    }

    cRayTracedLightParameters PromoteLegacyLightParameters(const cLegacyLightParameters &aLegacy,
                                                           float afRed, float afGreen, float afBlue)
    {
        cRayTracedLightParameters result;
        result.mfIntensity = FiniteOr(aLegacy.mfRadius, 0.0f);
        result.mfRadius = DeriveReach(result.mfIntensity, afRed, afGreen, afBlue);
        result.mfSourceRadius = 0.0f;
        result.mfFlickerOffIntensity = FiniteOr(aLegacy.mfFlickerOffRadius, 0.0f);
        return result;
    }

    bool ShouldUseRayTracedLightClass(const cLightElementInfo &aInfo, unsigned int alMask,
                                      bool abRayTracedBackend)
    {
        if(aInfo.mbValid==false) return false;
        // The area light has no legacy class, so it is always the ray-traced one.
        if(aInfo.mbRayTracedOnly) return true;
        // Box lights have no ray-traced class at all.
        return aInfo.mShape != eLightElementShape_Box && abRayTracedBackend &&
               (SanitizeRendererMask(alMask) & kRendererMaskRayTraced) != 0u;
    }
}
