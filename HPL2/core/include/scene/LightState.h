/*
 * Copyright © 2009-2020 Frictional Games
 *
 * This file is part of Amnesia: The Dark Descent.
 *
 * Amnesia: The Dark Descent is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * Amnesia: The Dark Descent is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Amnesia: The Dark Descent.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef HPL_LIGHT_STATE_H
#define HPL_LIGHT_STATE_H

#include "graphics/Color.h"

namespace hpl {

	//------------------------------------------

	// Which backend a tuning describes. A light carries BOTH and resolves one
	// per query from the world it lives in, so the backend is no longer baked
	// into the light at load.
	enum eLightModel
	{
		eLightModel_Legacy,
		eLightModel_RayTraced,
	};

	//------------------------------------------

	// One backend's authored tuning. Fades, flicker and scripts never write
	// these -- they move the shared level on cLightState, so both backends keep
	// their own authored numbers and move together in proportion.
	class cLightTuningState
	{
	public:
		// Ray-traced gain. Inert in the Standard tuning, where the retail
		// Radius is the whole photometry.
		float mfIntensity = 1.0f;
		// Where attenuation ends: the retail Radius on Standard, the reach on
		// ray-traced. Same name in both, different quantity -- see
		// notes/merged-light-elements.md.
		float mfReach = 1.0f;
		float mfSourceRadius = 0.0f;
		// No reach was authored, so it follows the intensity and colour and
		// keeps following them as scripts change them.
		bool mbReachFollowsIntensity = false;

		// Endpoints of the animated value at level 1 and level 0. The animated
		// value IS mfReach on Standard and mfIntensity on ray-traced.
		float mfOnValue = 1.0f;
		float mfOffValue = 0.0f;

		cColor mDiffuseColor = cColor(1);
		cColor mDefaultDiffuseColor = cColor(1);
		bool mbCastShadows = false;

		// The map authored this tuning outright (retail attributes, or Re_*
		// ones). False means the loader promoted it from the other tuning, and
		// a later authoring write may re-derive it.
		bool mbAuthored = false;
		// This shape has no such backend at all: a box light has no ray-traced
		// tuning, an area light no Standard one.
		bool mbPresent = true;
	};

	//------------------------------------------

	// The colour drive shared by both tunings: a scale over whichever tuning is
	// active, so it means the same thing on either backend and survives a
	// switch. Snapshotted whole by the flicker, which has to remember the
	// colour it returns to without latching one backend's numbers.
	class cLightColorDrive
	{
	public:
		cColor mColorScale = cColor(1);
		// A script faded a channel the tuning authored at zero, so there is no
		// ratio to scale: this colour applies outright until the next ratio
		// write clears it.
		bool mbColorAbsolute = false;
		cColor mAbsoluteColor = cColor(1);
	};

	//------------------------------------------

	// A light's per-backend tuning plus the drive shared between them.
	class cLightState
	{
	public:
		cLightTuningState mStandard;
		cLightTuningState mRayTraced;

		// 1 = each tuning's authored ON value, 0 = its authored flicker OFF
		// value. Shared, so a light dimmed to half is half on both backends.
		float mfLevel = 1.0f;

		// Colour drive, per channel, over whichever tuning is active.
		cLightColorDrive mColorDrive;

		cLightTuningState& Tuning(eLightModel aModel)
			{ return aModel==eLightModel_RayTraced ? mRayTraced : mStandard; }
		const cLightTuningState& Tuning(eLightModel aModel) const
			{ return aModel==eLightModel_RayTraced ? mRayTraced : mStandard; }

		// The animated value of a tuning: its reach on Standard, its intensity
		// on ray-traced.
		static float GetTuningAnimatedValue(const cLightTuningState& aTuning, eLightModel aModel)
			{ return aModel==eLightModel_RayTraced ? aTuning.mfIntensity : aTuning.mfReach; }
	};

	//------------------------------------------

};
#endif // HPL_LIGHT_STATE_H
