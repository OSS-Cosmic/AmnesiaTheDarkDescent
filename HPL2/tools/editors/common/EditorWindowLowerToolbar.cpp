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

#include "EditorWindowLowerToolbar.h"

#include "EditorBaseClasses.h"
#include "EditorInput.h"

#include "../common/EditorActionHandler.h"

#include "../common/EditorWindowViewport.h"
#include "../common/EditorSelection.h"
#include "../common/EditorGrid.h"
#include "../common/EditorClipPlane.h"
#include "../common/EditorWorld.h"
#include "../common/EditorActionMisc.h"

#include "graphics/HybridRenderer.h"

//---------------------------------------------------------------

//////////////////////////////////////////////////////////////////
// CONSTRUCTORS
//////////////////////////////////////////////////////////////////

//---------------------------------------------------------------

iEditorWindowLowerToolbar::iEditorWindowLowerToolbar(iEditorBase* apEditor) : iEditorWindow(apEditor, "Lower Toolbar" )
{	
	mpGridControlsGroup = NULL;
	mpBPlaneSwitch = NULL;
	mpBSnap = NULL;
	mpGridControlsGroup = NULL;
	mpInpPlaneHeight = NULL;
	mpInpSnapSep = NULL;
	
	mpHandleViewportControls = NULL;
	mpBEnlargeViewport = NULL;

	mpHandleLighting =NULL;
	mpBGlobalAmbientLight = NULL;
	mpBGlobalPointLight = NULL;
	mpBIconsEnabled = NULL;

	mpHandleVisibility = NULL;
	mpBAreas = NULL;
	mpBBlockers = NULL;
	mpBFog = NULL;

	mpDebugControlsGroup = NULL;
	mpBShadowFlag = NULL;

	mpHandleCamera = NULL;
	mpBCameraLockToGrid = NULL;

	mpGClipPlanes = NULL;

	mbClipPlaneAdded = false;
	mbClipPlaneRemoved = false;
	mbClipPlaneVecUpdated = false;
}

//---------------------------------------------------------------

iEditorWindowLowerToolbar::~iEditorWindowLowerToolbar()
{
}

//---------------------------------------------------------------

//////////////////////////////////////////////////////////////////
// PUBLIC METHODS
//////////////////////////////////////////////////////////////////

//---------------------------------------------------------------

void iEditorWindowLowerToolbar::OnInitLayout()
{
	//////////////////////////////
	// Set up layout
	SetSize(cVector2f(mpEditor->GetLayoutVec2f(eLayoutVec2_ViewportAreaSize).x-4,50));
}

//---------------------------------------------------------------

//////////////////////////////////////////////////////////////////
// PROTECTED METHODS
//////////////////////////////////////////////////////////////////

//---------------------------------------------------------------

iWidget* iEditorWindowLowerToolbar::AddGridControls()
{
	// Grid Controls group
	mpGridControlsGroup = mpSet->CreateWidgetGroup(0,cVector2f(200,43),_W("Grid Controls"), mpBGFrame);
	mpGridControlsGroup->SetDefaultFontSize(iEditorInput::GetFontSize());

	// Buttons

	// Plane switch
	mpBPlaneSwitch = mpSet->CreateWidgetButton(cVector3f(5,10,0.1f), 25, _W(""), mpGridControlsGroup);
	mpBPlaneSwitch->AddCallback(eGuiMessage_ButtonPressed, this, kGuiCallback(InputCallback));
	mpBPlaneSwitch->SetToolTip(_W("Cycle through grid planes"));
	mpBPlaneSwitch->SetToolTipEnabled(true);

	////////////////////
	// Snap controls
	// Active
	cGuiGfxElement* pImg = mpSet->GetGui()->CreateGfxImage("button_snap.tga", eGuiMaterial_Alpha);
	mpBSnap = mpSet->CreateWidgetButton(cVector3f(33, 10, 0.1f), 25, _W(""), mpGridControlsGroup);
	mpBSnap->SetToggleable(true);
	mpBSnap->SetImage(pImg);
	mpBSnap->AddCallback(eGuiMessage_ButtonPressed, this, kGuiCallback(InputCallback));
	mpBSnap->SetToolTip(_W("Toggle grid snapping"));
	mpBSnap->AddShortcut(eKeyModifier_None, eKey_X); //Hpl3
	mpBSnap->AddShortcut(eKeyModifier_None, eKey_R); //Legacy
	mpBSnap->SetToolTipEnabled(true);

	// Height and sep.
	mpInpPlaneHeight = CreateInputNumber(cVector3f(65,5,0.1f), _W("Height"), "", mpGridControlsGroup, 50, 0.25f);
	mpInpSnapSep = CreateInputNumber(cVector3f(120,5,0.1f), _W("Snap sep."), "", mpGridControlsGroup, 50, 0.25f); 

	return mpGridControlsGroup;
}

iWidget* iEditorWindowLowerToolbar::AddLightingControls()
{
	mpHandleLighting = mpSet->CreateWidgetDummy(0, mpBGFrame);

	mpBGlobalAmbientLight = mpSet->CreateWidgetButton(cVector3f(0,0,0.1f), 19, _W("A"), mpHandleLighting);
	mpBGlobalAmbientLight->SetToolTip(_W("Toggle global ambient light"));
	mpBGlobalAmbientLight->SetToolTipEnabled(true);
	mpBGlobalAmbientLight->SetToggleable(true);
	mpBGlobalAmbientLight->AddCallback(eGuiMessage_ButtonPressed, this, kGuiCallback(InputCallback));
	mpBGlobalAmbientLight->AddShortcut(eKeyModifier_None, eKey_A);

	mpBGlobalPointLight = mpSet->CreateWidgetButton(cVector3f(0,21,0.1f), 19, _W("P"), mpHandleLighting);
	mpBGlobalPointLight->SetToolTip(_W("Toggle global point light"));
	mpBGlobalPointLight->SetToolTipEnabled(true);
	mpBGlobalPointLight->SetToggleable(true);
	mpBGlobalPointLight->AddCallback(eGuiMessage_ButtonPressed, this, kGuiCallback(InputCallback));
	mpBGlobalPointLight->AddShortcut(eKeyModifier_None, eKey_P);

	mpBIconsEnabled = mpSet->CreateWidgetButton(cVector3f(21, 0, 0.1f), 19, _W("I"), mpHandleLighting);
	mpBIconsEnabled->SetToggleable(true);
	mpBIconsEnabled->AddCallback(eGuiMessage_ButtonPressed, this, kGuiCallback(InputCallback));
	mpBIconsEnabled->SetToolTip(_W("Toggle icon rendering"));
	mpBIconsEnabled->AddShortcut(eKeyModifier_None, eKey_I);
	mpBIconsEnabled->SetToolTipEnabled(true);

	return mpHandleLighting;
}

iWidget* iEditorWindowLowerToolbar::AddVisibilityControls()
{
	mpHandleVisibility = mpSet->CreateWidgetDummy(0, mpBGFrame);

	mpBAreas = mpSet->CreateWidgetButton(cVector3f(0, 0, 0.1f), cVector2f(50,20), _W("Areas"), mpHandleVisibility);
	mpBAreas->SetDefaultFontSize(11);
	mpBAreas->AddCallback(eGuiMessage_ButtonPressed, this, kGuiCallback(InputCallback));
	mpBAreas->SetToolTip(_W("Toggle Area gizmo rendering"));
	mpBAreas->SetToolTipEnabled(true);
	mpBAreas->SetToggleable(true);

	mpBBlockers = mpSet->CreateWidgetButton(cVector3f(0, 23, 0.1f), cVector2f(50, 20), _W("Blockers"), mpHandleVisibility);
	mpBBlockers->SetDefaultFontSize(11);
	mpBBlockers->AddCallback(eGuiMessage_ButtonPressed, this, kGuiCallback(InputCallback));
	mpBBlockers->SetToolTip(_W("Toggle Blocker rendering"));
	mpBBlockers->SetToolTipEnabled(true);
	mpBBlockers->SetToggleable(true);

	mpBFog = mpSet->CreateWidgetButton(cVector3f(53, 0, 0.1f), cVector2f(50, 20), _W("Fog"), mpHandleVisibility);
	mpBFog->SetDefaultFontSize(11);
	mpBFog->AddCallback(eGuiMessage_ButtonPressed, this, kGuiCallback(InputCallback));
	mpBFog->SetToolTip(_W("Toggle Global fog rendering"));
	mpBFog->SetToolTipEnabled(true);
	mpBFog->SetToggleable(true);

	return mpHandleVisibility;
}

iWidget* iEditorWindowLowerToolbar::AddDebugControls()
{
	// Debug Controls group
	mpDebugControlsGroup = mpSet->CreateWidgetGroup(0, cVector2f(120, 43), _W("Debug Controls"), mpBGFrame);
	mpDebugControlsGroup->SetDefaultFontSize(iEditorInput::GetFontSize());

	// Shadow Flag
	mpBShadowFlag = mpSet->CreateWidgetButton(cVector3f(15, 9, 0.1f), cVector2f(85, 25), _W("Shadow Flag"), mpDebugControlsGroup);
	mpBShadowFlag->SetDefaultFontSize(13);
	mpBShadowFlag->AddCallback(eGuiMessage_ButtonPressed, this, kGuiCallback(InputCallback));
	mpBShadowFlag->SetToolTip(_W("Toggle debug overlay for effective shadow casting: green = \"Cast shadows\" flag set, yellow = casting only because all lights cast shadows, red = not blocking shadow rays"));
	mpBShadowFlag->SetToolTipEnabled(true);
	mpBShadowFlag->SetToggleable(true);

	return mpDebugControlsGroup;
}

iWidget* iEditorWindowLowerToolbar::AddViewportControls()
{
	mpHandleViewportControls = mpSet->CreateWidgetDummy(0, mpBGFrame);

	cGuiGfxElement* pImg = mpSet->GetGui()->CreateGfxImage("button_enlarge.tga", eGuiMaterial_Alpha);
	mpBEnlargeViewport = mpSet->CreateWidgetButton(0, 40, _W(""), mpHandleViewportControls);
	mpBEnlargeViewport->SetToggleable(true);
	mpBEnlargeViewport->SetImage(pImg);
	mpBEnlargeViewport->AddCallback(eGuiMessage_ButtonPressed,this, kGuiCallback(InputCallback));
	mpBEnlargeViewport->SetToolTip(_W("Enlarge current viewport"));
	mpBEnlargeViewport->SetToolTipEnabled(true);

	mpSet->AddGlobalShortcut(0, eKey_Space, mpBEnlargeViewport, eGuiMessage_ButtonPressed);
	
	return mpHandleViewportControls;
}

iWidget* iEditorWindowLowerToolbar::AddCameraControls()
{
	mpHandleCamera = mpSet->CreateWidgetDummy(0, mpBGFrame);

	mpBCameraLockToGrid = mpSet->CreateWidgetButton(0, 19, _W("LT"), mpHandleCamera);
	mpBCameraLockToGrid->AddCallback(eGuiMessage_ButtonPressed, this, kGuiCallback(InputCallback));
	mpBCameraLockToGrid->SetToggleable(true);
	mpBCameraLockToGrid->SetToolTip(_W("Lock camera tracking to grid"));
	mpBCameraLockToGrid->SetToolTipEnabled(true);

	mpBCameraFocusOnSelection = mpSet->CreateWidgetButton(cVector3f(0,21,0), 19, _W("F"), mpHandleCamera);
	mpBCameraFocusOnSelection->AddCallback(eGuiMessage_ButtonPressed, this, kGuiCallback(InputCallback));
	mpBCameraFocusOnSelection->SetToolTip(_W("Focus on selection"));
	mpBCameraFocusOnSelection->SetToolTipEnabled(true);
	mpSet->AddGlobalShortcut(0, eKey_F, mpBCameraFocusOnSelection, eGuiMessage_ButtonPressed);

	return mpHandleCamera;
}

iWidget* iEditorWindowLowerToolbar::AddClipPlaneControls()
{
	mbClipPlaneVecUpdated = true;

	mpGClipPlanes = mpSet->CreateWidgetGroup(0, cVector2f(275,43), _W("Clip planes"), mpBGFrame);
	mpGClipPlanes->SetDefaultFontSize(iEditorInput::GetFontSize());

	mpInpClipPlanes = CreateInputEnum(cVector3f(5,0,0.1f), _W(""), "", tWStringList(), mpGClipPlanes,50);
	mpInpClipPlanes->SetMaxShownItems(3);
	mpBAddClipPlane = mpSet->CreateWidgetButton(cVector3f(60, 5, 0.1f), cVector2f(15), _W("+"), mpGClipPlanes);

	mpBAddClipPlane->AddCallback(eGuiMessage_ButtonPressed, this, kGuiCallback(InputCallback));
	mpBRemClipPlane = mpSet->CreateWidgetButton(cVector3f(60, 21, 0.1f), cVector2f(15), _W("-"), mpGClipPlanes);
	mpBRemClipPlane->AddCallback(eGuiMessage_ButtonPressed, this, kGuiCallback(InputCallback));

	// Buttons
	mpBClipNormalSwitch = mpSet->CreateWidgetButton(cVector3f(100,10,0.1f), 25, _W(""), mpGClipPlanes);
	mpBClipNormalSwitch->AddCallback(eGuiMessage_ButtonPressed, this, kGuiCallback(InputCallback));
	mpBClipNormalSwitch->SetToolTip(_W("Cycle through clip plane alignments"));
	mpBClipNormalSwitch->SetToolTipEnabled(true);

	mpInpClipPlaneHeight = CreateInputNumber(cVector3f(130,5,0.1f), _W("Height"), "", mpGClipPlanes, 50, 0.25f);
	mpSet->AddGlobalShortcut(0, eKey_O, mpInpClipPlaneHeight->GetInputWidget(), eGuiMessage_TextBoxValueUp);
	mpSet->AddGlobalShortcut(0, eKey_L, mpInpClipPlaneHeight->GetInputWidget(), eGuiMessage_TextBoxValueDown);

	mpBClipPlaneCullSide = mpSet->CreateWidgetButton(cVector3f(185,5,0.1f), 25, _W(""), mpGClipPlanes);
	mpBClipPlaneCullSide->AddCallback(eGuiMessage_ButtonPressed, this, kGuiCallback(InputCallback));
	mpBClipPlaneCullSide->SetToolTip(_W("Set culled plane side"));
	mpBClipPlaneCullSide->SetToolTipEnabled(true);

	mpInpClipPlaneActive = CreateInputBool(cVector3f(215,5,0.1f), _W("Active"), "", mpGClipPlanes);

	return mpGClipPlanes;
}

//---------------------------------------------------------------

void iEditorWindowLowerToolbar::SetFocusedClipPlane(int alX)
{
	if(mpInpClipPlanes)
		mpInpClipPlanes->SetValue(alX);
}

//---------------------------------------------------------------

int iEditorWindowLowerToolbar::GetFocusedClipPlane()
{
	if(mpInpClipPlanes)
		return mpInpClipPlanes->GetValue();

	return -1;
}

//---------------------------------------------------------------

//---------------------------------------------------------------

void iEditorWindowLowerToolbar::OnUpdate(float afTimeStep)
{
	iEditorWorld* pWorld = mpEditor->GetEditorWorld();
	cEditorWindowViewport* pViewport = mpEditor->GetFocusedViewport();
	cEditorGrid* pGrid = pViewport->GetGrid();

	bool bCameraIsOrtho = pViewport->GetVCamera()->IsOrtho();
    
	float fHeight = pGrid->GetHeight();
	float fSnapSeparation = pGrid->GetSnapSeparation();

	if(mpGridControlsGroup)
	{
		mpBPlaneSwitch->SetText(pGrid->GetPlaneString());
		mpBPlaneSwitch->SetEnabled(bCameraIsOrtho==false);

		mpInpPlaneHeight->SetValue(fHeight, false);
		mpInpSnapSep->SetValue(fSnapSeparation, false);
		mpBSnap->SetPressed(pGrid->GetSnapToGrid(), false);
	}

	if(mpHandleLighting)
	{
		mpBGlobalAmbientLight->SetPressed(pWorld->GetGlobalAmbientLightEnabled(), false);
		mpBGlobalPointLight->SetPressed(pWorld->GetGlobalPointLightEnabled(), false);
		mpBIconsEnabled->SetPressed(mpEditor->GetVisibilityTypeState(eEditorVisibilityType_Icons), false);
	}

	if(mpHandleVisibility)
	{
		mpBAreas->SetPressed(mpEditor->GetVisibilityTypeState(eEditorVisibilityType_Areas), false);
		mpBBlockers->SetPressed(mpEditor->GetVisibilityTypeState(eEditorVisibilityType_Blockers), false);
		mpBFog->SetPressed(mpEditor->GetVisibilityTypeState(eEditorVisibilityType_GlobalFog), false);
	}

	if(mpHandleViewportControls)
	{
		mpBEnlargeViewport->SetPressed(mpEditor->GetFlags(eEditorFlag_ViewportEnlarged), false);
	}

	if(mpBShadowFlag)
	{
		// The overlay is a ray-traced renderer feature. The backend can change
		// at runtime (View > Render mode), so re-gate every frame and re-push
		// the pressed state when it comes back.
		const bool bRayTraced = mpEditor->GetLitRendererBackend()!=eRendererBackend_Standard;
		if(bRayTraced && mpBShadowFlag->IsEnabled()==false)
			ApplyShadowFlagOverlay();
		mpBShadowFlag->SetEnabled(bRayTraced);
	}

	if(mpHandleCamera)
	{
		mpBCameraLockToGrid->SetEnabled(bCameraIsOrtho==false);
		mpBCameraLockToGrid->SetPressed(mpEditor->GetFocusedViewport()->GetVCamera()->IsLockedToGrid(), false);
	}

	if(mpGClipPlanes)
	{
		int lPrevIndex = mpInpClipPlanes->GetValue();
		tEditorClipPlaneVec& vClipPlanes = pWorld->GetClipPlanes();
		bool bClipPlaneNumUpdated = pWorld->ClipPlaneNumUpdated();
		if(bClipPlaneNumUpdated)
		{
			mpInpClipPlanes->ClearValues();
			for(int i=0;i<(int)vClipPlanes.size();++i)
				mpInpClipPlanes->AddValue(cString::ToStringW(i+1));

			int lIndex = -1;

			if(mbClipPlaneAdded)
			{
				mbClipPlaneAdded = false;
				lIndex = (int)vClipPlanes.size()-1;
			}
			if(mbClipPlaneRemoved)
			{
				mbClipPlaneRemoved = false;
				lIndex = lPrevIndex-1;
				if(lIndex<0 && vClipPlanes.empty()==false)
					lIndex=0;
			}

			mpInpClipPlanes->SetValue(lIndex, false);
		}
		UpdateClipPlaneControls();
	}
}

//---------------------------------------------------------------

bool iEditorWindowLowerToolbar::WindowSpecificInputCallback(iEditorInput* apInput)
{
	iEditorWorld* pWorld = mpEditor->GetEditorWorld();

	cEditorGrid* pGrid = mpEditor->GetFocusedViewport()->GetGrid();
	ePlaneNormal plane = pGrid->GetPlaneNormal();
	iEditorAction* pAction = NULL;

	int lClipPlaneIdx = -1;
	if(mpInpClipPlanes) lClipPlaneIdx = mpInpClipPlanes->GetValue();
	cEditorClipPlane* pClipPlane = pWorld->GetClipPlane(lClipPlaneIdx);

	if(apInput==mpInpPlaneHeight)
	{
		cVector3f vHeight = pGrid->GetHeights();
		vHeight.v[pGrid->GetPlaneNormal()] = mpInpPlaneHeight->GetValue();
		pAction = hplNew(cEditorActionPlaneChangeHeight,(pGrid, vHeight));
	}
	else if(apInput==mpInpSnapSep)
	{
		pAction = hplNew(cEditorActionGridChangeSnapSeparation,(pGrid, mpInpSnapSep->GetValue()));
	}
	else if(apInput==mpInpClipPlanes)
	{
		UpdateClipPlaneControls();
	}
	else if(apInput==mpInpClipPlaneHeight)
	{
		if(pClipPlane)
		{
			cVector3f vHeight = pClipPlane->GetHeights();
			vHeight.v[pClipPlane->GetPlaneNormal()] = mpInpClipPlaneHeight->GetValue();
			
			pAction = hplNew(cEditorActionClipPlaneSetHeight,(pWorld, lClipPlaneIdx, vHeight));
		}
	}
	else if(apInput==mpInpClipPlaneActive)
	{
		if(pClipPlane)
		{
			pAction = hplNew(cEditorActionClipPlaneSetActive,(pWorld, lClipPlaneIdx, mpInpClipPlaneActive->GetValue()));
		}
	}

	mpEditor->AddAction(pAction);

	return pAction!=NULL;
}

void iEditorWindowLowerToolbar::ApplyShadowFlagOverlay()
{
	if(mpBShadowFlag==NULL)
		return;

	// NULL under Standard, which has no such overlay.
	cHybridRenderer* pHybridRenderer = dynamic_cast<cHybridRenderer*>(
		mpEditor->GetEngine()->GetGraphics()->GetRenderer(eRenderer_Main));
	if(pHybridRenderer)
	{
		pHybridRenderer->SetOverlay(mpBShadowFlag->IsPressed()
			? (int)kOverlayModeShadowFlag
			: (int)kOverlayModeIndirectLighting);
	}
}

//----------------------------------------------------------------------

bool iEditorWindowLowerToolbar::InputCallback(iWidget* apWidget, const cGuiMessageData& aData)
{
	iEditorWorld* pWorld = mpEditor->GetEditorWorld();

	cEditorGrid* pGrid = mpEditor->GetFocusedViewport()->GetGrid();
	float fSnap = pGrid->GetSnapSeparation();
	float fHeight = pGrid->GetHeight();

	int lClipPlaneIdx = -1;
	cEditorClipPlane* pClipPlane = NULL;
	if(mpGClipPlanes)
	{
		lClipPlaneIdx = mpInpClipPlanes->GetValue();
		pClipPlane = pWorld->GetClipPlane(lClipPlaneIdx);
	}

	iEditorAction* pAction = NULL;

	///////////////////////////
	// Enlarge Viewport
	if(apWidget==mpBEnlargeViewport)
	{
		bool bEnlarged = mpEditor->GetFlags(eEditorFlag_ViewportEnlarged);
		mpEditor->SetFlags(eEditorFlag_ViewportEnlarged, !bEnlarged);
	}
	///////////////////////////
	// GridPlane switch button
	else if(apWidget==mpBPlaneSwitch)
	{
		int currentPlane = (int)pGrid->GetPlaneNormal();
		currentPlane = ((currentPlane+1) %(int)ePlaneNormal_LastEnum);

		pAction = hplNew(cEditorActionPlaneChangeNormal,(pGrid,(ePlaneNormal) currentPlane));
	}
	///////////////////////////
	// Grid snap button
	else if(apWidget==mpBSnap)
	{
		// Toggle from the grid's current state, not the button's — a keyboard
		// shortcut (X/R) fires InputCallback without flipping the toggle button,
		// so reading IsPressed() would no-op or invert on shortcut use.
		bool bSnap = pGrid->GetSnapToGrid();
		pGrid->SetSnapToGrid(!bSnap);
		mpBSnap->SetPressed(!bSnap, false);
	}
	///////////////////////////
	// Ambient light button
	else if(apWidget==mpBGlobalAmbientLight)
		mpEditor->GetEditorWorld()->SetGlobalAmbientLightEnabled(mpBGlobalAmbientLight->IsPressed());
	///////////////////////////
	// Point light button
	else if(apWidget==mpBGlobalPointLight)
		mpEditor->GetEditorWorld()->SetGlobalPointLightEnabled(mpBGlobalPointLight->IsPressed());
	///////////////////////////
	// Visibility toggles — read current state (the I shortcut fires this
	// callback without flipping the toggle button) and write the inverse.
	else if(apWidget==mpBIconsEnabled)
	{
		bool bEnabled = mpEditor->GetVisibilityTypeState(eEditorVisibilityType_Icons);
		mpEditor->SetVisibilityTypeState(eEditorVisibilityType_Icons, !bEnabled);
		mpBIconsEnabled->SetPressed(!bEnabled, false);
	}
	else if(apWidget==mpBAreas)
	{
		bool bEnabled = mpEditor->GetVisibilityTypeState(eEditorVisibilityType_Areas);
		mpEditor->SetVisibilityTypeState(eEditorVisibilityType_Areas, !bEnabled);
		mpBAreas->SetPressed(!bEnabled, false);
	}
	else if(apWidget==mpBBlockers)
	{
		bool bEnabled = mpEditor->GetVisibilityTypeState(eEditorVisibilityType_Blockers);
		mpEditor->SetVisibilityTypeState(eEditorVisibilityType_Blockers, !bEnabled);
		mpBBlockers->SetPressed(!bEnabled, false);
	}
	else if(apWidget==mpBFog)
	{
		bool bEnabled = mpEditor->GetVisibilityTypeState(eEditorVisibilityType_GlobalFog);
		mpEditor->SetVisibilityTypeState(eEditorVisibilityType_GlobalFog, !bEnabled);
		mpBFog->SetPressed(!bEnabled, false);
		// Fog reads the visibility flag through SetFogActive (not UpdateVisibility),
		// so re-apply it now to take effect immediately.
		mpEditor->GetEditorWorld()->SetFogActive(mpEditor->GetEditorWorld()->GetFogActive());
	}
	else if(apWidget==mpBShadowFlag)
	{
		ApplyShadowFlagOverlay();
	}
	///////////////////////////
	// Camera lock to grid button
	else if(apWidget==mpBCameraLockToGrid)
	{
		mpEditor->GetFocusedViewport()->GetVCamera()->LockToGrid(mpBCameraLockToGrid->IsPressed());
	}
	else if(apWidget==mpBCameraFocusOnSelection)
	{
		pAction = mpEditor->CreateFocusOnSelectionAction();
	}
	else if(apWidget==mpBAddClipPlane)
	{
		mbClipPlaneAdded = true;
		pAction = hplNew(cEditorActionClipPlaneAdd,(mpEditor->GetEditorWorld()));
	}
	else if(apWidget==mpBRemClipPlane)
	{
		mbClipPlaneRemoved = true;
		if(lClipPlaneIdx!=-1)
		{
			mbClipPlaneVecUpdated = true;
			pAction = hplNew(cEditorActionClipPlaneRemove,(pWorld, lClipPlaneIdx));
		}
	}

	else if(apWidget==mpBClipNormalSwitch)
	{
		if(pClipPlane)
		{
			int currentPlane = (int)pClipPlane->GetPlaneNormal();
			currentPlane = ((currentPlane+1) %(int)ePlaneNormal_LastEnum);

			pAction = hplNew(cEditorActionClipPlaneSetNormal,(pWorld, lClipPlaneIdx, currentPlane));
		}
	}
	else if(apWidget==mpBClipPlaneCullSide)
	{
		if(pClipPlane)
		{
			pAction = hplNew(cEditorActionClipPlaneSetCullingOnPositiveSide,(pWorld, lClipPlaneIdx, !pClipPlane->GetCullingOnPositiveSide()));
		}
	}
	
	mpEditor->AddAction(pAction);

	return true;
}
kGuiCallbackDeclaredFuncEnd(iEditorWindowLowerToolbar,InputCallback);

//---------------------------------------------------------------

void iEditorWindowLowerToolbar::UpdateClipPlaneControls()
{
	if(mpGClipPlanes)
	{
		int lPlaneIdx = mpInpClipPlanes->GetValue();
		iEditorWorld* pWorld = mpEditor->GetEditorWorld();
	
		cEditorClipPlane* pPlane = pWorld->GetClipPlane(lPlaneIdx);
		bool bEnabled = (pPlane!=NULL);

		mpInpClipPlaneHeight->GetHandle()->SetEnabled(bEnabled);
		mpBClipNormalSwitch->SetEnabled(bEnabled);
		mpBClipPlaneCullSide->SetEnabled(bEnabled);
		mpInpClipPlaneActive->GetHandle()->SetEnabled(bEnabled);

        if(pPlane)
		{
			mpInpClipPlaneHeight->SetValue(pPlane->GetHeight(),false);
			mpBClipNormalSwitch->SetText(pPlane->GetPlaneString());
			mpBClipPlaneCullSide->SetText(pPlane->GetCullingOnPositiveSide()?_W("Pos"):_W("Neg"));
			mpInpClipPlaneActive->SetValue(pPlane->IsActive(), false);
		}
	}

}

//---------------------------------------------------------------

//---------------------------------------------------------------
