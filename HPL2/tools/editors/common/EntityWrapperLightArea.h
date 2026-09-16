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

#ifndef HPLEDITOR_ENTITY_WRAPPER_LIGHT_AREA_H
#define HPLEDITOR_ENTITY_WRAPPER_LIGHT_AREA_H

#include "EntityWrapperLight.h"

class cEditorWindowViewport;

//---------------------------------------------------------------------------

class cIconEntityLightArea : public iIconEntityLight
{
public:
	cIconEntityLightArea(iEntityWrapper* apParent);

	bool Create(const tString& asName);
};

//---------------------------------------------------------------------------

#define LightAreaPropIdStart 100

enum eLightAreaFloat
{
	eLightAreaFloat_SourceWidth = LightAreaPropIdStart,
	eLightAreaFloat_SourceHeight,
	eLightAreaFloat_BarnDoorAngle,
	eLightAreaFloat_BarnDoorLength,

	eLightAreaFloat_LastEnum,
};

enum eLightAreaStr
{
	eLightAreaStr_SourceTexture = LightAreaPropIdStart,

	eLightAreaStr_LastEnum,
};

//---------------------------------------------------------------------------

class cEntityWrapperTypeLightArea : public iEntityWrapperTypeLight
{
public:
	cEntityWrapperTypeLightArea();

	// AreaLight is the pre-split spelling of Re_AreaLight.
	bool IsAppropriateType(tinyxml2::XMLElement* apElement);

protected:
	iEntityWrapperData* CreateSpecificData();
};

//---------------------------------------------------------------------------

class cEntityWrapperDataLightArea : public iEntityWrapperDataLight
{
public:
	cEntityWrapperDataLightArea(iEntityWrapperType*);

protected:
	iEntityWrapper* CreateSpecificEntity();
};

//---------------------------------------------------------------------------

class cEntityWrapperLightArea : public iEntityWrapperLight
{
public:
	cEntityWrapperLightArea(iEntityWrapperData*);
	~cEntityWrapperLightArea();

	bool SetProperty(int, const float&);
	bool SetProperty(int, const tString&);
	bool GetProperty(int, float&);
	bool GetProperty(int, tString&);

	void SetWidth(float afX);
	inline float GetWidth() const { return mfWidth; }

	void SetHeight(float afX);
	inline float GetHeight() const { return mfHeight; }

	void SetBarnDoorAngle(float afX);
	inline float GetBarnDoorAngle() const { return mfBarnDoorAngle; }

	void SetBarnDoorLength(float afX);
	inline float GetBarnDoorLength() const { return mfBarnDoorLength; }

	tString& GetSourceTexture() { return msSourceTexture; }
	void SetSourceTexture(const tString& asTexture);

	void DrawLightTypeSpecific(cEditorWindowViewport* apViewport, DebugDraw* apFunctions, iEditorEditMode* apEditMode, bool abIsSelected);

protected:
	iEngineEntity* CreateSpecificEngineEntity();

	//////////////////////
	// Data
	tString msSourceTexture;
	float mfWidth;
	float mfHeight;
	float mfBarnDoorAngle;
	float mfBarnDoorLength;
};

//---------------------------------------------------------------------------

#endif // HPLEDITOR_ENTITY_WRAPPER_LIGHT_AREA_H
