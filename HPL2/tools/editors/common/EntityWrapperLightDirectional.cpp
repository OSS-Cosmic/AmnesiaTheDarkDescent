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

#include "EntityWrapperLightDirectional.h"
#include "EntityWrapperLight.h"

#include "EditorBaseClasses.h"
#include "EditorWorld.h"
#include "EditorHelper.h"

#include "EditorWindowViewport.h"

#include "scene/LightDirectional.h"

#include <tinyxml2.h>

//---------------------------------------------------------------------------

// The sun's angular radius, matching cLightDirectional's default.
static const float kDefaultAngularRadius = 0.00465f;

/////////////////////////////////////////////////////////////////////////////
// ICON ENTITY DIRECTIONALLIGHT : CONSTRUCTORS
/////////////////////////////////////////////////////////////////////////////

//---------------------------------------------------------------------------

cIconEntityLightDirectional::cIconEntityLightDirectional(iEntityWrapper* apParent) : iIconEntityLight(apParent, "Spot")
{
}

bool cIconEntityLightDirectional::Create(const tString& asName)
{
	cWorld* pWorld = mpParent->GetEditorWorld()->GetWorld();

	mpEntity = pWorld->CreateLightDirectional(asName);

	return true;
}

//---------------------------------------------------------------------------

/////////////////////////////////////////////////////////////////////////////
// DIRECTIONALLIGHT TYPE : CONSTRUCTORS
/////////////////////////////////////////////////////////////////////////////

//---------------------------------------------------------------------------

cEntityWrapperTypeLightDirectional::cEntityWrapperTypeLightDirectional() : iEntityWrapperTypeLight("DirectionalLight", eEditorEntityLightType_Directional, eLightSchema_RayTracedOnly)
{
	mScaleType = eScaleType_None;

	AddFloat(eLightDirectionalFloat_AngularRadius, "AngularRadius", kDefaultAngularRadius);
}

iEntityWrapperData* cEntityWrapperTypeLightDirectional::CreateSpecificData()
{
	return hplNew(cEntityWrapperDataLightDirectional,(this));
}

//---------------------------------------------------------------------------

/////////////////////////////////////////////////////////////////////////////
// DIRECTIONALLIGHT DATA : CONSTRUCTORS
/////////////////////////////////////////////////////////////////////////////

//---------------------------------------------------------------------------

cEntityWrapperDataLightDirectional::cEntityWrapperDataLightDirectional(iEntityWrapperType* apType) : iEntityWrapperDataLight(apType)
{
}

iEntityWrapper* cEntityWrapperDataLightDirectional::CreateSpecificEntity()
{
	return hplNew(cEntityWrapperLightDirectional,(this));
}

//---------------------------------------------------------------------------

/////////////////////////////////////////////////////////////////////////////
// DIRECTIONALLIGHT : CONSTRUCTORS
/////////////////////////////////////////////////////////////////////////////

//---------------------------------------------------------------------------

cEntityWrapperLightDirectional::cEntityWrapperLightDirectional(iEntityWrapperData* apData) : iEntityWrapperLight(apData)
{
	mfAngularRadius = kDefaultAngularRadius;
}

cEntityWrapperLightDirectional::~cEntityWrapperLightDirectional()
{
}

//---------------------------------------------------------------------------

/////////////////////////////////////////////////////////////////////////////
// DIRECTIONALLIGHT : PUBLIC METHODS
/////////////////////////////////////////////////////////////////////////////

//---------------------------------------------------------------------------

bool cEntityWrapperLightDirectional::SetProperty(int alPropID, const float& afX)
{
	switch(alPropID)
	{
	case eLightDirectionalFloat_AngularRadius:
		SetAngularRadius(afX);
		break;
	default:
		return iEntityWrapperLight::SetProperty(alPropID, afX);
	}

	return true;
}

bool cEntityWrapperLightDirectional::GetProperty(int alPropID, float& afX)
{
	switch(alPropID)
	{
	case eLightDirectionalFloat_AngularRadius:
		afX = GetAngularRadius();
		break;
	default:
		return iEntityWrapperLight::GetProperty(alPropID, afX);
	}

	return true;
}

//---------------------------------------------------------------------------

void cEntityWrapperLightDirectional::SetAngularRadius(float afX)
{
	mfAngularRadius = afX;

	((cLightDirectional*)mpEngineEntity->GetEntity())->SetAngularRadius(mfAngularRadius);
}

//---------------------------------------------------------------------------

void cEntityWrapperLightDirectional::DrawLightTypeSpecific(cEditorWindowViewport* apViewport, DebugDraw* apFunctions,
														   iEditorEditMode* apEditMode, bool abIsSelected)
{
	// Position means nothing to a directional light, so the icon is only a
	// handle: draw an arrow along the way the light travels.
	cVector3f vDir = ((cLightDirectional*)mpEngineEntity->GetEntity())->GetDirection();
	cVector3f vSide = cMath::Vector3Cross(vDir, std::abs(vDir.y) < 0.99f ? cVector3f(0,1,0) : cVector3f(1,0,0));
	vSide.Normalize();

	const float fLength = abIsSelected ? 2.0f : 1.0f;
	const cVector3f vTip = mvPosition + vDir * fLength;
	const cVector3f vBack = vTip - vDir * (fLength * 0.25f);

	apFunctions->DebugDrawLine(mvPosition, vTip, mcolDiffuseColor);
	apFunctions->DebugDrawLine(vTip, vBack + vSide * (fLength * 0.15f), mcolDiffuseColor);
	apFunctions->DebugDrawLine(vTip, vBack - vSide * (fLength * 0.15f), mcolDiffuseColor);
}

//---------------------------------------------------------------------------

/////////////////////////////////////////////////////////////////////////////
// DIRECTIONALLIGHT : PROTECTED METHODS
/////////////////////////////////////////////////////////////////////////////

//---------------------------------------------------------------------------

iEngineEntity* cEntityWrapperLightDirectional::CreateSpecificEngineEntity()
{
	return hplNew(cIconEntityLightDirectional,(this));
}

//---------------------------------------------------------------------------
