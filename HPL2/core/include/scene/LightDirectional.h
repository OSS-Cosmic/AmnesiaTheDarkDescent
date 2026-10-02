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

#ifndef HPL_LIGHT_DIRECTIONAL_H
#define HPL_LIGHT_DIRECTIONAL_H

#include "scene/Light.h"
#include "scene/SceneTypes.h"

namespace hpl {

	//------------------------------------------

	// Directional (sun / moon) light for outdoor scenes. It shines along the
	// light's local -Z axis (the same forward as a spot light) with constant
	// radiance everywhere: no position, no falloff and no reach. Ray-traced only
	// -- the retail games never had one, so there is no Standard tuning and
	// Standard maps keep their spot/box-light fakes. The hybrid renderer uploads
	// it as the DirectionalLight ILight type and samples it next to the light
	// grid rather than through it (the grid bins by reach).
	class cLightDirectional : public iLight
	{
	public:
		cLightDirectional(tString asName, cResources *apResources);

		// World-space direction the light travels in (unit, local -Z).
		cVector3f GetDirection();

		// How far toward the light a shadow test has to reach to count as open
		// sky (the renderer's kDirectionalLightReach, kept out of this header).
		static float GetReach();

		// Half-angle of the source disk in radians. Sets the width of the soft
		// shadow penumbra; 0 gives hard shadows. Defaults to the real sun's.
		void SetAngularRadius(float afX){ mfAngularRadius = afX; }
		inline float GetAngularRadius() const { return mfAngularRadius; }

	private:
		void ExtraXMLProperties(tinyxml2::XMLElement *apMainElem);
		void UpdateBoundingVolume();

		float mfAngularRadius;
	};

};
#endif // HPL_LIGHT_DIRECTIONAL_H
