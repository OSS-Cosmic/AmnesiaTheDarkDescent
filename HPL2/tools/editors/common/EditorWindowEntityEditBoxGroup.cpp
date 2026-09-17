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

#include "EditorWindowEntityEditBoxGroup.h"

#include "EntityWrapper.h"
#include "EditorAction.h"
#include "EditorInput.h"

//---------------------------------------------------------------------------

/////////////////////////////////////////////////////////////////////////////
// CONSTRUCTORS
/////////////////////////////////////////////////////////////////////////////

//---------------------------------------------------------------------------

cEditorWindowEntityEditBoxGroup::cEditorWindowEntityEditBoxGroup(cEditorEditModeSelect* apEditMode, const tEntityWrapperList& alstEntities) : cEditorWindowEntityEditBox(apEditMode, NULL)
{
	mlstEntities = alstEntities;
}

cEditorWindowEntityEditBoxGroup::~cEditorWindowEntityEditBoxGroup()
{
}

//---------------------------------------------------------------------------

/////////////////////////////////////////////////////////////////////////////
// PUBLIC METHODS
/////////////////////////////////////////////////////////////////////////////

//---------------------------------------------------------------------------

void cEditorWindowEntityEditBoxGroup::Create()
{
	AddPropertyRendererMask(mpTabs->AddTab(_W("Renderer")));
}

//--------------------------------------------------------------------------

void cEditorWindowEntityEditBoxGroup::OnUpdate(float afTimeStep)
{
	if(mpInpRendererStandard==NULL) return;

	// A renderer is ticked when every selected object loads for it.
	unsigned lCommonMask = hpl::kRendererMaskAll;
	for(tEntityWrapperListIt it = mlstEntities.begin(); it != mlstEntities.end(); ++it)
		lCommonMask &= static_cast<unsigned>((*it)->GetRendererMask());

	mpInpRendererStandard->SetValue((lCommonMask & hpl::kRendererMaskStandard) != 0, false);
	mpInpRendererRayTraced->SetValue((lCommonMask & hpl::kRendererMaskRayTraced) != 0, false);
}

//--------------------------------------------------------------------------

bool cEditorWindowEntityEditBoxGroup::WindowSpecificInputCallback(iEditorInput* apInput)
{
	if(apInput!=mpInpRendererStandard && apInput!=mpInpRendererRayTraced)
		return false;

	// Change only the toggled renderer on each object, as one undo step.
	const unsigned lBit = apInput==mpInpRendererStandard ? hpl::kRendererMaskStandard : hpl::kRendererMaskRayTraced;
	const bool bEnabled = static_cast<cEditorInputBool*>(apInput)->GetValue();

	cEditorActionCompoundAction* pAction = hplNew(cEditorActionCompoundAction, ("Set Renderer"));
	for(tEntityWrapperListIt it = mlstEntities.begin(); it != mlstEntities.end(); ++it)
	{
		iEntityWrapper* pEnt = *it;
		unsigned lRendererMask = static_cast<unsigned>(pEnt->GetRendererMask());
		lRendererMask = bEnabled ? (lRendererMask | lBit) : (lRendererMask & ~lBit);
		pAction->AddAction(pEnt->CreateSetPropertyActionInt(eObjInt_RendererMask, static_cast<int>(lRendererMask)));
	}

	if(pAction->IsEmpty())
	{
		hplDelete(pAction);
		return true;
	}

	mpEditor->AddAction(pAction);
	return true;
}

//--------------------------------------------------------------------------

/////////////////////////////////////////////////////////////////////////////
// PROTECTED METHODS
/////////////////////////////////////////////////////////////////////////////

//--------------------------------------------------------------------------

//--------------------------------------------------------------------------
