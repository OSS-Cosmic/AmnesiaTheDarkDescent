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

#include "EditorWindowLights.h"
#include "EditorEditModeLights.h"

#include "EntityWrapperLight.h"
#include "EditorBaseClasses.h"
#include "EditorInput.h"


//---------------------------------------------------------------------------

/////////////////////////////////////////////////////////////////////////////
// CONSTRUCTORS
/////////////////////////////////////////////////////////////////////////////

//---------------------------------------------------------------------------

cEditorWindowLights::cEditorWindowLights(cEditorEditModeLights* apEditMode) : iEditModeObjectCreatorWindow(apEditMode)
{
	mpInpShowLegacyLights = NULL;
	mpInpShowOverdriveLights = NULL;
}

//---------------------------------------------------------------------------

/////////////////////////////////////////////////////////////////////////////
// PROTECTED METHODS
/////////////////////////////////////////////////////////////////////////////

//---------------------------------------------------------------------------

bool cEditorWindowLights::WindowSpecificInputCallback(iEditorInput* apInput)
{
	if(apInput==mpInpLightType)
	{
		cEditorEditModeLights* pEditMode = (cEditorEditModeLights*)mpEditMode;
		pEditMode->SetSubType(mpInpLightType->GetValue());
	}
	else if(apInput==mpInpShowLegacyLights)
		mpEditor->SetVisibilityTypeState(eEditorVisibilityType_LegacyLights, mpInpShowLegacyLights->GetValue());
	else if(apInput==mpInpShowOverdriveLights)
		mpEditor->SetVisibilityTypeState(eEditorVisibilityType_OverdriveLights, mpInpShowOverdriveLights->GetValue());
	

	return true;
}



//---------------------------------------------------------------------------

void cEditorWindowLights::OnInitLayout()
{	
	mpBGFrame->SetSize(cVector2f(200,120));
	mpBGFrame->SetClipActive(false);

	mpSelectionGroup = mpSet->CreateWidgetGroup(cVector3f(5,8,0.1f), cVector2f(190,50), _W("Light Type"), mpBGFrame);

	cEditorEditModeLights* pEditMode = (cEditorEditModeLights*)mpEditMode;
	mpInpLightType = CreateInputEnum(cVector3f(10,10,0.1f), _W("Light Type"), "", tWStringList(), mpSelectionGroup);
	for(int i=0;i<pEditMode->GetTypeNum();++i)
	{
		iEntityWrapperTypeLight* pType = (iEntityWrapperTypeLight*)pEditMode->GetType(i);
		mpInpLightType->AddValue(cString::To16Char(pType->GetXmlElementName()));
	}

	mpInpLightType->SetValue(0);

	// Both light sets stay editable in either renderer; these hide one set's
	// handles and lights.
	cVector3f vPos(10, 65, 0.1f);
	mpInpShowLegacyLights = CreateInputBool(vPos, _W("Show Legacy lights"), "", mpBGFrame);
	mpInpShowLegacyLights->SetValue(mpEditor->GetVisibilityTypeState(eEditorVisibilityType_LegacyLights), false);
	vPos.y += mpInpShowLegacyLights->GetSize().y + 5;
	mpInpShowOverdriveLights = CreateInputBool(vPos, _W("Show Overdrive lights"), "", mpBGFrame);
	mpInpShowOverdriveLights->SetValue(mpEditor->GetVisibilityTypeState(eEditorVisibilityType_OverdriveLights), false);
}

//---------------------------------------------------------------------------
