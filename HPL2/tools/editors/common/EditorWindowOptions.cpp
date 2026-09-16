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

#include "EditorWindowOptions.h"
#include "EditorWorld.h"
#include "EditorWindowViewport.h"
#include "EditorActionHandler.h"
#include "EditorSelection.h"
#include "EngineEntity.h"

#include "graphics/Graphics.h"
#include "system/Platform.h"        // cPlatform::CopyTextToClipboard
#include "gui/WidgetTabFrame.h"
#include "gui/WidgetLabel.h"
#include "gui/WidgetButton.h"
#include "gui/WidgetTextBox.h"

//-----------------------------------------------------------------

cEditorWindowOptions::cEditorWindowOptions(iEditorBase* apEditor) : iEditorWindowPopUp(apEditor, "Options Window", true, false, false, cVector2f(640,360))
{
}

//-----------------------------------------------------------------

void cEditorWindowOptions::OnInitLayout()
{
	iEditorWindowPopUp::OnInitLayout();

	mpWindow->SetText(_W("Options"));

	cWidgetTabFrame* pFrame = mpSet->CreateWidgetTabFrame(cVector3f(10,35,0.1f), mpWindow->GetSize()-cVector2f(20,45), _W(""), mpWindow);
	cWidgetTab* pTab = NULL;

	cVector3f vPos;

	/////////////////////////////////////////////////////////
	// General options
	{
		tWStringList lstLabels;
		lstLabels.push_back(_W("X"));
		lstLabels.push_back(_W("Y"));

		pTab = pFrame->AddTab(_W("General"));
		vPos = cVector3f(15,15,0.1f);

		// Resolution
		mpInpResolution = CreateInputVec2(vPos-cVector3f(0,3,0), _W("Resolution"), "", pTab, 50,lstLabels, eEditorInputLayoutStyle_RowLabelOnLeft);
		mpInpResolution->SetLowerBound(true, 640);

		// Undo stack
		vPos.x += mpInpResolution->GetSize().x + 15;
		mpInpUndoStackSize = CreateInputNumber(vPos-cVector3f(0,3,0), _W("Undo stack size"), "", pTab, 50, 1);
		mpInpUndoStackSize->SetLayoutStyle(eEditorInputLayoutStyle_RowLabelOnLeft);
		mpInpUndoStackSize->UpdateLayout();
		mpInpUndoStackSize->SetLowerBound(true, 0);

		// BG color
		vPos.x += mpInpUndoStackSize->GetSize().x + 15;
		mpInpBackgroundColor = CreateInputColorFrame(vPos, _W("BG color"), "", pTab);

		// Disabled mesh coverage
		vPos.x = 15;
		vPos.y += mpInpBackgroundColor->GetSize().y + 15;
		mpInpDisabledCoverage = CreateInputNumber(vPos, _W("Disabled mesh coverage"), "", pTab, 50, 0.1f);
		mpInpDisabledCoverage->SetLayoutStyle(eEditorInputLayoutStyle_RowLabelOnLeft);
		mpInpDisabledCoverage->UpdateLayout();
		mpInpDisabledCoverage->SetLowerBound(true, 0);
		mpInpDisabledCoverage->SetUpperBound(true, 1);

		// Display gamma: same value the game feeds its tonemap from
		// Graphics/Gamma, so the pane matches what a player sees. Bounds match
		// the in-game slider.
		vPos.y += mpInpDisabledCoverage->GetSize().y + 15;
		mpInpDisplayGamma = CreateInputNumber(vPos, _W("Display gamma"), "", pTab, 50, 0.05f);
		mpInpDisplayGamma->SetLayoutStyle(eEditorInputLayoutStyle_RowLabelOnLeft);
		mpInpDisplayGamma->UpdateLayout();
		mpInpDisplayGamma->SetLowerBound(true, 0.3f);
		mpInpDisplayGamma->SetUpperBound(true, 2.0f);
		mpInpDisplayGamma->SetDecimals(2);

		// Renderer backend: kept as the "RendererBackend" editor setting (same
		// strings as the game's Graphics/RendererBackend) and applied when the
		// editor next creates its engine.
		vPos.y += mpInpDisplayGamma->GetSize().y + 15;
		tWStringList lstBackendOptions;
		lstBackendOptions.push_back(_W("Standard (original)"));
		lstBackendOptions.push_back(_W("Ray traced (Overdrive)"));
		mpCBRendererBackend = CreateInputEnum(vPos, _W("Renderer (needs restart)"), "", lstBackendOptions, pTab, 170);
		mpCBRendererBackend->SetLayoutStyle(eEditorInputLayoutStyle_RowLabelOnLeft);
		mpCBRendererBackend->UpdateLayout();
	}

	/////////////////////////////////////////////////////////
	// Performance options
	pTab = pFrame->AddTab(_W("Performance"));
	vPos = cVector3f(15,15,0.1f);

	// Lights active
	mpInpLightsActive = CreateInputBool(vPos, _W("Lights Active"), "", pTab);
	vPos.y += mpInpLightsActive->GetSize().y + 10;

	// Particles active
	mpInpPSActive = CreateInputBool(vPos, _W("Particle Systems Active"),"", pTab);
	vPos.y += mpInpPSActive->GetSize().y + 10;

	// World reflection
	mpInpWorldReflection = CreateInputBool(vPos, _W("World Reflection"), "", pTab);
	vPos.y += mpInpWorldReflection->GetSize().y + 15;

	tWStringList lstPlaneLabels;
	lstPlaneLabels.push_back(_W("Near"));
	lstPlaneLabels.push_back(_W("Far"));
	mpInpCamPlanes = CreateInputVec2(vPos, _W("Camera Clip Planes:"), "", pTab, 50, lstPlaneLabels, eEditorInputLayoutStyle_RowLabelOnLeft, 1);
	
	vPos.y += mpInpCamPlanes->GetSize().y + 15;
	mpInpShowSkybox = CreateInputBool(vPos, _W("Show Skybox"), "", pTab);
	vPos.y += mpInpShowSkybox->GetSize().y + 10;
	mpInpShowFog = CreateInputBool(vPos, _W("Show Fog"), "", pTab);
	vPos.y += mpInpShowFog->GetSize().y + 10;
	mpInpShowOtherRenderer = CreateInputBool(vPos, _W("Show Other Renderer Objects"), "", pTab);

	vPos.x = mpInpLightsActive->GetPosition().x + mpInpLightsActive->GetSize().x + 100;
	vPos.y = 15;
	mpInpTextureQuality = CreateInputEnum(vPos, _W("Texture Quality"), "", tWStringList(), pTab);
	mpInpTextureQuality->SetLayoutStyle(eEditorInputLayoutStyle_RowLabelOnLeft);
	mpInpTextureQuality->UpdateLayout();
	mpInpTextureQuality->AddValue(_W("High"));
	mpInpTextureQuality->AddValue(_W("Medium"));
	mpInpTextureQuality->AddValue(_W("Low"));
	vPos.y += mpInpTextureQuality->GetSize().y + 10;
	// Development-only control for the render/display resolution split: it only
	// changes the render extent so the spatial-fallback and display-depth-
	// reconstruction paths can be exercised, and it wires no upscaler provider
	// and enables no camera jitter. Global to cGraphics, so it applies to every
	// live viewport, including the editor's offscreen thumbnail and
	// camera-capture viewports.
	mpInpDevRenderScale = CreateInputEnum(vPos, _W("Render Scale (dev)"), "", tWStringList(), pTab);
	mpInpDevRenderScale->SetLayoutStyle(eEditorInputLayoutStyle_RowLabelOnLeft);
	mpInpDevRenderScale->UpdateLayout();
	mpInpDevRenderScale->AddValue(_W("Native (100%)"));
	mpInpDevRenderScale->AddValue(_W("90%"));
	mpInpDevRenderScale->AddValue(_W("75%"));
	mpInpDevRenderScale->AddValue(_W("66%"));
	mpInpDevRenderScale->AddValue(_W("50%"));
	mpInpDevRenderScale->AddValue(_W("33%"));

	/////////////////////////////////////////////////////////
	// Input options
	pTab = pFrame->AddTab(_W("Input"));
	vPos = cVector3f(15,15,0.1f);

	mpInpTumbleFactor = CreateInputNumber(vPos, _W("Tumble factor"), "", pTab, 75, 0.001f);
	vPos.x += mpInpTumbleFactor->GetSize().x + 15;

	mpInpTrackFactor = CreateInputNumber(vPos, _W("Track factor(non LT cam)"), "", pTab, 75, 0.01f);
	vPos.x += mpInpTrackFactor->GetSize().x + 15;

	mpInpZoomFactor = CreateInputNumber(vPos, _W("Zoom factor"), "", pTab, 75, 0.001f);
	vPos.x += mpInpZoomFactor->GetSize().x + 15;

	mpInpMouseWheelZoom = CreateInputNumber(vPos, _W("Mouse wheel zoom"), "", pTab, 75, 0.01f);
	vPos.x = 15;

	vPos.y += mpInpTumbleFactor->GetSize().y + 10;

	mpInpRotateSnap = CreateInputNumber(vPos, _W("Rotate snap"), "", pTab, 50, 1);
	mpInpRotateSnap->SetLowerBound(true,0);
	mpInpRotateSnap->SetDecimals(3);

	vPos.x += mpInpRotateSnap->GetSize().x + 15;

	mpInpScaleSnap = CreateInputNumber(vPos, _W("Scale snap"), "", pTab, 50, 0.25f);
	mpInpScaleSnap->SetLowerBound(true,0);
	mpInpRotateSnap->SetDecimals(3);

	/////////////////////////////////////////////////////////
	// MCP server options (only where supported — the LevelEditor)
	mpInpMCPEnabled    = NULL;
	mpInpMCPPort       = NULL;
	mpInpMCPToken      = NULL;
	mpMCPStatusLabel   = NULL;
	mpMCPRestartButton = NULL;
	mpMCPPreview       = NULL;
	if(mpEditor->SupportsMCP())
	{
		pTab = pFrame->AddTab(_W("MCP"));

		mpInpMCPEnabled = CreateInputBool(cVector3f(15,15,0.1f), _W("Server enabled"), "", pTab);

		mpInpMCPPort = CreateInputNumber(cVector3f(230,12,0.1f), _W("Port"), "", pTab, 60, 1);
		mpInpMCPPort->SetLayoutStyle(eEditorInputLayoutStyle_RowLabelOnLeft);
		mpInpMCPPort->UpdateLayout();
		mpInpMCPPort->SetLowerBound(true, 1);
		mpInpMCPPort->SetUpperBound(true, 65535);
		mpInpMCPPort->SetDecimals(0);

		mpInpMCPToken = CreateInputString(cVector3f(15,45,0.1f), _W("Token (optional)"), "", pTab, 200);
		mpInpMCPToken->SetLayoutStyle(eEditorInputLayoutStyle_RowLabelOnLeft);
		mpInpMCPToken->UpdateLayout();

		mpMCPStatusLabel = mpSet->CreateWidgetLabel(cVector3f(15,82,0.1f), 0, _W(""), pTab);

		mpMCPRestartButton = mpSet->CreateWidgetButton(cVector3f(430,78,0.1f), cVector2f(150,25), _W("Apply / Restart"), pTab);
		mpMCPRestartButton->AddCallback(eGuiMessage_ButtonPressed, this, kGuiCallback(MCPButtonCallback));

		mpSet->CreateWidgetLabel(cVector3f(15,112,0.1f), 0, _W("Copy connection config for:"), pTab);

		mvMCPCopyButtons.clear();
		int lClientCount = mpEditor->GetMCPClientCount();
		for(int i=0; i<lClientCount; ++i)
		{
			int lCol = i % 2;
			int lRow = i / 2;
			float fX = 15.0f + (float)lCol*300.0f;
			float fY = 134.0f + (float)lRow*28.0f;

			mpSet->CreateWidgetLabel(cVector3f(fX, fY+4, 0.1f), 0, cString::To16Char(mpEditor->GetMCPClientLabel(i)), pTab);
			cWidgetButton* pCopy = mpSet->CreateWidgetButton(cVector3f(fX+110, fY, 0.1f), cVector2f(60,24), _W("Copy"), pTab);
			pCopy->AddCallback(eGuiMessage_ButtonPressed, this, kGuiCallback(MCPButtonCallback));
			mvMCPCopyButtons.push_back(pCopy);
		}

		int lNumRows = (lClientCount + 1) / 2;
		float fPreviewY = 134.0f + (float)lNumRows*28.0f + 8.0f;
		mpMCPPreview = mpSet->CreateWidgetTextBox(cVector3f(15, fPreviewY, 0.1f), cVector2f(590,25), _W(""), pTab);
	}
}

//-----------------------------------------------------------------

void cEditorWindowOptions::PostInitLayout()
{
	OnUpdate(0);
	// Deliberately not read from or written to editor settings; this override is
	// development-only and must not persist across editor sessions.
	mpInpDevRenderScale->SetValue(0, false);
}

//-----------------------------------------------------------------

void cEditorWindowOptions::OnUpdate(float afTimeStep)
{
	iEditorWorld* pWorld = mpEditor->GetEditorWorld();

	cVector2f vScreenSize = cVector2f(cString::ToFloat(mpEditor->GetSetting("ScreenWidth").c_str(),1024),
										cString::ToFloat(mpEditor->GetSetting("ScreenHeight").c_str(),768));
	mpInpResolution->SetValue(vScreenSize, false);
	mpInpBackgroundColor->SetValue(pWorld->GetBGDefaultColor(), false);
	mpInpDisabledCoverage->SetValue(iEngineEntityMesh::GetDisabledCoverage(), false);
	mpInpUndoStackSize->SetValue((float)mpEditor->GetActionHandler()->GetMaxUndoSize(), false);
	mpInpDisplayGamma->SetValue(mpEditor->GetViewportDisplayGamma(), false);
	mpCBRendererBackend->SetValue(mpEditor->GetSetting("RendererBackend")=="overdrive" ? 1 : 0, false);

	{
		mpInpLightsActive->SetValue(pWorld->GetTypeActive(eEditorEntityType_Light), false);
		mpInpPSActive->SetValue(pWorld->GetTypeActive(eEditorEntityType_ParticleSystem), false);
		mpInpWorldReflection->SetValue(mpEditor->GetWorldReflectionActive(), false);
		mpInpShowSkybox->SetValue(pWorld->GetShowSkybox(), false);
		mpInpShowFog->SetValue(pWorld->GetShowFog(), false);
		mpInpShowOtherRenderer->SetValue(pWorld->GetShowOtherRendererObjects(), false);

		mpInpTextureQuality->SetValue(cString::ToInt(mpEditor->GetSetting("TexQuality").c_str(), 0), false);
	}

	mpInpCamPlanes->SetValue(cEditorWindowViewport::GetCamPlanes(), false);

	mpInpTumbleFactor->SetValue(cString::ToFloat(mpEditor->GetSetting("TumbleFactor").c_str(), 0.005f), false);
	mpInpTrackFactor->SetValue(cString::ToFloat(mpEditor->GetSetting("TrackFactor").c_str(), 0.01f), false);
	mpInpZoomFactor->SetValue(cString::ToFloat(mpEditor->GetSetting("ZoomFactor").c_str(), 0.001f), false);
	mpInpMouseWheelZoom->SetValue(cString::ToFloat(mpEditor->GetSetting("MouseWheelZoom").c_str(), 0.1f), false);
	mpInpRotateSnap->SetValue(cMath::ToDeg(cEditorSelection::GetRotateSnap()), false);
	mpInpScaleSnap->SetValue(cEditorSelection::GetScaleSnap(), false);

	if(mpEditor->SupportsMCP() && mpInpMCPEnabled)
	{
		bool bEnabled=false; int lPort=0; tString sToken, sStatus;
		mpEditor->GetMCPState(bEnabled, lPort, sToken, sStatus);
		mpInpMCPEnabled->SetValue(bEnabled, false);
		mpInpMCPPort->SetValue((float)lPort, false);
		mpInpMCPToken->SetValue(cString::To16Char(sToken), false);
		if(mpMCPStatusLabel) mpMCPStatusLabel->SetText(_W("Status: ") + cString::To16Char(sStatus));
	}
}

//-----------------------------------------------------------------

bool cEditorWindowOptions::WindowSpecificInputCallback(iEditorInput* apInput)
{
	iEditorWorld* pWorld = mpEditor->GetEditorWorld();
	if(apInput==mpInpResolution)
	{
		cVector2f vScreenSize = mpInpResolution->GetValue();
		mpEditor->SetSettingValue("ScreenWidth", cString::ToString(vScreenSize.x));
		mpEditor->SetSettingValue("ScreenHeight", cString::ToString(vScreenSize.y));
	}

	else if(apInput==mpInpBackgroundColor)
		pWorld->SetBGDefaultColor(mpInpBackgroundColor->GetValue());

	else if(apInput==mpInpDisabledCoverage)
	{
		iEngineEntityMesh::SetDisabledCoverage(mpInpDisabledCoverage->GetValue());
		pWorld->SetVisibilityUpdated();
		pWorld->UpdateVisibility();		
	}

	else if(apInput==mpInpDisplayGamma)
		mpEditor->SetViewportDisplayGamma(mpInpDisplayGamma->GetValue());

	else if(apInput==mpCBRendererBackend)
		mpEditor->SetSettingValue("RendererBackend", mpCBRendererBackend->GetValue()==1 ? "overdrive" : "standard");

	else if(apInput==mpInpUndoStackSize)
		mpEditor->GetActionHandler()->SetMaxUndoSize((int)mpInpUndoStackSize->GetValue());

	else if(apInput==mpInpLightsActive)
		pWorld->SetTypeActive(eEditorEntityType_Light, mpInpLightsActive->GetValue());

	else if(apInput==mpInpPSActive)
		pWorld->SetTypeActive(eEditorEntityType_ParticleSystem, mpInpPSActive->GetValue());

	else if(apInput==mpInpWorldReflection)
		mpEditor->SetWorldReflectionActive(mpInpWorldReflection->GetValue());

	else if(apInput==mpInpCamPlanes)
		cEditorWindowViewport::SetCamPlanes(mpInpCamPlanes->GetValue());

	else if(apInput==mpInpShowFog)
		pWorld->SetShowFog(mpInpShowFog->GetValue());

	else if(apInput==mpInpShowOtherRenderer)
		pWorld->SetShowOtherRendererObjects(mpInpShowOtherRenderer->GetValue());

	else if(apInput==mpInpShowSkybox)
		pWorld->SetShowSkybox(mpInpShowSkybox->GetValue());

	else if(apInput==mpInpTextureQuality)
		mpEditor->SetSettingValue("TexQuality", cString::ToString(mpInpTextureQuality->GetValue()));

	// Development-only control for the render/display resolution split: it only
	// changes the render extent so the spatial-fallback and display-depth-
	// reconstruction paths can be exercised, and it wires no upscaler provider
	// and enables no camera jitter. Global to cGraphics, so it applies to every
	// live viewport, including the editor's offscreen thumbnail and
	// camera-capture viewports.
	else if(apInput==mpInpDevRenderScale)
	{
		static const float avRenderScales[] = {1.0f, 0.90f, 0.75f, 0.66f, 0.50f, 0.33f};
		int lRenderScale = mpInpDevRenderScale->GetValue();
		if(lRenderScale >= 0 && lRenderScale < (int)(sizeof(avRenderScales) / sizeof(avRenderScales[0])))
		{
			cGraphics* pGraphics = Interface<cGraphics>::Get();
			if(pGraphics)
				pGraphics->devRenderScale = avRenderScales[lRenderScale];
		}
	}

	else if(apInput==mpInpTumbleFactor)
		mpEditor->SetSettingValue("TumbleFactor", cString::ToString(mpInpTumbleFactor->GetValue()));

	else if(apInput==mpInpTrackFactor)
		mpEditor->SetSettingValue("TrackFactor", cString::ToString(mpInpTrackFactor->GetValue()));

	else if(apInput==mpInpZoomFactor)
		mpEditor->SetSettingValue("ZoomFactor", cString::ToString(mpInpZoomFactor->GetValue()));

	else if(apInput==mpInpMouseWheelZoom)
		mpEditor->SetSettingValue("MouseWheelZoom", cString::ToString(mpInpMouseWheelZoom->GetValue()));

	else if(apInput==mpInpRotateSnap)
		cEditorSelection::SetRotateSnap(cMath::ToRad(mpInpRotateSnap->GetValue()));

	else if(apInput==mpInpScaleSnap)
		cEditorSelection::SetScaleSnap(mpInpScaleSnap->GetValue());

	else if(apInput==mpInpMCPEnabled || apInput==mpInpMCPPort || apInput==mpInpMCPToken)
	{
		bool bEnabled  = mpInpMCPEnabled->GetValue();
		int  lPort     = (int)mpInpMCPPort->GetValue();
		tString sToken = cString::To8Char(mpInpMCPToken->GetValue());
		mpEditor->ApplyMCPConfig(bEnabled, lPort, sToken);
	}


	tEditorViewportVec& vViewports = mpEditor->GetViewports();
	for(int i=0;i<(int)vViewports.size();++i)
	{
		cEditorWindowViewport* pViewport = vViewports[i];
		pViewport->GetVCamera()->FetchSettings();
	}

	mpEditor->SetLayoutNeedsUpdate(true);

	return true;
}

//-----------------------------------------------------------------

bool cEditorWindowOptions::MCPButtonCallback(iWidget* apWidget, const cGuiMessageData& aData)
{
	if(mpEditor->SupportsMCP()==false) return true;

	// Apply / Restart: push the current field values to the editor (restarts server).
	if(apWidget==mpMCPRestartButton)
	{
		bool bEnabled  = mpInpMCPEnabled->GetValue();
		int  lPort     = (int)mpInpMCPPort->GetValue();
		tString sToken = cString::To8Char(mpInpMCPToken->GetValue());
		mpEditor->ApplyMCPConfig(bEnabled, lPort, sToken);
		OnUpdate(0);
		return true;
	}

	// Copy[i]: put that client's connection snippet on the clipboard + preview it.
	for(size_t i=0; i<mvMCPCopyButtons.size(); ++i)
	{
		if(apWidget==mvMCPCopyButtons[i])
		{
			tString sSnippet = mpEditor->GetMCPClientSnippet((int)i);
			cPlatform::CopyTextToClipboard(cString::To16Char(sSnippet));
			if(mpMCPPreview) mpMCPPreview->SetText(cString::To16Char(sSnippet));
			break;
		}
	}
	return true;
}
kGuiCallbackDeclaredFuncEnd(cEditorWindowOptions, MCPButtonCallback);

//-----------------------------------------------------------------
