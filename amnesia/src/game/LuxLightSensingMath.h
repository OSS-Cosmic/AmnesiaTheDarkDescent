/*
 * Small, renderer-independent helpers used by gameplay light sensing.
 */

#ifndef LUX_LIGHT_SENSING_MATH_H
#define LUX_LIGHT_SENSING_MATH_H

#include <cmath>

namespace lux
{
	// Radius add is an authored compatibility adjustment. It changes the
	// attenuation denominator, but not the light's culling reach.
	inline bool TryGetLightAttenuation(float afDistance, float afReach,
										 float afRadiusAdd, float *apAttenuation)
	{
		const float fDenominator = afReach + afRadiusAdd;
		if(std::isfinite(fDenominator)==false || fDenominator<=0.0f)
		{
			if(apAttenuation) *apAttenuation = 0.0f;
			return false;
		}

		float fAttenuation = 1.0f - afDistance / fDenominator;
		if(fAttenuation<0.0f) fAttenuation = 0.0f;
		if(apAttenuation) *apAttenuation = fAttenuation;
		return true;
	}
}

#endif // LUX_LIGHT_SENSING_MATH_H
