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

#include "scene/LightArea.h"
#include "graphics/Image.h"

#include "resources/XmlHelper.h"
#include "resources/TextureManager.h"
#include "resources/Resources.h"
#include "math/Math.h"

#include "scene/World.h"
#include "scene/Scene.h"
#include "engine/Engine.h"


namespace hpl {

	//////////////////////////////////////////////////////////////////////////
	// CONSTRUCTORS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	cLightArea::cLightArea(tString asName, cResources *apResources) : iLight(asName,apResources)
	{
		mLightType = eLightType_Area;
		mActiveModel = eLightModel_RayTraced;
		// There is no legacy area light, so this shape has no Standard tuning.
		mState.Tuning(eLightModel_Legacy).mbPresent = false;

		mfWidth = 1.0f;
		mfHeight = 1.0f;
		mfBarnDoorAngle = cMath::ToRad(45.0f);
		mfBarnDoorLength = 0.0f;

		UpdateBoundingVolume();
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PUBLIC METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	void cLightArea::SetWidth(float afX)
	{
		mfWidth = afX;
		mbUpdateBoundingVolume = true;
		SetTransformUpdated();
	}

	//-----------------------------------------------------------------------

	void cLightArea::SetHeight(float afX)
	{
		mfHeight = afX;
		mbUpdateBoundingVolume = true;
		SetTransformUpdated();
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PRIVATE METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	void cLightArea::ExtraXMLProperties(tinyxml2::XMLElement *apMainElem)
	{
		mfWidth = GetAttributeFloat(apMainElem, "SourceWidth", mfWidth);
		mfHeight = GetAttributeFloat(apMainElem, "SourceHeight", mfHeight);
		mfBarnDoorAngle = GetAttributeFloat(apMainElem, "BarnDoorAngle", mfBarnDoorAngle);
		mfBarnDoorLength = GetAttributeFloat(apMainElem, "BarnDoorLength", mfBarnDoorLength);

		// Optional source texture — stored in the base gobo slot (a generic 2D Image
		// resolved to a bindless slot in the renderer's area-light upload). Tints the
		// emission; does not affect shadows.
		tString sSourceTex = GetAttributeString(apMainElem, "SourceTexture");
		if(sSourceTex != "")
		{
			SharedResourceHandle<Image> pImage = mpTextureManager->Create2DImage(sSourceTex,true);
			if(pImage)
				SetGoboTexture(pImage.Release());
		}

		mbUpdateBoundingVolume = true;
	}

	//-----------------------------------------------------------------------

	void cLightArea::UpdateBoundingVolume()
	{
		// Reach-sphere like a point light: the emitter is planar but it lights a
		// hemisphere out to its reach, so a sphere sized to the reach is the
		// correct conservative cull volume.
		mBoundingVolume.SetSize(GetRadius()*2.0f);
		mBoundingVolume.SetPosition(GetWorldPosition());
	}

	//-----------------------------------------------------------------------

}
