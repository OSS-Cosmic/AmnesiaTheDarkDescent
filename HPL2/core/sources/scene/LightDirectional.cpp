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

#include "scene/LightDirectional.h"

#include "resources/XmlHelper.h"
#include "math/Math.h"

#include "Constants.h"

namespace hpl {

	//////////////////////////////////////////////////////////////////////////
	// CONSTRUCTORS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	// The sun's angular radius, ~0.27 degrees.
	static constexpr float kDefaultSunAngularRadius = 0.00465f;

	cLightDirectional::cLightDirectional(tString asName, cResources *apResources) : iLight(asName,apResources)
	{
		mLightType = eLightType_Directional;
		mActiveModel = eLightModel_RayTraced;
		// There is no legacy directional light, so this shape has no Standard tuning.
		mState.Tuning(eLightModel_Legacy).mbPresent = false;

		mfAngularRadius = kDefaultSunAngularRadius;

		UpdateBoundingVolume();
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PUBLIC METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	cVector3f cLightDirectional::GetDirection()
	{
		const cMatrixf &world = GetWorldMatrix();
		cVector3f vDir(-world.m[0][2], -world.m[1][2], -world.m[2][2]);
		const float fLength = vDir.Length();
		return fLength > 1e-6f ? vDir / fLength : cVector3f(0, -1, 0);
	}

	//-----------------------------------------------------------------------

	float cLightDirectional::GetReach()
	{
		return kDirectionalLightReach;
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PRIVATE METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	void cLightDirectional::ExtraXMLProperties(tinyxml2::XMLElement *apMainElem)
	{
		mfAngularRadius = GetAttributeFloat(apMainElem, "AngularRadius", mfAngularRadius);
	}

	//-----------------------------------------------------------------------

	void cLightDirectional::UpdateBoundingVolume()
	{
		// It lights everything, so its cull volume has to contain the map. The
		// reach constant is the same one the shadow rays use.
		mBoundingVolume.SetSize(kDirectionalLightReach*2.0f);
		mBoundingVolume.SetPosition(GetWorldPosition());
	}

	//-----------------------------------------------------------------------

}
