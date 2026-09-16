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

#include "EntityWrapperLightArea.h"
#include "EntityWrapperLight.h"

#include "EditorBaseClasses.h"
#include "EditorWorld.h"
#include "EditorHelper.h"

#include "EditorWindowViewport.h"

#include "scene/LightArea.h"

#include <tinyxml2.h>

#include <cmath>

//---------------------------------------------------------------------------

/////////////////////////////////////////////////////////////////////////////
// ICON ENTITY AREALIGHT : CONSTRUCTORS
/////////////////////////////////////////////////////////////////////////////

//---------------------------------------------------------------------------

cIconEntityLightArea::cIconEntityLightArea(iEntityWrapper* apParent) : iIconEntityLight(apParent, "Box")
{
}

bool cIconEntityLightArea::Create(const tString& asName)
{
	cWorld* pWorld = mpParent->GetEditorWorld()->GetWorld();

	mpEntity = pWorld->CreateLightArea(asName);

	return true;
}

//---------------------------------------------------------------------------

/////////////////////////////////////////////////////////////////////////////
// AREALIGHT TYPE : CONSTRUCTORS
/////////////////////////////////////////////////////////////////////////////

//---------------------------------------------------------------------------

cEntityWrapperTypeLightArea::cEntityWrapperTypeLightArea() : iEntityWrapperTypeLight("Re_AreaLight", eEditorEntityLightType_Area, true)
{
	mScaleType = eScaleType_None;

	AddFloat(eLightAreaFloat_SourceWidth, "SourceWidth", 1.0f);
	AddFloat(eLightAreaFloat_SourceHeight, "SourceHeight", 1.0f);
	AddFloat(eLightAreaFloat_BarnDoorAngle, "BarnDoorAngle", cMath::ToRad(45.0f));
	AddFloat(eLightAreaFloat_BarnDoorLength, "BarnDoorLength", 0.0f);
	AddString(eLightAreaStr_SourceTexture, "SourceTexture");
}

bool cEntityWrapperTypeLightArea::IsAppropriateType(tinyxml2::XMLElement* apElement)
{
	return iEntityWrapperTypeLight::IsAppropriateType(apElement) || tString(apElement->Value())=="AreaLight";
}

iEntityWrapperData* cEntityWrapperTypeLightArea::CreateSpecificData()
{
	return hplNew(cEntityWrapperDataLightArea,(this));
}

//---------------------------------------------------------------------------

/////////////////////////////////////////////////////////////////////////////
// AREALIGHT DATA : CONSTRUCTORS
/////////////////////////////////////////////////////////////////////////////

//---------------------------------------------------------------------------

cEntityWrapperDataLightArea::cEntityWrapperDataLightArea(iEntityWrapperType* apType) : iEntityWrapperDataLight(apType)
{
}

iEntityWrapper* cEntityWrapperDataLightArea::CreateSpecificEntity()
{
	return hplNew(cEntityWrapperLightArea,(this));
}

//---------------------------------------------------------------------------

/////////////////////////////////////////////////////////////////////////////
// AREALIGHT : CONSTRUCTORS
/////////////////////////////////////////////////////////////////////////////

//---------------------------------------------------------------------------

cEntityWrapperLightArea::cEntityWrapperLightArea(iEntityWrapperData* apData) : iEntityWrapperLight(apData)
{
	mfWidth = 1.0f;
	mfHeight = 1.0f;
	mfBarnDoorAngle = cMath::ToRad(45.0f);
	mfBarnDoorLength = 0.0f;
}

cEntityWrapperLightArea::~cEntityWrapperLightArea()
{
}

//---------------------------------------------------------------------------

/////////////////////////////////////////////////////////////////////////////
// AREALIGHT : PUBLIC METHODS
/////////////////////////////////////////////////////////////////////////////

//---------------------------------------------------------------------------

bool cEntityWrapperLightArea::SetProperty(int alPropID, const float& afX)
{
	switch(alPropID)
	{
	case eLightAreaFloat_SourceWidth:
		SetWidth(afX);
		break;
	case eLightAreaFloat_SourceHeight:
		SetHeight(afX);
		break;
	case eLightAreaFloat_BarnDoorAngle:
		SetBarnDoorAngle(afX);
		break;
	case eLightAreaFloat_BarnDoorLength:
		SetBarnDoorLength(afX);
		break;
	default:
		return iEntityWrapperLight::SetProperty(alPropID, afX);
	}

	return true;
}

bool cEntityWrapperLightArea::SetProperty(int alPropID, const tString& asX)
{
	switch(alPropID)
	{
	case eLightAreaStr_SourceTexture:
		SetSourceTexture(asX);
		break;
	default:
		return iEntityWrapperLight::SetProperty(alPropID, asX);
	}

	return true;
}

bool cEntityWrapperLightArea::GetProperty(int alPropID, float& afX)
{
	switch(alPropID)
	{
	case eLightAreaFloat_SourceWidth:
		afX = GetWidth();
		break;
	case eLightAreaFloat_SourceHeight:
		afX = GetHeight();
		break;
	case eLightAreaFloat_BarnDoorAngle:
		afX = GetBarnDoorAngle();
		break;
	case eLightAreaFloat_BarnDoorLength:
		afX = GetBarnDoorLength();
		break;
	default:
		return iEntityWrapperLight::GetProperty(alPropID, afX);
	}

	return true;
}

bool cEntityWrapperLightArea::GetProperty(int alPropID, tString& asX)
{
	switch(alPropID)
	{
	case eLightAreaStr_SourceTexture:
		asX = GetSourceTexture();
		break;
	default:
		return iEntityWrapperLight::GetProperty(alPropID, asX);
	}

	return true;
}

//---------------------------------------------------------------------------

void cEntityWrapperLightArea::SetWidth(float afX)
{
	mfWidth = afX;

	((cLightArea*)mpEngineEntity->GetEntity())->SetWidth(mfWidth);
}

//---------------------------------------------------------------------------

void cEntityWrapperLightArea::SetHeight(float afX)
{
	mfHeight = afX;

	((cLightArea*)mpEngineEntity->GetEntity())->SetHeight(mfHeight);
}

//---------------------------------------------------------------------------

void cEntityWrapperLightArea::SetBarnDoorAngle(float afX)
{
	mfBarnDoorAngle = afX;

	((cLightArea*)mpEngineEntity->GetEntity())->SetBarnDoorAngle(mfBarnDoorAngle);
}

//---------------------------------------------------------------------------

void cEntityWrapperLightArea::SetBarnDoorLength(float afX)
{
	mfBarnDoorLength = afX;

	((cLightArea*)mpEngineEntity->GetEntity())->SetBarnDoorLength(mfBarnDoorLength);
}

//---------------------------------------------------------------------------

void cEntityWrapperLightArea::SetSourceTexture(const tString& asTexture)
{
	Image* pTex = NULL;
	if(asTexture != "" && cEditorHelper::LoadTextureResource(eEditorTextureResourceType_2D, asTexture, &pTex))
		msSourceTexture = cString::To8Char(GetEditorWorld()->GetEditor()->GetPathRelToWD(asTexture));
	else
		msSourceTexture = "";

	// Source texture lives in the base gobo slot — the renderer resolves GetGoboImage().
	((cLightArea*)mpEngineEntity->GetEntity())->SetGoboTexture(pTex);
}

//---------------------------------------------------------------------------

void cEntityWrapperLightArea::DrawLightTypeSpecific(cEditorWindowViewport* apViewport, DebugDraw* apFunctions,
													iEditorEditMode* apEditMode, bool abIsSelected)
{
	if(abIsSelected==false) return;

	// Rectangle outline + emission-normal stub, framed by the light's world axes.
	// UE Rect Light convention: width = local +Y, height = local +Z, emission = +X.
	cMatrixf mtx = ((cLightArea*)mpEngineEntity->GetEntity())->GetWorldMatrix();
	cVector3f vRight(mtx.m[0][1], mtx.m[1][1], mtx.m[2][1]);
	cVector3f vUp(mtx.m[0][2], mtx.m[1][2], mtx.m[2][2]);
	cVector3f vNormal(mtx.m[0][0], mtx.m[1][0], mtx.m[2][0]);

	cVector3f vHW = vRight * (mfWidth * 0.5f);
	cVector3f vHH = vUp * (mfHeight * 0.5f);

	cVector3f vC0 = mvPosition - vHW - vHH;
	cVector3f vC1 = mvPosition + vHW - vHH;
	cVector3f vC2 = mvPosition + vHW + vHH;
	cVector3f vC3 = mvPosition - vHW + vHH;

	apFunctions->DebugDrawLine(vC0, vC1, mcolDiffuseColor);
	apFunctions->DebugDrawLine(vC1, vC2, mcolDiffuseColor);
	apFunctions->DebugDrawLine(vC2, vC3, mcolDiffuseColor);
	apFunctions->DebugDrawLine(vC3, vC0, mcolDiffuseColor);
	apFunctions->DebugDrawLine(mvPosition, mvPosition + vNormal * 0.5f, mcolDiffuseColor);

	// Barn-door flaps: one per rect edge, hinged on the edge and tilted from the
	// emission normal by the barn-door angle θ, extending barnDoorLength L. Each
	// flap edge runs from a source-rect corner along dir = sinθ*outward + cosθ*normal
	// (matches barnAxis in ILight.slang), so the four flaps form the exit aperture.
	// L = 0 (or θ → 90°) means open flaps → nothing to show, like the shader no-op.
	if(mfBarnDoorLength > 0.0f)
	{
		const float sinA = std::sin(mfBarnDoorAngle);
		const float cosA = std::cos(mfBarnDoorAngle);
		const float L = mfBarnDoorLength;
		const cColor colBarn(1.0f, 0.75f, 0.2f, 1.0f); // amber — distinct from the rect

		auto drawFlap = [&](const cVector3f& cA, const cVector3f& cB, const cVector3f& vOutward)
		{
			cVector3f vDir = vOutward * sinA + vNormal * cosA;
			cVector3f tA = cA + vDir * L;
			cVector3f tB = cB + vDir * L;
			apFunctions->DebugDrawLine(cA, tA, colBarn);
			apFunctions->DebugDrawLine(cB, tB, colBarn);
			apFunctions->DebugDrawLine(tA, tB, colBarn);
		};

		drawFlap(vC1, vC2,  vRight); // +right edge
		drawFlap(vC0, vC3, vRight * -1.0f); // -right edge
		drawFlap(vC2, vC3,  vUp);    // +up edge
		drawFlap(vC0, vC1, vUp * -1.0f); // -up edge
	}
}

//---------------------------------------------------------------------------

/////////////////////////////////////////////////////////////////////////////
// AREALIGHT : PROTECTED METHODS
/////////////////////////////////////////////////////////////////////////////

//---------------------------------------------------------------------------

iEngineEntity* cEntityWrapperLightArea::CreateSpecificEngineEntity()
{
	return hplNew(cIconEntityLightArea,(this));
}

//---------------------------------------------------------------------------
