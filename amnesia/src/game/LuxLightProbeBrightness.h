// SPDX-License-Identifier: GPL-3.0
#ifndef LUX_LIGHT_PROBE_BRIGHTNESS_H
#define LUX_LIGHT_PROBE_BRIGHTNESS_H

#include <algorithm>
#include <cmath>

#include "Constants.h" // kRayTracedGammaBias, kLightProbeGammaBias

// Shared brightness state for CPU Legacy sensing and asynchronous physical
// probes. Keep the environment separate so the lantern bonus never feeds back.
//
// The physical probe level goes through the same display-gamma lift the
// ray-traced tonemap applies (kRayTracedGammaBias), so gameplay darkness agrees
// with what the screen shows, plus kLightProbeGammaBias to make darkness a
// little more forgiving. The player's own gamma slider is deliberately
// excluded: a display preference must not change gameplay.
class cLuxLightProbeBrightness {
public:
    void Reset() { mfLuminance = 0.0f; mfEnvironment = 1.0f; mbLantern = false; }

    void Update(const float (*samples)[3], int count, float gain) {
        if (!samples || count <= 0) return;
        if (!std::isfinite(gain) || gain <= 0.0f) gain = 1.0f;
        double brightest = 0.0;
        for (int i = 0; i < count; ++i) {
            const double luminance = 0.2126 * Component(samples[i][0])
                                   + 0.7152 * Component(samples[i][1])
                                   + 0.0722 * Component(samples[i][2]);
            brightest = std::max(brightest, luminance);
        }
        mfLuminance = static_cast<float>(brightest);
        const double level = std::min(brightest * gain, 1.0);
        mfEnvironment = static_cast<float>(
            std::pow(level, 1.0 / (1.0 + hpl::kRayTracedGammaBias +
                                  hpl::kLightProbeGammaBias)));
    }

    // Publish the legacy CPU environmental level without applying the GPU
    // probe's overdrive gain or its normalized-level clamp. CPU sums can be
    // greater than one and are not physical irradiance measurements.
    void UpdateLegacy(float level) {
        if (!std::isfinite(level) || level < 0.0f) return;
        mfEnvironment = level;
        mfLuminance = 0.0f;
    }

    void SetLantern(bool active) { mbLantern = active; }
    float GetLevel() const { return mfEnvironment + (mbLantern ? 1.0f : 0.0f); }
    float GetLuminance() const { return mfLuminance; }

private:
    static double Component(float value) {
        return std::isfinite(value) && value > 0.0f ? value : 0.0;
    }
    float mfLuminance = 0.0f;
    float mfEnvironment = 1.0f;
    bool mbLantern = false;
};

#endif
