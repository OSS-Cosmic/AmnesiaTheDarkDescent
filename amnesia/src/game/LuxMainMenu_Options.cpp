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

#include "LuxMainMenu_Options.h"

#include "LuxMap.h"
#include "LuxMapHandler.h"
#include "LuxInputHandler.h"
#include "LuxHintHandler.h"
#include "LuxConfigHandler.h"
#include "LuxPostEffects.h"
#include "LuxMessageHandler.h"
#include "LuxPlayerHelpers.h"
#include "LuxPlayer.h"
#include "LuxHelpFuncs.h"
#include "graphics/TemporalUpscaler.h"
#include "graphics/TemporalUpscalerTypes.h"

#include <cstring>

//////////////////////////////////////////////////////////////////////////
// HELPERS
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

static int GetIndexFromAnisotropy(float afX)
{
	if(afX <= 1) return 0;
	if(afX <= 2) return 1;
	if(afX <= 4) return 2;
	if(afX <= 8) return 3;
	if(afX <= 16) return 4;
	return 0;
}

static float GetAnisotropyFromIndex(int alX)
{
	switch(alX)
	{
	case 0: return 1.0f;
	case 1: return 2.0f;
	case 2: return 4.0f;
	case 3: return 8.0f;
	case 4: return 16.0f;
	}
	return 1.0f;
}

static tWString TranslateOrDefault(const tString& asCat, const tString& asName, const tWString& asFallback)
{
	tWString sTranslation = kTranslate(asCat, asName);
	return sTranslation.empty() ? asFallback : sTranslation;
}

static tWString TemporalUpscalerQualityToDisplay(TemporalUpscalerQuality aQuality)
{
	switch(aQuality)
	{
	case TemporalUpscalerQuality::NativeAA: return TranslateOrDefault("OptionsMenu", "NativeAA", _W("Native AA"));
	case TemporalUpscalerQuality::Quality: return TranslateOrDefault("OptionsMenu", "Quality", _W("Quality"));
	case TemporalUpscalerQuality::Balanced: return TranslateOrDefault("OptionsMenu", "Balanced", _W("Balanced"));
	case TemporalUpscalerQuality::Performance: return TranslateOrDefault("OptionsMenu", "Performance", _W("Performance"));
	case TemporalUpscalerQuality::UltraPerformance: return TranslateOrDefault("OptionsMenu", "UltraPerformance", _W("Ultra Performance"));
	}
	return TranslateOrDefault("OptionsMenu", "Quality", _W("Quality"));
}

static tWString TemporalUpscalerProviderToDisplay(TemporalUpscalerProvider aProvider)
{
	switch(aProvider)
	{
	case TemporalUpscalerProvider::Fsr: return _W("FidelityFX FSR 3.1");
	case TemporalUpscalerProvider::XeSS: return _W("Intel XeSS");
	case TemporalUpscalerProvider::Off: return TranslateOrDefault("OptionsMenu", "Off", _W("Off"));
	}
	return TranslateOrDefault("OptionsMenu", "Off", _W("Off"));
}

static TemporalUpscalerProvider GetSelectedTemporalUpscalerProvider(cWidgetComboBox* apCombo)
{
	if(apCombo == NULL)
		return TemporalUpscalerProvider::Off;

	int lSelectedItem = apCombo->GetSelectedItem();
	if(lSelectedItem < 0 || lSelectedItem >= apCombo->GetItemNum())
		return TemporalUpscalerProvider::Off;

	cWidgetItem* pItem = apCombo->GetItem(lSelectedItem);
	if(pItem && pItem->GetUserValue() >= (int)TemporalUpscalerProvider::Off &&
		pItem->GetUserValue() <= (int)TemporalUpscalerProvider::XeSS)
		return (TemporalUpscalerProvider)pItem->GetUserValue();
	return TemporalUpscalerProvider::Off;
}

static TemporalUpscalerQuality GetSelectedTemporalUpscalerQuality(cWidgetComboBox* apCombo)
{
	if(apCombo == NULL)
		return TemporalUpscalerQuality::Quality;

	int lSelectedItem = apCombo->GetSelectedItem();
	if(lSelectedItem < 0 || lSelectedItem >= apCombo->GetItemNum())
		return TemporalUpscalerQuality::Quality;

	cWidgetItem* pItem = apCombo->GetItem(lSelectedItem);
	if(pItem && pItem->GetUserValue() >= (int)TemporalUpscalerQuality::NativeAA &&
		pItem->GetUserValue() <= (int)TemporalUpscalerQuality::UltraPerformance)
		return (TemporalUpscalerQuality)pItem->GetUserValue();
	return TemporalUpscalerQuality::Quality;
}

static hpl::eRendererBackend GetSelectedRendererBackend(cWidgetComboBox* apCombo)
{
	if(apCombo == NULL)
		return hpl::eRendererBackend_Standard;

	int lSelectedItem = apCombo->GetSelectedItem();
	if(lSelectedItem < 0 || lSelectedItem >= apCombo->GetItemNum())
		return hpl::eRendererBackend_Standard;

	cWidgetItem* pItem = apCombo->GetItem(lSelectedItem);
	if(pItem && pItem->GetUserValue() == (int)hpl::eRendererBackend_Overdrive)
		return hpl::eRendererBackend_Overdrive;
	return hpl::eRendererBackend_Standard;
}

static void SelectRendererBackend(cWidgetComboBox* apCombo, hpl::eRendererBackend aBackend, bool abGenCallback)
{
	if(apCombo == NULL)
		return;

	for(int i=0; i<apCombo->GetItemNum(); ++i)
	{
		cWidgetItem* pItem = apCombo->GetItem(i);
		if(pItem && pItem->GetUserValue() == (int)aBackend)
		{
			apCombo->SetSelectedItem(i, true, abGenCallback);
			return;
		}
	}
}

static tWString RendererBackendTip(bool abOverdriveSupported)
{
	if(abOverdriveSupported)
		return TranslateOrDefault("OptionsMenu", "RendererTip",
			_W("Ray traced lighting needs a GPU with hardware ray tracing. Changes take effect after restarting the game."));
	return TranslateOrDefault("OptionsMenu", "RendererUnsupportedTip",
		_W("This GPU does not support hardware ray tracing, so the Standard renderer is used."));
}

//-----------------------------------------------------------------------

cResourceVarsObject cLuxMainMenu_Options::mInitialValues = cResourceVarsObject();
cResourceVarsObject cLuxMainMenu_Options::mCurrentValues = cResourceVarsObject();

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// CONSTRUCTORS
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

cLuxMainMenu_Options::cLuxMainMenu_Options(cGuiSet *apGuiSet, cGuiSkin *apGuiSkin) : iLuxMainMenuWindow(apGuiSet, apGuiSkin)
{
	mvWindowSize = cVector2f(620,460);

	mbTipFadeRestart = false;
	mbTipWidgetUpdated = true;
	mbTipTextReset = false;
	mpCurrentTipWidget = NULL;

	mfGammaMin = 0.3f;
	mfGammaStep = 0.05f;
	mfGammaMax = 2.0f;

	mfMouseSensitivityMin = 0.2f;
	mfMouseSensitivityStep = 0.1f;
	mfMouseSensitivityMax = 5.0f;

#ifdef USE_GAMEPAD
	mfGamepadLookSensitivityMin = 0.5f;
	mfGamepadLookSensitivityStep = 0.05f;
	mfGamepadLookSensitivityMax = 3.0f;
#endif

	mfVolumeMin = 0.0f;
	mfVolumeStep = 0.1f;
	mfVolumeMax = 1.0f;

	mbShowCommentary = gpBase->mpMenuCfg->GetBool("Options","ShowCommentary", false);

	mbSettingInitialValues = false;
	mbRebuildingTemporalUpscaler = false;
	mbTemporalUpscalerFsrAvailable = false;
	mbTemporalUpscalerXeSSAvailable = false;
	mSuperSamplingRequestedProvider = hpl::TemporalUpscalerProvider::Off;
	mSuperSamplingRequestedQuality = hpl::TemporalUpscalerQuality::Quality;
	mfRenderScaleRequested = 1.0f;
	mSuperSamplingUnavailableProvider = hpl::TemporalUpscalerProvider::Off;

	mpCBTemporalUpscaler = NULL;
	mpCBTemporalUpscalerQuality = NULL;
	mpCBRenderScale = NULL;
	mpLRenderScaleHelp = NULL;
	mpCBRendererBackend = NULL;
	mpLRendererBackendHelp = NULL;
	mpLTemporalUpscalerStatus = NULL;
	mpSuperSamplingLoggedReason = NULL;
	msSuperSamplingStatusText = _W("");

	mbKeyConfigOpen = false;
}

//-----------------------------------------------------------------------

cLuxMainMenu_Options::~cLuxMainMenu_Options()
{
	STLDeleteAll(mvOptionData);
}

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// PUBLIC METHODS
//////////////////////////////////////////////////////////////////////////


//-----------------------------------------------------------------------

void cLuxMainMenu_Options::CreateGui()
{
	CreateMainGui();
}

//-----------------------------------------------------------------------

void cLuxMainMenu_Options::ExitPressed()
{
	if(mpGuiSet->PopUpIsActive()) return;

    gpBase->mpMainMenu->SetWindowActive(eLuxMainMenuWindow_LastEnum);
}

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// PRIVATE METHODS
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

void cLuxMainMenu_Options::CreateMainGui()
{
	float fLeftBorderSize = 30;
	float fUpperBorderSize = 10;

	cVector3f vPos(fLeftBorderSize, 35+fUpperBorderSize,1);

	//////////////////////////
	//Window
	mpWindow = mpGuiSet->CreateWidgetWindow(eWidgetWindowButtonFlag_None,cVector3f(0,0,5),mvWindowSize,kTranslate("MainMenu","Options"));
	mpWindow->AddCallback(eGuiMessage_OnUpdate, this, kGuiCallback(Window_OnUpdate));
	mpWindow->SetStatic(true);

	//////////////////////////
	//Buttons
	float fButtonWidth = 80;
	float fButtonSepp = 3;
	vPos.x = mpWindow->GetSize().x - fButtonWidth*2-fButtonSepp-5;
	vPos.y = mpWindow->GetSize().y - 25 - 10;

	mpBOK = mpGuiSet->CreateWidgetButton(vPos,cVector2f(fButtonWidth,30),kTranslate("MainMenu","OK"),mpWindow);
	mpBOK->AddCallback(eGuiMessage_ButtonPressed,this, kGuiCallback(PressOK));

	vPos.x += fButtonWidth + fButtonSepp;
	mpBCancel = mpGuiSet->CreateWidgetButton(vPos,cVector2f(fButtonWidth,30),kTranslate("MainMenu","Cancel"),mpWindow);
	mpBCancel->AddCallback(eGuiMessage_ButtonPressed,this, kGuiCallback(PressCancel));
	mpBCancel->AddCallback(eGuiMessage_UIButtonPress,this, kGuiCallback(UIPressCancel));
	mpBCancel->SetGlobalUIInputListener(true);

	mpBOK->SetFocusNavigation(eUIArrow_Right, mpBCancel);
	mpBCancel->SetFocusNavigation(eUIArrow_Left, mpBOK);




	vPos = cVector3f(fLeftBorderSize, 35+fUpperBorderSize,1);

	//////////////////////////
	//Tabs
	cVector2f vTabFrameSize(mvWindowSize.x-fLeftBorderSize*2, mvWindowSize.y-fUpperBorderSize*3-30-25-20);
	cWidgetTabFrame *pTabFrame = mpGuiSet->CreateWidgetTabFrame(vPos,vTabFrameSize,_W(""),mpWindow, false, true);
	pTabFrame->SetGlobalUIInputListener(true);


	pTabFrame->AddCallback(eGuiMessage_SelectionChange, this, kGuiCallback(TabFrame_OnPageChange));



#ifdef USE_ONLIVE
	mpTabGame = pTabFrame->AddTab(kTranslate("MainMenu","OptionsGame"));
	mpTabInput = pTabFrame->AddTab(kTranslate("MainMenu","OptionsInput"));
	mpTabGraphics = NULL;
	mpTabSound = NULL;
#else
	mpTabGame = pTabFrame->AddTab(kTranslate("MainMenu","OptionsGame"));
	mpTabGraphics = pTabFrame->AddTab(kTranslate("MainMenu","OptionsGraphics"));
	mpTabInput = pTabFrame->AddTab(kTranslate("MainMenu","OptionsInput"));
	mpTabSound = pTabFrame->AddTab(kTranslate("MainMenu","OptionsSound"));
#endif

#if USE_GAMEPAD
	///////////////////
	// Add help icons for tabs
	cVector2f vImageSize = fUpperBorderSize * 2; vImageSize.x *= 1.5f;
	vPos.y -= 1;
	vPos.x -= fUpperBorderSize * 1.3f;

	mpShoulderHint[0] = mpGuiSet->CreateWidgetImage("gamepad_lb.tga", vPos, vImageSize, eGuiMaterial_Alpha, false, mpWindow, "LB Tip");

	vPos.x = mpWindow->GetSize().x - fUpperBorderSize * 5.0f;

	mpShoulderHint[1] = mpGuiSet->CreateWidgetImage("gamepad_rb.tga", vPos, vImageSize, eGuiMaterial_Alpha, false, mpWindow, "RB Tip");
#endif

	for(int i=0; i<pTabFrame->GetTabNum(); ++i)
	{
		cWidgetTab* pTab = pTabFrame->GetTab(i);
		int lPrev = i-1;
		int lNext = i+1;

		if(lPrev>=0)
			pTab->GetTabLabel()->SetFocusNavigation(eUIArrow_Left, pTabFrame->GetTab(lPrev)->GetTabLabel());
		if(lNext<pTabFrame->GetTabNum())
			pTab->GetTabLabel()->SetFocusNavigation(eUIArrow_Right, pTabFrame->GetTab(lNext)->GetTabLabel());
	}

#ifndef USE_ONLIVE
	AddGraphicsOptions(mpTabGraphics);
	AddSoundOptions(mpTabSound);
#endif
	AddGameOptions(mpTabGame);
	AddInputOptions(mpTabInput);

	//////////////////////////
	//Tip Label
	vPos.x = fLeftBorderSize;
	vPos.y = mpWindow->GetSize().y - 25 - 10;
	mpLTip = mpGuiSet->CreateWidgetLabel(vPos + cVector3f(10,0,0), cVector2f(400,30), _W(""), mpWindow);
	mpLTip->SetDefaultFontColor(cColor(1,1));
	mpLTip->SetWordWrap(true);
	mpLTip->SetClipActive(true);
	mpLTip->SetScrollSpeedMul(4.0f);
	mpLTip->SetDrawBackGround(true);
	mpLTip->SetBackGroundColor(cColor(0, 0.5f));

	mpLastFocusedWidget = mpCBLanguage;
}

//-----------------------------------------------------------------------

void cLuxMainMenu_Options::AddGameOptions(cWidgetTab* apTab)
{
	float fBorderSize = 15;
	cVector3f vPos(fBorderSize, 6 + fBorderSize + 10,0.1f);

	///////////////////////////////////////////////
	// Language Combobox
	cWidgetLabel* pLabel = mpGuiSet->CreateWidgetLabel(vPos, -1, kTranslate("OptionsMenu","Language"), apTab);
	mpCBLanguage = mpGuiSet->CreateWidgetComboBox(vPos + cVector3f(pLabel->GetSize().x + 5,-2,0), cVector2f(150,25), _W(""), apTab);
	SetUpInput(pLabel, mpCBLanguage, false, kTranslate("OptionsMenu", "LanguageTip"));
	mpCBLanguage->AddCallback(eGuiMessage_SelectionChange, this, kGuiCallback(ChangeLanguage));

	vPos.y += mpCBLanguage->GetSize().y + 15;

	///////////////////////////////////////////////
	// Subtitles Checkbox
	mpChBShowSubtitles = mpGuiSet->CreateWidgetCheckBox(vPos, 0, kTranslate("OptionsMenu","ShowSubtitles"), apTab);
	SetUpInput(NULL, mpChBShowSubtitles, false, kTranslate("OptionsMenu", "ShowSubtitlesTip"));

	///////////////////////////////////////////////
	// Effect subtitles Checkbox
	mpChBShowEffectSubtitles = mpGuiSet->CreateWidgetCheckBox(vPos + cVector3f(mpChBShowSubtitles->GetSize().x+15,0,0), 0, kTranslate("OptionsMenu", "ShowEffectSubtitles"), apTab);
	SetUpInput(NULL, mpChBShowEffectSubtitles, false, kTranslate("OptionsMenu", "ShowEffectSubtitlesTip"));

	vPos.y += mpChBShowSubtitles->GetSize().y + 15;

	///////////////////////////////////////////////
	// Hints Checkbox
	mpChBShowHints = mpGuiSet->CreateWidgetCheckBox(vPos, 0, kTranslate("OptionsMenu","ShowHints"), apTab);
	SetUpInput(NULL, mpChBShowHints, false, kTranslate("OptionsMenu", "ShowHintsTip"));

	vPos.y += mpChBShowHints->GetSize().y + 15;

	///////////////////////////////////////////////
	// Death Hints subtitles Checkbox
	mpChBShowDeathHints = mpGuiSet->CreateWidgetCheckBox(vPos, 0, kTranslate("OptionsMenu","ShowDeathHints"), apTab);
	SetUpInput(NULL, mpChBShowDeathHints, false, kTranslate("OptionsMenu", "ShowDeathHintsTip"));

	vPos.y += mpChBShowDeathHints->GetSize().y + 15;

	///////////////////////////////////////////////
	// Crosshair Checkbox
	mpChBShowCrosshair = mpGuiSet->CreateWidgetCheckBox(vPos, 0, kTranslate("OptionsMenu", "ShowCrosshair"), apTab);
	SetUpInput(NULL, mpChBShowCrosshair, false, kTranslate("OptionsMenu", "ShowCrosshairTip"));
	vPos.y += mpChBShowCrosshair->GetSize().y + 15;

	///////////////////////////////////////////////
	// Focus Icon style Combobox
	pLabel = mpGuiSet->CreateWidgetLabel(vPos, -1, kTranslate("OptionsMenu","FocusIconStyle"), apTab);
	mpCBFocusIconStyle = mpGuiSet->CreateWidgetComboBox(vPos + cVector3f(pLabel->GetSize().x + 5,-2,0), cVector2f(150, 25), _W(""), apTab);
	SetUpInput(pLabel, mpCBFocusIconStyle, false, kTranslate("OptionsMenu", "FocusIconStyleTip"));
	mpCBFocusIconStyle->AddItem(kTranslate("OptionsMenu", "FocusIconStyleDefault"));
	mpCBFocusIconStyle->AddItem(kTranslate("OptionsMenu", "FocusIconStyleSimple"));
	vPos.y += pLabel->GetSize().y + 15;

	//////////////////////////////////
	// Commentary
	if(mbShowCommentary)
	{
		mpChBShowCommentary = mpGuiSet->CreateWidgetCheckBox(vPos, 0, kTranslate("OptionsMenu", "Commentary"), apTab);
		SetUpInput(NULL, mpChBShowCommentary, false, kTranslate("OptionsMenu", "CommentaryTip"));
		vPos.y += mpChBShowCommentary->GetSize().y + 15;
	}

	// Populate languages
	PopulateLanguageList();


	//////////////////////////////////////////////////////////////////////////////////
	// Set up navigation

	iWidget* pLastWidget = NULL;

	// Down
	mpCBLanguage->SetFocusNavigation(eUIArrow_Down, mpChBShowSubtitles);
	mpChBShowSubtitles->SetFocusNavigation(eUIArrow_Down, mpChBShowHints);
	mpChBShowEffectSubtitles->SetFocusNavigation(eUIArrow_Down, mpChBShowHints);
	mpChBShowHints->SetFocusNavigation(eUIArrow_Down, mpChBShowDeathHints);
	mpChBShowDeathHints->SetFocusNavigation(eUIArrow_Down, mpChBShowCrosshair);
	mpChBShowCrosshair->SetFocusNavigation(eUIArrow_Down, mpCBFocusIconStyle);
	if(mbShowCommentary)
	{
		mpCBFocusIconStyle->SetFocusNavigation(eUIArrow_Down, mpChBShowCommentary);
		mpChBShowCommentary->SetFocusNavigation(eUIArrow_Down, mpBOK);

		pLastWidget = mpChBShowCommentary;
	}
	else
	{
		mpCBFocusIconStyle->SetFocusNavigation(eUIArrow_Down, mpBOK);

		pLastWidget = mpCBFocusIconStyle;
	}

	apTab->SetUserData(pLastWidget);
	apTab->GetTabLabel()->SetUserData(mpCBLanguage);


	// Up
	mpChBShowSubtitles->SetFocusNavigation(eUIArrow_Up, mpCBLanguage);
	mpChBShowEffectSubtitles->SetFocusNavigation(eUIArrow_Up, mpCBLanguage);
	mpChBShowHints->SetFocusNavigation(eUIArrow_Up, mpChBShowSubtitles);
	mpChBShowDeathHints->SetFocusNavigation(eUIArrow_Up, mpChBShowHints);
	mpChBShowCrosshair->SetFocusNavigation(eUIArrow_Up, mpChBShowDeathHints);
	mpCBFocusIconStyle->SetFocusNavigation(eUIArrow_Up, mpChBShowCrosshair);
	if(mbShowCommentary)
		mpChBShowCommentary->SetFocusNavigation(eUIArrow_Up, mpCBFocusIconStyle);

	// Left/Right
	mpChBShowSubtitles->SetFocusNavigation(eUIArrow_Right, mpChBShowEffectSubtitles);
	mpChBShowEffectSubtitles->SetFocusNavigation(eUIArrow_Left, mpChBShowSubtitles);
}

//-----------------------------------------------------------------------

void cLuxMainMenu_Options::AddGraphicsOptions(cWidgetTab* apTab)
{
	float fBorderSize = 15;
	cVector3f vPos(fBorderSize-5, 6 + fBorderSize, 0.1f);

	/////////////////////////////////
	// Basic options
	mpDBasicGfxOptions = mpGuiSet->CreateWidgetDummy(vPos, apTab);
	mpDBasicGfxOptions->SetVisible(true);
	mpDBasicGfxOptions->SetEnabled(true);
	AddBasicGfxOptions(mpDBasicGfxOptions);

	/////////////////////////////////
	// Avanced options
	mpDAdvancedGfxOptions = mpGuiSet->CreateWidgetDummy(vPos, apTab);
	mpDAdvancedGfxOptions->SetVisible(false);
	mpDAdvancedGfxOptions->SetEnabled(false);
	AddAdvancedGfxOptions(mpDAdvancedGfxOptions);

	// Basic/Advanced toggle button
	tWStringVec vToggleButtonStrings;
	vToggleButtonStrings.push_back(kTranslate("OptionsMenu","AdvancedOptions"));
	vToggleButtonStrings.push_back(kTranslate("OptionsMenu","BasicOptions"));

	float fButtonWidth=0;
	mpBToggleShowGfxOptions = mpGuiSet->CreateWidgetButton(0,
														   cVector2f(0,25),
														   vToggleButtonStrings[0],
														   apTab);
	mpBToggleShowGfxOptions->AddCallback(eGuiMessage_ButtonPressed, this, kGuiCallback(PressToggleShowGfxOptions));
	for(int i=0;i<(int)vToggleButtonStrings.size();++i)
	{
		float fStringLength = mpBToggleShowGfxOptions->GetDefaultFontType()->GetLength(mpBToggleShowGfxOptions->GetDefaultFontSize(),
																					mpBToggleShowGfxOptions->GetText().c_str()) + 10;

		if(fButtonWidth < fStringLength)
			fButtonWidth = fStringLength;
	}


	mpBToggleShowGfxOptions->SetSize(cVector2f(fButtonWidth, mpBToggleShowGfxOptions->GetSize().y));
	mpBToggleShowGfxOptions->SetPosition(cVector3f(apTab->GetSize())-cVector3f(fButtonWidth, 50, -2));

	SetUpInput(NULL, mpBToggleShowGfxOptions, true, _W(""));

	mpBToggleShowGfxOptions->SetFocusNavigation(eUIArrow_Down, mpBOK);
	mpBToggleShowGfxOptions->SetFocusNavigation(eUIArrow_Up, mpSGamma);

	mpSGamma->SetFocusNavigation(eUIArrow_Down, mpBToggleShowGfxOptions);
	mpChBRefraction->SetFocusNavigation(eUIArrow_Down, mpBToggleShowGfxOptions);

	apTab->SetUserData(mpBToggleShowGfxOptions);
	apTab->GetTabLabel()->SetUserData(mpCBResolution);
}

//-----------------------------------------------------------------------

void cLuxMainMenu_Options::AddBasicGfxOptions(cWidgetDummy* apDummy)
{
	float fBorderSize = 0;
	cVector3f vPos(fBorderSize, 6 + fBorderSize, 0.1f);

	// Scrollable, for the same reason the advanced pane is: the renderer
	// selector and everything added after it push the pane past the tab, and a
	// fixed dummy just clips whatever does not fit. Sized and shaped exactly
	// like AddAdvancedGfxOptions' frame so toggling between the two panes does
	// not visibly resize the content.
	//
	// Fit the scroll frame to the tab area above the Basic/Advanced toggle
	// button (placed 50 units above the tab's bottom edge). A taller fixed
	// frame hangs below the visible tab, and its scroll range counts that
	// hidden strip as already on screen, so the last options could never be
	// scrolled into view.
	const float fFrameHeight = apDummy->GetParent()->GetSize().y - apDummy->GetLocalPosition().y - 55;
	cWidgetFrame* pMainFrame = mpGuiSet->CreateWidgetFrame(cVector3f(0,0,1), cVector2f(550,fFrameHeight), false, apDummy, false, true);
	pMainFrame->SetDrawBackground(false);

	cWidgetLabel* pLabel = NULL;

	/////////////////////////////////
	// Screen group
	cWidgetGroup *pGroup = mpGuiSet->CreateWidgetGroup(vPos,0, kTranslate("OptionsMenu", "Screen"), pMainFrame);
	{
		float fBorderSize = 15;
		// Width off the scroll frame, not the tab: the vertical scrollbar eats
		// the difference, and a group sized to the tab would sit under it.
		pGroup->SetSize(cVector2f(pMainFrame->GetSize().x-fBorderSize-fBorderSize,70));
		cVector3f vPosInGroup = cVector3f(fBorderSize, fBorderSize, 0.1f);

		/////////////////////////////////
		// Resolution
		pLabel = mpGuiSet->CreateWidgetLabel(vPosInGroup, -1, kTranslate("OptionsMenu","Resolution"), pGroup);
		mpCBResolution = mpGuiSet->CreateWidgetComboBox(pLabel->GetLocalPosition() + cVector3f(0,pLabel->GetSize().y+5,0), cVector2f(175, 25), _W(""), pGroup);
		SetUpInput(pLabel, mpCBResolution, true, kTranslate("OptionsMenu","ResolutionTip"));

		vPosInGroup.x += mpCBResolution->GetSize().x + 100;

		/////////////////////////////////
		// Full screen and Vsync
		mpChBFullScreen = mpGuiSet->CreateWidgetCheckBox(vPosInGroup + cVector3f(0,2,0), -1, kTranslate("OptionsMenu","FullScreen"), pGroup);
		SetUpInput(NULL, mpChBFullScreen, true, kTranslate("OptionsMenu","FullScreenTip"));

		mpChBVSync = mpGuiSet->CreateWidgetCheckBox(vPosInGroup + cVector3f(0,mpChBFullScreen->GetSize().y+10,0), 0, kTranslate("OptionsMenu","VSync"), pGroup);
		SetUpInput(NULL, mpChBVSync, false, kTranslate("OptionsMenu","VSyncTip"));


//		mpChBAdaptiveVSync = mpGuiSet->CreateWidgetCheckBox(vPosInGroup + cVector3f(mpChBVSync->GetSize().x+10,mpChBFullScreen->GetSize().y+10,0), 0, kTranslate("OptionsMenu","AdaptiveVSync"), pGroup);
//		SetUpInput(NULL, mpChBAdaptiveVSync, false, kTranslate("OptionsMenu","AdaptiveVSyncTip"));

	}

	vPos.y += pGroup->GetSize().y + 15;

	/////////////////////////////////
	// Renderer
	pLabel = mpGuiSet->CreateWidgetLabel(vPos, -1, TranslateOrDefault("OptionsMenu", "Renderer", _W("Renderer")), pMainFrame);
	mpCBRendererBackend = mpGuiSet->CreateWidgetComboBox(pLabel->GetLocalPosition() + cVector3f(0,pLabel->GetSize().y+5,0), cVector2f(220, 25), _W(""), pMainFrame);
	SetUpInput(pLabel, mpCBRendererBackend, true, RendererBackendTip(true));
	{
		cWidgetItem* pItem = mpCBRendererBackend->AddItem(TranslateOrDefault("OptionsMenu", "RendererStandard", _W("Standard (original)")));
		pItem->SetUserValue((int)hpl::eRendererBackend_Standard);
		pItem = mpCBRendererBackend->AddItem(TranslateOrDefault("OptionsMenu", "RendererOverdrive", _W("Ray traced (Overdrive)")));
		pItem->SetUserValue((int)hpl::eRendererBackend_Overdrive);
	}

	// Shown by RefreshRendererBackendControl when the GPU cannot ray trace.
	mpLRendererBackendHelp = mpGuiSet->CreateWidgetLabel(mpCBRendererBackend->GetLocalPosition() + cVector3f(mpCBRendererBackend->GetSize().x + 10, 4, 0), -1,
		TranslateOrDefault("OptionsMenu", "RendererRayTracingUnsupported", _W("Ray tracing is not supported on this GPU.")), pMainFrame);
	mpLRendererBackendHelp->SetVisible(false);

	vPos.y += pLabel->GetSize().y + 5 + mpCBRendererBackend->GetSize().y + 15;

	/////////////////////////////////
	// Texture Quality
	pLabel = mpGuiSet->CreateWidgetLabel(vPos, -1, kTranslate("OptionsMenu","TexQuality"), pMainFrame);
	mpCBTextureSizeLevel = mpGuiSet->CreateWidgetComboBox(pLabel->GetLocalPosition() + cVector3f(0,pLabel->GetSize().y +5,0), cVector2f(100,25), _W(""), pMainFrame);
	SetUpInput(pLabel, mpCBTextureSizeLevel, true, kTranslate("OptionsMenu","TexQualityTip"));

	cMaterialManager* pMatMgr = gpBase->mpEngine->GetResources()->GetMaterialManager();
	tWStringVec vTexQualityStrings;
	vTexQualityStrings.push_back(kTranslate("OptionsMenu","Low"));
	vTexQualityStrings.push_back(kTranslate("OptionsMenu","Medium"));
	vTexQualityStrings.push_back(kTranslate("OptionsMenu","High"));

	mpCBTextureSizeLevel->ClearItems();
	for(int i=0;i<(int)vTexQualityStrings.size();++i)
		mpCBTextureSizeLevel->AddItem(vTexQualityStrings[i]);

	/////////////////////////////////
	// Gamma
	vPos.x += 140;

	pLabel = mpGuiSet->CreateWidgetLabel(vPos, -1, kTranslate("OptionsMenu","Gamma"), pMainFrame);
	{
		cVector3f vLabelPos(0, pLabel->GetSize().y+5, 0);

		cWidgetImage *pImg = mpGuiSet->CreateWidgetImage("menu_gamma.tga", vLabelPos, -1, eGuiMaterial_Alpha, false, pLabel);
		SetUpInput(NULL, pImg, false, kTranslate("OptionsMenu","GammaInstructions"));
		vLabelPos.y += pImg->GetSize().y + 5.0f;

		mpSGamma = mpGuiSet->CreateWidgetSlider(eWidgetSliderOrientation_Horizontal,vLabelPos, cVector2f(pImg->GetSize().x, 20), 0, pLabel);
		//mpSGamma->AddCallback(eGuiMessage_SliderMove, this, kGuiCallback(GammaSlider_OnMove));
		SetUpInput(pLabel, mpSGamma, false, kTranslate("OptionsMenu","GammaInstructions"));
		SetUpSlider(mpSGamma, mfGammaMin, mfGammaMax, mfGammaStep, kGuiCallback(GammaSlider_OnMove), &mpLGamma);
		mpSGamma->SetBarValueSize(4);

		//mpLGamma = mpGuiSet->CreateWidgetLabel(cVector3f(mpSGamma->GetSize().x*0.5f,2,1), -1, _W(""), mpSGamma);
		//mpLGamma->SetTextAlign(eFontAlign_Center);

		vLabelPos.y += mpSGamma->GetSize().y + 4.0f;

		// Grow the label to the box its children actually occupy.
		//
		// cWidgetFrame::OnUpdate measures the scroll range from its DIRECT
		// children only -- it walks mlstChildren taking localPos + size, and
		// never descends. This label's own size is just the word "Gamma", so
		// the frame would think the pane ended there: no scrollbar, and the
		// image and slider hanging below it silently clipped.
		pLabel->SetSize(cVector2f(cMath::Max(pLabel->GetSize().x, pImg->GetSize().x),
								  vLabelPos.y));

		//cWidgetLabel *pLInstr = mpGuiSet->CreateWidgetLabel(vLabelPos, cVector2f(pImg->GetSize().x,27), kTranslate("OptionsMenu","GammaInstructions"), pLabel);
		//pLInstr->SetDrawBackGround(true);
		//pLInstr->SetBackGroundColor(cColor(0, 0.7f));
		//pLInstr->SetWordWrap(true);
		//pLInstr->SetDefaultFontSize(12);
	}

	mpCBResolution->SetFocusNavigation(eUIArrow_Down, mpCBRendererBackend);
	mpCBResolution->SetFocusNavigation(eUIArrow_Right, mpChBFullScreen);

	mpChBFullScreen->SetFocusNavigation(eUIArrow_Left, mpCBResolution);
	mpChBFullScreen->SetFocusNavigation(eUIArrow_Down, mpChBVSync);

	mpChBVSync->SetFocusNavigation(eUIArrow_Left, mpCBResolution);
//	mpChBVSync->SetFocusNavigation(eUIArrow_Right, mpChBAdaptiveVSync);
	mpChBVSync->SetFocusNavigation(eUIArrow_Up, mpChBFullScreen);
	mpChBVSync->SetFocusNavigation(eUIArrow_Down, mpCBRendererBackend);

//	mpChBAdaptiveVSync->SetFocusNavigation(eUIArrow_Left, mpChBVSync);
//	mpChBAdaptiveVSync->SetFocusNavigation(eUIArrow_Up, mpChBFullScreen);
//	mpChBAdaptiveVSync->SetFocusNavigation(eUIArrow_Down, mpCBTextureSizeLevel);

	mpCBRendererBackend->SetFocusNavigation(eUIArrow_Up, mpChBVSync);
	mpCBRendererBackend->SetFocusNavigation(eUIArrow_Down, mpCBTextureSizeLevel);

	mpCBTextureSizeLevel->SetFocusNavigation(eUIArrow_Up, mpCBRendererBackend);
	mpCBTextureSizeLevel->SetFocusNavigation(eUIArrow_Down, mpSGamma);

	mpSGamma->SetFocusNavigation(eUIArrow_Up, mpCBTextureSizeLevel);

}

//-----------------------------------------------------------------------

void cLuxMainMenu_Options::AddAdvancedGfxOptions(cWidgetDummy* apDummy)
{
	float fBorderSize = 0;
	cVector3f vPos(fBorderSize, 6 + fBorderSize, 0.1f);
	float fItemSep = 180;

	// Fit the scroll frame to the tab area above the Basic/Advanced toggle button
	// (placed 50 units above the tab's bottom edge). A taller fixed frame hangs
	// below the visible tab, and its scroll range counts that hidden strip as
	// already on screen, so the last options could never be scrolled into view.
	const float fFrameHeight = apDummy->GetParent()->GetSize().y - apDummy->GetLocalPosition().y - 55;
	cWidgetFrame* pMainFrame = mpGuiSet->CreateWidgetFrame(cVector3f(0,0,1), cVector2f(550,fFrameHeight), false, apDummy, false, true);
	pMainFrame->SetDrawBackground(false);

	cWidgetLabel* pLabel = NULL;

	cVector2f vGroupSize = cVector2f(520, 70);

	////////////////////////////
	// Texture
	cWidgetGroup *pGroup = mpGuiSet->CreateWidgetGroup(vPos, vGroupSize, kTranslate("OptionsMenu","Material"), pMainFrame);
	{
		float fBorderSize = 15;
		cVector3f vPosInGroup = cVector3f(fBorderSize, fBorderSize, 0.1f);

		////////////////////////////
		// Texture filter
		pLabel = mpGuiSet->CreateWidgetLabel(vPosInGroup, -1, kTranslate("OptionsMenu","TexFilter"), pGroup);
		mpCBTextureFilter = mpGuiSet->CreateWidgetComboBox(cVector3f(0,pLabel->GetSize().y+5,0), cVector2f(150,25), _W(""), pLabel);
		SetUpInput(pLabel, mpCBTextureFilter, false, kTranslate("OptionsMenu","TexFilterTip"));

		//vPosInGroup.x += mpCBTextureFilter->GetSize().x + 20;

		vPosInGroup.x += fItemSep;

		/////////////////////////////
		// Anisotropy
		pLabel = mpGuiSet->CreateWidgetLabel(vPosInGroup, -1, kTranslate("OptionsMenu","Anisotropy"), pGroup);
		mpCBAnisotropy = mpGuiSet->CreateWidgetComboBox(cVector3f(0,pLabel->GetSize().y+5,0), cVector2f(100,25), _W(""), pLabel);
		SetUpInput(pLabel, mpCBAnisotropy, false, kTranslate("OptionsMenu","AnisotropyTip"));

		//vPosInGroup.x += mpCBAnisotropy->GetSize().x + 20;

		vPosInGroup.x += fItemSep;

		/////////////////////////////
		// Parallax Quality
		pLabel = mpGuiSet->CreateWidgetLabel(vPosInGroup, -1, kTranslate("Launcher","Parallax"), pGroup);
		mpCBParallaxQuality = mpGuiSet->CreateWidgetComboBox(cVector3f(0,pLabel->GetSize().y+5,0), cVector2f(100,25), _W(""), pLabel);
		SetUpInput(pLabel, mpCBParallaxQuality, true, kTranslate("OptionsMenu","ParallaxQualityTip"));
	}

	vPos.y += pGroup->GetSize().y + 10;

    /////////////////////////////
	// Shadows Group
	pGroup = mpGuiSet->CreateWidgetGroup(vPos, vGroupSize, kTranslate("OptionsMenu", "Shadows"), pMainFrame);
	{
		float fBorderSize = 15;
		cVector3f vPosInGroup = cVector3f(fBorderSize, fBorderSize, 0.1f);

		/////////////////////////////
		// Shadows Active
		mpChBShadows = mpGuiSet->CreateWidgetCheckBox(vPosInGroup, 0, kTranslate("OptionsMenu", "Shadows"), pGroup);
		SetUpInput(NULL, mpChBShadows, false, kTranslate("OptionsMenu","ShadowsTip"));

		//vPosInGroup.x += mpChBShadows->GetSize().x + 15;
		vPosInGroup.x += fItemSep;

		/////////////////////////////
		// Shadow Quality
		pLabel = mpGuiSet->CreateWidgetLabel(vPosInGroup, -1, kTranslate("OptionsMenu", "ShadowQuality"), pGroup);
		mpCBShadowQuality = mpGuiSet->CreateWidgetComboBox(cVector3f(0,pLabel->GetSize().y+5,0), cVector2f(100,25), _W(""), pLabel);
		SetUpInput(pLabel, mpCBShadowQuality, true, kTranslate("OptionsMenu","ShadowQualityTip"));

		//vPosInGroup.x += mpCBShadowQuality->GetSize().x + 15;
		vPosInGroup.x += fItemSep;

		/////////////////////////////
		// Shadow Resolution
		pLabel = mpGuiSet->CreateWidgetLabel(vPosInGroup, -1, kTranslate("OptionsMenu", "ShadowRes"), pGroup);
		mpCBShadowRes = mpGuiSet->CreateWidgetComboBox(cVector3f(0,pLabel->GetSize().y+5,0), cVector2f(100,25), _W(""), pLabel);
		SetUpInput(pLabel, mpCBShadowRes, true, kTranslate("OptionsMenu","ShadowResTip"));


		// Set up values
		tWStringVec vOptionStrings;
		vOptionStrings.push_back(kTranslate("OptionsMenu","Low"));
		vOptionStrings.push_back(kTranslate("OptionsMenu","Medium"));
		vOptionStrings.push_back(kTranslate("OptionsMenu","High"));

		mpCBShadowQuality->ClearItems();
		mpCBShadowRes->ClearItems();
		mpCBParallaxQuality->ClearItems();
		for(int i=0;i<(int)vOptionStrings.size();++i)
		{
			mpCBShadowQuality->AddItem(vOptionStrings[i]);
			mpCBShadowRes->AddItem(vOptionStrings[i]);
		}

		mpCBParallaxQuality->AddItem(kTranslate("Launcher","Off"));
		mpCBParallaxQuality->AddItem(kTranslate("Launcher","On"));//Skipping medium since high and medium is really the same!
	}

	vPos.y += pGroup->GetSize().y + 10;

	////////////////////////////
	// Post Effects
	pGroup = mpGuiSet->CreateWidgetGroup(vPos, vGroupSize, kTranslate("OptionsMenu","PostEffects"), pMainFrame);
	{
		float fBorderSize = 15;
		float fInputSep = 10;
		cVector3f vPosInGroup = cVector3f(fBorderSize, fBorderSize, 0.1f);
		float fMaxWidth = 0;

		// Bloom
		mpChBBloom = mpGuiSet->CreateWidgetCheckBox(vPosInGroup, 0, kTranslate("OptionsMenu","Bloom"), pGroup);
		SetUpInput(NULL, mpChBBloom, false, kTranslate("OptionsMenu","BloomTip"));

		if(fMaxWidth < mpChBBloom->GetSize().x)
			fMaxWidth = mpChBBloom->GetSize().x;

		vPosInGroup.y += mpChBBloom->GetSize().y + fInputSep;

		// ImageTrail
		mpChBImageTrail = mpGuiSet->CreateWidgetCheckBox(vPosInGroup, 0, kTranslate("OptionsMenu","ImageTrail"), pGroup);
		SetUpInput(NULL, mpChBImageTrail, false, kTranslate("OptionsMenu","ImageTrailTip"));

		if(fMaxWidth < mpChBImageTrail->GetSize().x)
			fMaxWidth = mpChBImageTrail->GetSize().x;

		vPosInGroup.y = fBorderSize;
		//vPosInGroup.x += fMaxWidth + fInputSep;
		//fMaxWidth = 0;

		vPosInGroup.x += fItemSep;

		// Sepia
		mpChBSepia = mpGuiSet->CreateWidgetCheckBox(vPosInGroup, 0, kTranslate("OptionsMenu","Sepia"), pGroup);
		SetUpInput(NULL, mpChBSepia, false, kTranslate("OptionsMenu","SepiaTip"));

		if(fMaxWidth < mpChBBloom->GetSize().x)
			fMaxWidth = mpChBBloom->GetSize().x;

		vPosInGroup.y += mpChBSepia->GetSize().y + fInputSep;

		// RadialBlur
		mpChBRadialBlur = mpGuiSet->CreateWidgetCheckBox(vPosInGroup, 0, kTranslate("OptionsMenu","RadialBlur"), pGroup);
		SetUpInput(NULL, mpChBRadialBlur, false, kTranslate("OptionsMenu","RadialBlurTip"));

		if(fMaxWidth < mpChBRadialBlur->GetSize().x)
			fMaxWidth = mpChBRadialBlur->GetSize().x;

		vPosInGroup.y = fBorderSize;
		//vPosInGroup.x += fMaxWidth + fInputSep;
		vPosInGroup.x += fItemSep;

		// Insanity
		mpChBInsanity = mpGuiSet->CreateWidgetCheckBox(vPosInGroup, 0, kTranslate("OptionsMenu","Insanity"), pGroup);
		SetUpInput(NULL, mpChBInsanity, false, kTranslate("OptionsMenu","InsanityTip"));
	}

	vPos.y += pGroup->GetSize().y + 10;

	////////////////////////////
	// Supersampling
	// Taller than the shared group size: three label+combo rows, a wrapping
	// help line, and a wrapping status line. The other groups keep vGroupSize.
	cVector2f vSuperSamplingGroupSize = cVector2f(vGroupSize.x, 220);
	pGroup = mpGuiSet->CreateWidgetGroup(vPos, vSuperSamplingGroupSize,
		TranslateOrDefault("OptionsMenu", "SuperSampling", _W("Supersampling")), pMainFrame);
	{
		float fBorderSize = 15;
		float fSuperSamplingItemSep = 240;
		cVector3f vPosInGroup = cVector3f(fBorderSize, fBorderSize, 0.1f);

		/////////////////////////////
		// Method
		pLabel = mpGuiSet->CreateWidgetLabel(vPosInGroup, -1,
			TranslateOrDefault("OptionsMenu", "SuperSamplingMethod", _W("Method")), pGroup);
		mpCBTemporalUpscaler = mpGuiSet->CreateWidgetComboBox(cVector3f(0,pLabel->GetSize().y+5,0), cVector2f(220,25), _W(""), pLabel);
		SetUpInput(pLabel, mpCBTemporalUpscaler, false,
			TranslateOrDefault("OptionsMenu", "SuperSamplingMethodTip",
				_W("A lower input resolution trades detail for performance; Native AA keeps native resolution.")));
		mpCBTemporalUpscaler->AddCallback(eGuiMessage_SelectionChange, this, kGuiCallback(TemporalUpscaler_OnProviderChange));

		vPosInGroup.x += fSuperSamplingItemSep;

		/////////////////////////////
		// Quality
		pLabel = mpGuiSet->CreateWidgetLabel(vPosInGroup, -1,
			TranslateOrDefault("OptionsMenu", "Quality", _W("Quality")), pGroup);
		mpCBTemporalUpscalerQuality = mpGuiSet->CreateWidgetComboBox(cVector3f(0,pLabel->GetSize().y+5,0), cVector2f(220,25), _W(""), pLabel);
		SetUpInput(pLabel, mpCBTemporalUpscalerQuality, false,
			TranslateOrDefault("OptionsMenu", "SuperSamplingQualityTip",
				_W("A lower input resolution trades detail for performance; Native AA keeps native resolution.")));
		mpCBTemporalUpscalerQuality->AddCallback(eGuiMessage_SelectionChange, this, kGuiCallback(TemporalUpscaler_OnQualityChange));

		float fSuperSamplingRowHeight = pLabel->GetSize().y + 5 + mpCBTemporalUpscalerQuality->GetSize().y;
		vPosInGroup.x = fBorderSize;
		vPosInGroup.y = fBorderSize + fSuperSamplingRowHeight + 10;

		/////////////////////////////
		// Render scale
		pLabel = mpGuiSet->CreateWidgetLabel(vPosInGroup, -1,
			TranslateOrDefault("OptionsMenu", "RenderScale", _W("Render scale")), pGroup);
		mpCBRenderScale = mpGuiSet->CreateWidgetComboBox(cVector3f(0,pLabel->GetSize().y+5,0), cVector2f(220,25), _W(""), pLabel);
		SetUpInput(pLabel, mpCBRenderScale, false,
			TranslateOrDefault("OptionsMenu", "RenderScaleTip",
				_W("Renders the scene at a percentage of the output resolution and scales the result to the window.")));
		mpCBRenderScale->AddCallback(eGuiMessage_SelectionChange, this, kGuiCallback(RenderScale_OnChange));

		float fRenderScaleRowHeight = pLabel->GetSize().y + 5 + mpCBRenderScale->GetSize().y;
		float fRenderScaleHelpY = vPosInGroup.y + fRenderScaleRowHeight + 10;
		mpLRenderScaleHelp = mpGuiSet->CreateWidgetLabel(cVector3f(fBorderSize, fRenderScaleHelpY, 0.1f),
			cVector2f(vGroupSize.x-fBorderSize*2, 35),
			TranslateOrDefault("OptionsMenu", "RenderScaleHelp",
				_W("FSR and XeSS choose the render resolution from Quality. The saved render scale applies when supersampling is Off or unavailable.")), pGroup);
		mpLRenderScaleHelp->SetWordWrap(true);

		float fTemporalUpscalerStatusY = fRenderScaleHelpY + mpLRenderScaleHelp->GetSize().y + 5;
		mpLTemporalUpscalerStatus = mpGuiSet->CreateWidgetLabel(cVector3f(fBorderSize, fTemporalUpscalerStatusY, 0.1f),
			cVector2f(vGroupSize.x-fBorderSize*2, 35), _W(""), pGroup);
		mpSuperSamplingLoggedReason = NULL;
		msSuperSamplingStatusText = _W("");
		mpLTemporalUpscalerStatus->SetWordWrap(true);

		vSuperSamplingGroupSize.y = fTemporalUpscalerStatusY + mpLTemporalUpscalerStatus->GetSize().y + fBorderSize;
		pGroup->SetSize(vSuperSamplingGroupSize);
	}

	vPos.y += pGroup->GetSize().y + 10;

	////////////////////////////
	// Water Group
	//pGroup = mpGuiSet->CreateWidgetGroup(vPos, vGroupSize, _W("Water"), pMainFrame);
	//{
	//	float fBorderSize = 15;
	//	cVector3f vPosInGroup = cVector3f(fBorderSize, fBorderSize, 0.1f);
	//}

	//vPos.x = fBorderSize;
	//vPos.y += pGroup->GetSize().y + 10;

	////////////////////////////
	// Misc
	vGroupSize.y = 70;
	pGroup = mpGuiSet->CreateWidgetGroup(vPos, vGroupSize, kTranslate("KeyConfig","Misc"), pMainFrame);
	{
		float fBorderSize = 15;
		cVector3f vPosInGroup = cVector3f(fBorderSize, fBorderSize, 0.1f);

		// Enabled
		mpChEdgeSmooth = mpGuiSet->CreateWidgetCheckBox(vPosInGroup, 0, kTranslate("OptionsMenu","FullscreenSmooth"), pGroup);
		SetUpInput(NULL, mpChEdgeSmooth, true, kTranslate("OptionsMenu","EdgeSmoothTip"));

		vPosInGroup.y += mpChEdgeSmooth->GetSize().y + 10;

		mpChBRefraction = mpGuiSet->CreateWidgetCheckBox(vPosInGroup, 0, kTranslate("OptionsMenu", "Refraction"), pGroup);
		SetUpInput(NULL, mpChBRefraction, true, kTranslate("OptionsMenu", "RefractionTip"));

		vPosInGroup.x += fItemSep;
		vPosInGroup.y = fBorderSize;

		///////////////////////////
		// World Reflection Active
		mpChBWorldReflection = mpGuiSet->CreateWidgetCheckBox(vPosInGroup, 0, kTranslate("OptionsMenu","WorldReflection") + _W(" (") + kTranslate("OptionsMenu", "Water")+_W(")"), pGroup);
		SetUpInput(NULL, mpChBWorldReflection, false, kTranslate("OptionsMenu","WorldReflectionTip"));
	}

	vPos.y += pGroup->GetSize().y + 10;
	// Small bottom margin; the frame derives its scroll range from its children's bounds.
	mpGuiSet->CreateWidgetDummy(vPos + cVector3f(0, 10, 0), pMainFrame);

	//////////////
	// Setup gamepad navigation
	mpCBTextureFilter; mpCBAnisotropy; mpCBParallaxQuality;

	mpChBShadows; mpCBShadowQuality; mpCBShadowRes;

	mpChBBloom; mpChBSepia; mpChBInsanity;
	mpChBImageTrail; mpChBRadialBlur;

	mpChEdgeSmooth;  mpChBWorldReflection;
	mpChBRefraction;

	{
		mpCBTextureFilter->SetFocusNavigation(eUIArrow_Right, mpCBAnisotropy);
		mpCBAnisotropy->SetFocusNavigation(eUIArrow_Right, mpCBParallaxQuality);

		mpCBAnisotropy->SetFocusNavigation(eUIArrow_Left, mpCBTextureFilter);
		mpCBParallaxQuality->SetFocusNavigation(eUIArrow_Left, mpCBAnisotropy);

		mpCBTextureFilter->SetFocusNavigation(eUIArrow_Down, mpChBShadows);
		mpCBAnisotropy->SetFocusNavigation(eUIArrow_Down, mpCBShadowQuality);
		mpCBParallaxQuality->SetFocusNavigation(eUIArrow_Down, mpCBShadowRes);
	}

	{
		mpChBShadows->SetFocusNavigation(eUIArrow_Up, mpCBTextureFilter);
		mpCBShadowQuality->SetFocusNavigation(eUIArrow_Up, mpCBAnisotropy);
		mpCBShadowRes->SetFocusNavigation(eUIArrow_Up, mpCBParallaxQuality);

		mpChBShadows->SetFocusNavigation(eUIArrow_Right, mpCBShadowQuality);
		mpCBShadowQuality->SetFocusNavigation(eUIArrow_Right, mpCBShadowRes);

		mpCBShadowQuality->SetFocusNavigation(eUIArrow_Left, mpChBShadows);
		mpCBShadowRes->SetFocusNavigation(eUIArrow_Left, mpCBShadowQuality);

		mpChBShadows->SetFocusNavigation(eUIArrow_Down, mpChBBloom);
		mpCBShadowQuality->SetFocusNavigation(eUIArrow_Down, mpChBSepia);
		mpCBShadowRes->SetFocusNavigation(eUIArrow_Down, mpChBInsanity);
	}

	{
		mpChBBloom->SetFocusNavigation(eUIArrow_Up, mpChBShadows);
		mpChBSepia->SetFocusNavigation(eUIArrow_Up, mpCBShadowQuality);
		mpChBInsanity->SetFocusNavigation(eUIArrow_Up, mpCBShadowRes);

		mpChBBloom->SetFocusNavigation(eUIArrow_Right, mpChBSepia);
		mpChBSepia->SetFocusNavigation(eUIArrow_Right, mpChBInsanity);

		mpChBSepia->SetFocusNavigation(eUIArrow_Left, mpChBBloom);
		mpChBInsanity->SetFocusNavigation(eUIArrow_Left, mpChBSepia);

		mpChBBloom->SetFocusNavigation(eUIArrow_Down, mpChBImageTrail);
		mpChBSepia->SetFocusNavigation(eUIArrow_Down, mpChBRadialBlur);
		mpChBInsanity->SetFocusNavigation(eUIArrow_Down, mpChBRadialBlur);

		mpChBImageTrail->SetFocusNavigation(eUIArrow_Up, mpChBBloom);
		mpChBRadialBlur->SetFocusNavigation(eUIArrow_Up, mpChBSepia);

		mpChBImageTrail->SetFocusNavigation(eUIArrow_Right, mpChBRadialBlur);
		mpChBRadialBlur->SetFocusNavigation(eUIArrow_Left, mpChBImageTrail);

		mpChBImageTrail->SetFocusNavigation(eUIArrow_Down, mpCBTemporalUpscaler);
		mpChBRadialBlur->SetFocusNavigation(eUIArrow_Down, mpCBTemporalUpscalerQuality);
	}

	{
		mpChEdgeSmooth->SetFocusNavigation(eUIArrow_Up, mpCBRenderScale);
		mpChBWorldReflection->SetFocusNavigation(eUIArrow_Up, mpCBRenderScale);

		mpChEdgeSmooth->SetFocusNavigation(eUIArrow_Right, mpChBWorldReflection);
		mpChBWorldReflection->SetFocusNavigation(eUIArrow_Left, mpChEdgeSmooth);

		mpChEdgeSmooth->SetFocusNavigation(eUIArrow_Down, mpChBRefraction);
		mpChBWorldReflection->SetFocusNavigation(eUIArrow_Down, mpChBRefraction);

		mpChBRefraction->SetFocusNavigation(eUIArrow_Up, mpChEdgeSmooth);

		mpCBTemporalUpscaler->SetFocusNavigation(eUIArrow_Up, mpChBImageTrail);
		mpCBTemporalUpscalerQuality->SetFocusNavigation(eUIArrow_Up, mpChBRadialBlur);
		mpCBTemporalUpscaler->SetFocusNavigation(eUIArrow_Right, mpCBTemporalUpscalerQuality);
		mpCBTemporalUpscalerQuality->SetFocusNavigation(eUIArrow_Left, mpCBTemporalUpscaler);
		mpCBTemporalUpscaler->SetFocusNavigation(eUIArrow_Down, mpCBRenderScale);
		mpCBTemporalUpscalerQuality->SetFocusNavigation(eUIArrow_Down, mpCBRenderScale);
		mpCBRenderScale->SetFocusNavigation(eUIArrow_Up, mpCBTemporalUpscaler);
		mpCBRenderScale->SetFocusNavigation(eUIArrow_Down, mpChEdgeSmooth);
	}
}

//-----------------------------------------------------------------------

void cLuxMainMenu_Options::AddInputOptions(cWidgetTab* apTab)
{
	float fBorderSize = 15;
	cVector3f vPos(fBorderSize, 6 + fBorderSize + 10, 0.1f);

	/////////////////////////////
	// Invert Mouse
    mpChBInvertMouse = mpGuiSet->CreateWidgetCheckBox(vPos, 0, kTranslate("OptionsMenu", "InvertMouse"), apTab);
	SetUpInput(NULL, mpChBInvertMouse, false, kTranslate("OptionsMenu","InvertMouseTip"));

	vPos.y += mpChBInvertMouse->GetSize().y + 15;

	/////////////////////////////
	// Smooth Mouse
	mpChBSmoothMouse = mpGuiSet->CreateWidgetCheckBox(vPos, 0, kTranslate("OptionsMenu", "SmoothMouse"), apTab);
	SetUpInput(NULL, mpChBSmoothMouse, false, kTranslate("OptionsMenu","SmoothMouseTip"));

	vPos.y += mpChBSmoothMouse->GetSize().y + 15;

	/////////////////////////////
	// Mouse Sensitivity
	cWidgetLabel* pLabel = mpGuiSet->CreateWidgetLabel(vPos, -1, kTranslate("OptionsMenu", "MouseSensitivity"), apTab);
	mpSMouseSensitivity = mpGuiSet->CreateWidgetSlider(eWidgetSliderOrientation_Horizontal, cVector3f(0,pLabel->GetSize().y+5,0), cVector2f(100,20), 0, pLabel);
	//mpSMouseSensitivity->AddCallback(eGuiMessage_SliderMove, this, kGuiCallback(MouseSensitivitySlider_OnMove));
	SetUpInput(pLabel, mpSMouseSensitivity, false, kTranslate("OptionsMenu", "MouseSensitivityTip"));
	SetUpSlider(mpSMouseSensitivity, mfMouseSensitivityMin, mfMouseSensitivityMax, mfMouseSensitivityStep, kGuiCallback(MouseSensitivitySlider_OnMove), &mpLMouseSensitivity);

	//mpLMouseSensitivity = mpGuiSet->CreateWidgetLabel(cVector3f(mpSMouseSensitivity->GetSize().x*0.5f,2,1), -1, _W(""), mpSMouseSensitivity);
	//mpLMouseSensitivity->SetTextAlign(eFontAlign_Center);

	vPos.y += mpSMouseSensitivity->GetLocalPosition().y + mpSMouseSensitivity->GetSize().y + 15;

#ifdef USE_GAMEPAD
	//////////////////////////////////////////////////
	// Gamepad stuff
	mpChBGamepadInvertLook = NULL;
	mpLGamepadLookSensitivity = NULL;
	mpSGamepadLookSensitivity = NULL;

	/////////////////////////////
	// Invert Gamepad look
	mpChBGamepadInvertLook = mpGuiSet->CreateWidgetCheckBox(vPos, 0, kTranslate("OptionsMenu", "InvertGamepadLook"), apTab);
	SetUpInput(NULL, mpChBGamepadInvertLook, false, kTranslate("OptionsMenu","InvertGamepadLookTip"));

	vPos.y += mpChBGamepadInvertLook->GetSize().y + 15;

	/////////////////////////////
	// Gamepad Sensitivity
	pLabel = mpGuiSet->CreateWidgetLabel(vPos, -1, kTranslate("OptionsMenu", "GamepadLookSensitivity"), apTab);
	mpSGamepadLookSensitivity = mpGuiSet->CreateWidgetSlider(eWidgetSliderOrientation_Horizontal, cVector3f(0,pLabel->GetSize().y+5,0), cVector2f(100,20), 0, pLabel);
	//mpSGamepadLookSensitivity->AddCallback(eGuiMessage_SliderMove, this, kGuiCallback(MouseSensitivitySlider_OnMove));
	SetUpInput(pLabel, mpSMouseSensitivity, false, kTranslate("OptionsMenu", "GamepadLookSensitivityTip"));
	SetUpSlider(mpSGamepadLookSensitivity, mfGamepadLookSensitivityMin, mfGamepadLookSensitivityMax, mfGamepadLookSensitivityStep, kGuiCallback(GamepadLookSensitivitySlider_OnMove), &mpLGamepadLookSensitivity);

	vPos.y += mpSGamepadLookSensitivity->GetLocalPosition().y + mpSGamepadLookSensitivity->GetSize().y + 15;

	//mpLMouseSensitivity = mpGuiSet->CreateWidgetLabel(cVector3f(mpSMouseSensitivity->GetSize().x*0.5f,2,1), -1, _W(""), mpSMouseSensitivity);
	//mpLMouseSensitivity->SetTextAlign(eFontAlign_Center);
#endif

	//////////////////////////////
	// Key Config Button
	mpBKeyConfig = mpGuiSet->CreateWidgetButton(vPos, cVector2f(0,25), kTranslate("OptionsMenu","KeyConfigButton"), apTab);
	float fButtonWidth = mpBKeyConfig->GetDefaultFontType()->GetLength(mpBKeyConfig->GetDefaultFontSize(), mpBKeyConfig->GetText().c_str());
	mpBKeyConfig->SetSize(cVector2f(fButtonWidth+20, mpBKeyConfig->GetSize().y));
	SetUpInput(NULL, mpBKeyConfig, false, kTranslate("OptionsMenu", "KeyConfigButtonTip"));
	mpBKeyConfig->AddCallback(eGuiMessage_ButtonPressed, this, kGuiCallback(PressKeyConfig));



	//////////////////////////////////////////////////////////////////////////////////
	// Set up navigation

	apTab->SetUserData(mpBKeyConfig);
	apTab->GetTabLabel()->SetUserData(mpChBInvertMouse);

	// Down
	mpChBInvertMouse->SetFocusNavigation(eUIArrow_Down, mpChBSmoothMouse);
	mpChBSmoothMouse->SetFocusNavigation(eUIArrow_Down, mpSMouseSensitivity);
	mpSMouseSensitivity->SetFocusNavigation(eUIArrow_Down, mpBKeyConfig);
#ifdef USE_GAMEPAD
	mpSMouseSensitivity->SetFocusNavigation(eUIArrow_Down, mpChBGamepadInvertLook);
	mpChBGamepadInvertLook->SetFocusNavigation(eUIArrow_Down, mpSGamepadLookSensitivity);
	mpSGamepadLookSensitivity->SetFocusNavigation(eUIArrow_Down, mpBKeyConfig);
#endif
	mpBKeyConfig->SetFocusNavigation(eUIArrow_Down, mpBOK);

	// Up
	mpChBSmoothMouse->SetFocusNavigation(eUIArrow_Up, mpChBInvertMouse);
	mpSMouseSensitivity->SetFocusNavigation(eUIArrow_Up, mpChBSmoothMouse);
	mpBKeyConfig->SetFocusNavigation(eUIArrow_Up, mpSMouseSensitivity);
#ifdef USE_GAMEPAD
	mpChBGamepadInvertLook->SetFocusNavigation(eUIArrow_Up, mpSMouseSensitivity);
	mpSGamepadLookSensitivity->SetFocusNavigation(eUIArrow_Up, mpChBGamepadInvertLook);
	mpBKeyConfig->SetFocusNavigation(eUIArrow_Up, mpSGamepadLookSensitivity);
#endif

}

//-----------------------------------------------------------------------

void cLuxMainMenu_Options::AddSoundOptions(cWidgetTab* apTab)
{
	float fBorderSize = 15;
	cVector3f vPos(fBorderSize, 6 + fBorderSize + 10, 0.1f);

	/////////////////////////////
	// Sound Device selector
	cWidgetLabel* pLabel = mpGuiSet->CreateWidgetLabel(vPos, -1, kTranslate("OptionsMenu", "SoundDevice"), apTab);
	vPos.y += pLabel->GetSize().y + 5;

	mpCBSndDevice = mpGuiSet->CreateWidgetComboBox(vPos, cVector2f(400, 25), _W(""), apTab);
	SetUpInput(pLabel, mpCBSndDevice, true, kTranslate("OptionsMenu", "SoundDeviceTip"));

	vPos.y += mpCBSndDevice->GetSize().y + 15;

	/////////////////////////////
	// Sound Master Volume
	pLabel = mpGuiSet->CreateWidgetLabel(vPos, -1, kTranslate("OptionsMenu", "Volume"), apTab);
	mpSVolume = mpGuiSet->CreateWidgetSlider(eWidgetSliderOrientation_Horizontal, vPos + cVector3f(0,pLabel->GetSize().y+5,0), cVector2f(100,20), 0, apTab);
	//mpSVolume->AddCallback(eGuiMessage_SliderMove, this, kGuiCallback(SoundSlider_OnMove));
	SetUpInput(pLabel, mpSVolume, false, kTranslate("OptionsMenu", "VolumeTip"));
	SetUpSlider(mpSVolume, mfVolumeMin, mfVolumeMax, mfVolumeStep, kGuiCallback(SoundSlider_OnMove), &mpLVolume);

	//mpLVolume = mpGuiSet->CreateWidgetLabel(cVector3f(mpSVolume->GetSize().x*0.5f,2,1), -1, _W(""), mpSVolume);
	//mpLVolume->SetTextAlign(eFontAlign_Center);

	vPos.y += mpSVolume->GetSize().y + 38;

	/////////////////////////////
	// HRTF
	mpChBHRTF = mpGuiSet->CreateWidgetCheckBox(vPos, 0, _W("OpenAL Soft HRTF"), apTab);

	// Tip translations (This is not ideal, but it prevents needing to ship the external language files.)
	tString sCurrentLang = gpBase->GetCurrentLanguage();
	tWString sHRTFTip;

	if (sCurrentLang == "brazilian_portuguese.lang")
		sHRTFTip = cString::ParseUnicodeString("[u65][u86][u73][u83][u79][u58][u32][u69][u115][u116][u101][u32][u114][u101][u99][u117][u114][u115][u111][u32][u233][u32][u99][u111][u110][u104][u101][u99][u105][u100][u111][u32][u112][u111][u114][u32][u99][u97][u117][u115][u97][u114][u32][u112][u114][u111][u98][u108][u101][u109][u97][u115][u32][u100][u101][u32][u225][u117][u100][u105][u111][u32][u110][u101][u115][u116][u101][u32][u106][u111][u103][u111][u46][u32][u82][u101][u99][u111][u109][u101][u110][u100][u97][u45][u115][u101][u32][u109][u97][u110][u116][u234][u45][u108][u111][u32][u100][u101][u115][u97][u116][u105][u118][u97][u100][u111][u46]");
	else if (sCurrentLang == "chinese.lang")
		sHRTFTip = cString::ParseUnicodeString("[u35686][u21578][uff1a][u27492][u21151][u33021][u5df2][u77e5][u4f1a][u5bfc][u81f4][u6e38][u620f][u97f3][u9891][u95ee][u9898][u3002][u5efa][u8bae][u7981][u7528][u6b64][u529f][u80fd][u3002]");
	else if (sCurrentLang == "french.lang")
		sHRTFTip = cString::ParseUnicodeString("[u65][u86][u69][u82][u84][u73][u83][u83][u69][u77][u69][u78][u84][u160][u58][u32][u99][u101][u116][u116][u101][u32][u102][u111][u110][u99][u116][u105][u111][u110][u110][u97][u108][u105][u116][u233][u32][u101][u115][u116][u32][u99][u111][u110][u110][u117][u101][u32][u112][u111][u117][u114][u32][u112][u114][u111][u118][u111][u113][u117][u101][u114][u32][u100][u101][u115][u32][u112][u114][u111][u98][u108][u232][u109][u101][u115][u32][u97][u117][u100][u105][u111][u32][u100][u97][u110][u115][u32][u99][u101][u32][u106][u101][u117][u46][u32][u73][u108][u32][u101][u115][u116][u32][u114][u101][u99][u111][u109][u109][u97][u110][u100][u233][u32][u100][u101][u32][u108][u97][u32][u100][u233][u115][u97][u99][u116][u105][u118][u101][u114][u46]");
	else if (sCurrentLang == "german.lang")
		sHRTFTip = cString::ParseUnicodeString("[u87][u65][u82][u78][u85][u78][u71][u58][u32][u68][u105][u101][u115][u101][u32][u70][u117][u110][u107][u116][u105][u111][u110][u32][u107][u97][u110][u110][u32][u105][u110][u32][u100][u105][u101][u115][u101][u109][u32][u83][u112][u105][u101][u108][u32][u122][u117][u32][u65][u117][u100][u105][u111][u112][u114][u111][u98][u108][u101][u109][u101][u110][u32][u102][u252][u104][u114][u101][u110][u46][u32][u69][u115][u32][u119][u105][u114][u100][u32][u101][u109][u112][u102][u111][u104][u108][u101][u110][u44][u32][u115][u105][u101][u32][u100][u101][u97][u107][u116][u105][u118][u105][u101][u114][u116][u32][u122][u117][u32][u108][u97][u115][u115][u101][u110][u46]");
	else if (sCurrentLang == "italian.lang")
		sHRTFTip = cString::ParseUnicodeString("[u65][u84][u84][u69][u78][u90][u73][u79][u78][u69][u58][u32][u113][u117][u101][u115][u116][u97][u32][u102][u117][u110][u122][u105][u111][u110][u101][u32][u232][u32][u110][u111][u116][u97][u32][u112][u101][u114][u32][u99][u97][u117][u115][u97][u114][u101][u32][u112][u114][u111][u98][u108][u101][u109][u105][u32][u97][u117][u100][u105][u111][u32][u105][u110][u32][u113][u117][u101][u115][u116][u111][u32][u103][u105][u111][u99][u111][u46][u32][u83][u105][u32][u99][u111][u110][u115][u105][u103][u108][u105][u97][u32][u100][u105][u32][u109][u97][u110][u116][u101][u110][u101][u114][u108][u97][u32][u100][u105][u115][u97][u116][u116][u105][u118][u97][u116][u97][u46]");
	else if (sCurrentLang == "russian.lang")
		sHRTFTip = cString::ParseUnicodeString("[u1042][u1053][u1048][u1052][u1040][u1053][u1048][u1045][u58][u32][u1069][u1090][u1072][u32][u1092][u1091][u1085][u1082][u1094][u1080][u1103][u44][u32][u1082][u1072][u1082][u32][u1080][u1079][u1074][u1077][u1101][u1090][u1085][u1086][u44][u32][u1074][u1099][u1079][u1099][u1074][u1072][u1077][u1090][u32][u1087][u1088][u1086][u1073][u1083][u1077][u1084][u1099][u32][u1089][u1086][u32][u1079][u1074][u1091][u1082][u1086][u1084][u32][u1074][u32][u1101][u1090][u1086][u1081][u32][u1080][u1075][u1088][u1077][u46][u32][u1056][u1077][u1082][u1086][u1084][u1077][u1085][u1076][u1091][u1077][u1090][u1089][u1103][u32][u1086][u1090][u1082][u1083][u1102][u1095][u1080][u1090][u1100][u32][u1077][u1105][u46]");
	else if (sCurrentLang == "spanish.lang")
		sHRTFTip = cString::ParseUnicodeString("[u65][u68][u86][u69][u82][u84][u69][u78][u67][u73][u65][u58][u32][u69][u115][u116][u97][u32][u102][u117][u110][u99][u105][u243][u110][u32][u112][u117][u101][u100][u101][u32][u99][u97][u117][u115][u97][u114][u32][u112][u114][u111][u98][u108][u101][u109][u97][u115][u32][u100][u101][u32][u97][u117][u100][u105][u111][u32][u101][u110][u32][u101][u115][u116][u101][u32][u106][u117][u101][u103][u111][u46][u32][u83][u101][u32][u114][u101][u99][u111][u109][u105][u101][u110][u100][u97][u32][u100][u101][u115][u97][u99][u116][u105][u118][u97][u114][u108][u97][u46]");
	else
		sHRTFTip = _W("WARNING: This feature is known to cause audio problems in this game. It is recommended to keep this disabled."); // English default

	SetUpInput(NULL, mpChBHRTF, true, sHRTFTip);

	mpCBSndDevice->SetFocusNavigation(eUIArrow_Down, mpSVolume);
	mpSVolume->SetFocusNavigation(eUIArrow_Up, mpCBSndDevice);
	mpSVolume->SetFocusNavigation(eUIArrow_Down, mpChBHRTF);
	mpChBHRTF->SetFocusNavigation(eUIArrow_Up, mpSVolume);
	mpChBHRTF->SetFocusNavigation(eUIArrow_Down, mpBOK);

	apTab->SetUserData(mpChBHRTF);
	apTab->GetTabLabel()->SetUserData(mpCBSndDevice);

	PopulateSoundDevices();
}

//-----------------------------------------------------------------------

void cLuxMainMenu_Options::SetInputValues(cResourceVarsObject& aObj)
{
	mbSettingInitialValues = true;
	////////////////////////////////
	// Game options
	{
		// Show hints
		mpChBShowSubtitles->SetChecked(aObj.GetVarBool("ShowSubtitles"),false);
		mpChBShowEffectSubtitles->SetChecked(aObj.GetVarBool("ShowEffectSubtitles"), false);
		mpChBShowHints->SetChecked(aObj.GetVarBool("ShowHints"), false);
		mpChBShowDeathHints->SetChecked(aObj.GetVarBool("ShowDeathHints"), false);

		mpChBShowCrosshair->SetChecked(aObj.GetVarBool("ShowCrosshair"), false);

		mpCBFocusIconStyle->SetSelectedItem(aObj.GetVarInt("FocusIconStyle"), false, false);
		if(mpCBFocusIconStyle->GetSelectedItem()==-1)
			mpCBFocusIconStyle->SetSelectedItem(0, false, true);

		if(mbShowCommentary)
		{
			mpChBShowCommentary->SetChecked(aObj.GetVarBool("ShowCommentary"), false);
		}

		// Language
		{
			tString sLang = aObj.GetVarString("Language", gpBase->msDefaultGameLanguage);
			int lLangIndex=0;
			for(int i=0;i<(int)mvLangFiles.size();++i)
			{
				if(sLang==cString::To8Char(mvLangFiles[i]))
				{
					lLangIndex = i;
					break;
				}
			}
			mpCBLanguage->SetSelectedItem(lLangIndex, false, false);
		}
	}
#ifndef USE_ONLIVE
	////////////////////////////////
	// Graphics options
	{

		/////////////////////////
		// Resolution
		{
			int lCurrentMode = -1;
			bool bGenerateCallback=false;
			tVideoModeVec vVidModes;
			cPlatform::GetAvailableVideoModes(vVidModes, 32);

			cVector2f vCurrentResf = aObj.GetVarVector2f("Resolution");
			cVideoMode vCurrentRes = cVideoMode(aObj.GetVarInt("Display"), cVector2l((int)vCurrentResf.x, (int)vCurrentResf.y), -1, -1);

			/////////////////
			// Remove duplicates
			for(size_t i=0;i<vVidModes.size();++i)
			{
				int lRemove = 0;
				int lRefreshRate = vVidModes[i].mlRefreshRate;

				//////////////
				// Move forward until there are no more matches
				for(size_t j=i+1;j<vVidModes.size();++j)
				{
					if(vVidModes[i].mvScreenSize == vVidModes[j].mvScreenSize)
					{
						lRemove++;
						lRefreshRate = cMath::Max(lRefreshRate, vVidModes[j].mlRefreshRate);
					}
					else break;
				}

				//////////////
				// Remove dupilcates and get the one with the highest refreshrate
				for(size_t j = i + lRemove; j < vVidModes.size(); j++)
				{
					vVidModes[j-lRemove] = vVidModes[j];
				}
				vVidModes[i].mlRefreshRate = lRefreshRate;

				while(lRemove--) vVidModes.pop_back();
			}

			mpCBResolution->ClearItems();
			// Rebuild the backing vector alongside the combobox so their indices
			// stay in lockstep. SetInputValues() runs on every Options open, so
			// without this clear mvScreenSizes accumulates and the selected item
			// index maps to the wrong cVideoMode on Apply.
			mvScreenSizes.clear();
			for(size_t i=0;i<vVidModes.size();++i)
			{
				const cVideoMode& mode = vVidModes[i];
                tWString sRes;
                if (mode.isFullScreenDesktop())
                {
                    sRes = kTranslate("OptionsMenu", "FullScreenDesktop") + _W(" ") + cPlatform::GetDisplayName(mode.mlDisplay);
                }
                else
                {
					sRes = cString::ToStringW(mode.mvScreenSize.x) + _W("x") + cString::ToStringW(mode.mvScreenSize.y);
            	}
// Since the same resolution on display 0 will have the same text as display 1, this won't work
//				if(mpCBResolution->HasItem(sRes))
//					continue;

				mpCBResolution->AddItem(sRes);
				mvScreenSizes.push_back(mode);

				if(mode==vCurrentRes)
					lCurrentMode = mpCBResolution->GetItemNum()-1;
			}
			if(lCurrentMode==-1)
			{
				tWString sRes = cString::ToStringW(vCurrentRes.mvScreenSize.x) + _W("x") + cString::ToStringW(vCurrentRes.mvScreenSize.y) + _W(" (Custom)");

				mpCBResolution->AddItem(sRes);
				mvScreenSizes.push_back(vCurrentRes);
				lCurrentMode = mpCBResolution->GetItemNum()-1;

				//lCurrentMode=0;
				//bGenerateCallback = true;
			}
			mpCBResolution->SetSelectedItem(lCurrentMode, true, false);
		}

		/////////////////////////
		// Fullscreen & vsync
		mpChBFullScreen->SetChecked(aObj.GetVarBool("FullScreen"), false);
		mpChBVSync->SetChecked(aObj.GetVarBool("VSync"), false);
//		mpChBAdaptiveVSync->SetChecked(aObj.GetVarBool("AdaptiveVsync"), false);

		/////////////////////////
		// Renderer
		SelectRendererBackend(mpCBRendererBackend, cLuxConfigHandler::RendererBackendFromString(aObj.GetVarString("RendererBackend", "standard")), false);

		/////////////////////////
		// Texture quality and filtering
		{
			/////////////////////////////////
			// Texture Quality
			mpCBTextureSizeLevel->SetSelectedItem((mpCBTextureSizeLevel->GetItemNum()-1) - aObj.GetVarInt("TextureQuality"), true, false);

			/////////////////////////////////
			// Texture filtering
			tWStringVec vTexFilterStrings;
			vTexFilterStrings.push_back(kTranslate("OptionsMenu","Nearest"));
			vTexFilterStrings.push_back(kTranslate("OptionsMenu","Bilinear"));
			vTexFilterStrings.push_back(kTranslate("OptionsMenu","Trilinear"));

			mpCBTextureFilter->ClearItems();
			for(int i=0;i<(int)vTexFilterStrings.size();++i)
				mpCBTextureFilter->AddItem(vTexFilterStrings[i]);

			mpCBTextureFilter->SetSelectedItem(aObj.GetVarInt("TextureFilter"), true, false);

			/////////////////////////////////
			// Anisotropy
            mpCBAnisotropy->ClearItems();
			mpCBAnisotropy->AddItem(kTranslate("OptionsMenu","Off"));
			int lAnisoPow=2;
			for(int i=0; i<4; ++i)
			{
				cWidgetItem *pItem = mpCBAnisotropy->AddItem(cString::ToStringW(lAnisoPow)+_W("X"));
				lAnisoPow *= 2;
			}
			mpCBAnisotropy->SetSelectedItem(GetIndexFromAnisotropy(aObj.GetVarFloat("TextureAnisotropy")));
		}

		/////////////////////////
		// Temporal upscaling
		{
			const char* pReason = NULL;
			msTemporalUpscalerFsrUnavailableReason = "";
			msTemporalUpscalerXeSSUnavailableReason = "";
			mbTemporalUpscalerFsrAvailable = TemporalUpscalerAvailable(TemporalUpscalerProvider::Fsr, &pReason);
			if(mbTemporalUpscalerFsrAvailable == false)
			{
				msTemporalUpscalerFsrUnavailableReason = pReason ? pReason : "unavailable";
				Warning("Temporal upscaler %s unavailable: %s\n",
					cString::To8Char(TemporalUpscalerProviderToDisplay(TemporalUpscalerProvider::Fsr)).c_str(),
					msTemporalUpscalerFsrUnavailableReason.c_str());
			}

			pReason = NULL;
			mbTemporalUpscalerXeSSAvailable = TemporalUpscalerAvailable(TemporalUpscalerProvider::XeSS, &pReason);
			if(mbTemporalUpscalerXeSSAvailable == false)
			{
				msTemporalUpscalerXeSSUnavailableReason = pReason ? pReason : "unavailable";
				Warning("Temporal upscaler %s unavailable: %s\n",
					cString::To8Char(TemporalUpscalerProviderToDisplay(TemporalUpscalerProvider::XeSS)).c_str(),
					msTemporalUpscalerXeSSUnavailableReason.c_str());
			}

			mSuperSamplingRequestedProvider = cLuxConfigHandler::SuperSamplingProviderFromString(aObj.GetVarString("SuperSamplingProvider", "off"));
			mSuperSamplingRequestedQuality = cLuxConfigHandler::SuperSamplingQualityFromString(aObj.GetVarString("SuperSamplingQuality", "quality"));
			mSuperSamplingUnavailableProvider = TemporalUpscalerProvider::Off;

			bool bUnavailableSavedProvider =
				(mSuperSamplingRequestedProvider == TemporalUpscalerProvider::Fsr && mbTemporalUpscalerFsrAvailable == false) ||
				(mSuperSamplingRequestedProvider == TemporalUpscalerProvider::XeSS && mbTemporalUpscalerXeSSAvailable == false);
			if(bUnavailableSavedProvider)
				mSuperSamplingUnavailableProvider = mSuperSamplingRequestedProvider;

			mbRebuildingTemporalUpscaler = true;
			mpCBTemporalUpscaler->SetSelectedItem(-1, false, false);
			mpCBTemporalUpscaler->ClearItems();
			int lProvider = 0;
			if(bUnavailableSavedProvider)
			{
				tWString sUnavailable = TemporalUpscalerProviderToDisplay(mSuperSamplingRequestedProvider) +
					_W(" (unavailable; using Off)");
				cWidgetItem* pItem = mpCBTemporalUpscaler->AddItem(
					TranslateOrDefault("OptionsMenu", "SuperSamplingUnavailable", sUnavailable));
				pItem->SetUserValue(-1);
				lProvider = 0;
			}

			cWidgetItem* pItem = mpCBTemporalUpscaler->AddItem(TranslateOrDefault("OptionsMenu", "Off", _W("Off")));
			pItem->SetUserValue((int)TemporalUpscalerProvider::Off);
			if(bUnavailableSavedProvider == false && mSuperSamplingRequestedProvider == TemporalUpscalerProvider::Off)
				lProvider = mpCBTemporalUpscaler->GetItemNum()-1;

			if(mbTemporalUpscalerFsrAvailable)
			{
				pItem = mpCBTemporalUpscaler->AddItem(TemporalUpscalerProviderToDisplay(TemporalUpscalerProvider::Fsr));
				pItem->SetUserValue((int)TemporalUpscalerProvider::Fsr);
				if(bUnavailableSavedProvider == false && mSuperSamplingRequestedProvider == TemporalUpscalerProvider::Fsr)
					lProvider = mpCBTemporalUpscaler->GetItemNum()-1;
			}
			if(mbTemporalUpscalerXeSSAvailable)
			{
				pItem = mpCBTemporalUpscaler->AddItem(TemporalUpscalerProviderToDisplay(TemporalUpscalerProvider::XeSS));
				pItem->SetUserValue((int)TemporalUpscalerProvider::XeSS);
				if(bUnavailableSavedProvider == false && mSuperSamplingRequestedProvider == TemporalUpscalerProvider::XeSS)
					lProvider = mpCBTemporalUpscaler->GetItemNum()-1;
			}
			mpCBTemporalUpscaler->SetSelectedItem(lProvider, true, false);

			mfRenderScaleRequested = cLuxConfigHandler::NormalizeRenderScale(aObj.GetVarFloat("RenderScale", 1.0f));
			mpCBRenderScale->SetSelectedItem(-1, false, false);
			mpCBRenderScale->ClearItems();
			for(int i=0; i<cLuxConfigHandler::GetRenderScalePresetNum(); ++i)
			{
				float fRenderScale = cLuxConfigHandler::GetRenderScalePreset(i);
				tWString sRenderScale = i == 0 ?
					TranslateOrDefault("OptionsMenu", "RenderScaleNative", _W("Native (100%)")) :
					cString::ToStringW(fRenderScale * 100.0f, 0, true) + _W("%");
				pItem = mpCBRenderScale->AddItem(sRenderScale);
				pItem->SetUserValue(i);
			}
			mpCBRenderScale->SetSelectedItem(
				cLuxConfigHandler::GetRenderScalePresetIndex(mfRenderScaleRequested), true, false);
			mbRebuildingTemporalUpscaler = false;

			RebuildTemporalUpscalerQualityList();
			RefreshTemporalUpscalerStatusLabel();
			RefreshRenderScaleControl();
		}

		/////////////////////////
		// Smoothing
		{
			//Enabled
			mpChEdgeSmooth->SetChecked(aObj.GetVarBool("EdgeSmooth"), false);
		}

		/////////////////////////
		// Shadows & Parallax
		{
			mpChBShadows->SetChecked(aObj.GetVarBool("ShadowsActive"), false);

			tWStringVec vOptionStrings;
			vOptionStrings.push_back(kTranslate("OptionsMenu","Low"));
			vOptionStrings.push_back(kTranslate("OptionsMenu","Medium"));
			vOptionStrings.push_back(kTranslate("OptionsMenu","High"));

			mpCBShadowQuality->ClearItems();
			mpCBShadowRes->ClearItems();
			mpCBParallaxQuality->ClearItems();
			for(int i=0;i<(int)vOptionStrings.size();++i)
			{
				mpCBShadowQuality->AddItem(vOptionStrings[i]);
				mpCBShadowRes->AddItem(vOptionStrings[i]);
			}

			mpCBShadowQuality->SetSelectedItem(aObj.GetVarInt("ShadowQuality"), true, false);
			mpCBShadowRes->SetSelectedItem(aObj.GetVarInt("ShadowResolution"), true, false);

			mpCBParallaxQuality->AddItem(kTranslate("Launcher","Off"));
			mpCBParallaxQuality->AddItem(kTranslate("Launcher","On"));//Skipping medium since high and medium is really the same!

			int lParallax = aObj.GetVarBool("ParallaxEnabled")? 1 : 0;
			mpCBParallaxQuality->SetSelectedItem(lParallax, true, false);
		}

		/////////////////////////
		// Water
		{
			mpChBWorldReflection->SetChecked(aObj.GetVarBool("WorldReflection"), false);
			mpChBRefraction->SetChecked(aObj.GetVarBool("Refraction"), false);
		}

		/////////////////
		// PostEffects
		{
			cLuxMapHandler* pMapHdlr = gpBase->mpMapHandler;
			cLuxPostEffectHandler *pPostEffects = gpBase->mpPostEffectHandler;
			// Bloom
			mpChBBloom->SetChecked(aObj.GetVarBool("BloomActive"), false);
			// ImageTrail
			mpChBImageTrail->SetChecked(aObj.GetVarBool("ImageTrailActive"), false);
			// Sepia
			mpChBSepia->SetChecked(aObj.GetVarBool("SepiaActive"), false);
			// RadialBlur
			mpChBRadialBlur->SetChecked(aObj.GetVarBool("RadialBlurActive"), false);
			//Insanity
			mpChBInsanity->SetChecked(aObj.GetVarBool("InsanityActive"), false);
		}

		// Gamma
		float fGamma = aObj.GetVarFloat("Gamma");
		SetSliderValue(mpSGamma, fGamma, false, mfGammaMin, mfGammaMax);
		SetGammaLabelString(fGamma);
	}
#endif
	////////////////////////////////
	// Input
	mpChBInvertMouse->SetChecked(aObj.GetVarBool("InvertMouse"), false);
	mpChBSmoothMouse->SetChecked(aObj.GetVarBool("SmoothMouse"), false);

	float fSensitivity = aObj.GetVarFloat("MouseSensitivity");
	SetSliderValue(mpSMouseSensitivity, fSensitivity, false, mfMouseSensitivityMin, mfMouseSensitivityMax);
	SetSensitivityLabelString(fSensitivity);

#ifdef USE_GAMEPAD
	mpChBGamepadInvertLook->SetChecked(aObj.GetVarBool("GamepadInvertLook"), false);

	fSensitivity = aObj.GetVarFloat("GamepadLookSensitivity");
	SetSliderValue(mpSGamepadLookSensitivity, fSensitivity, false, mfGamepadLookSensitivityMin, mfGamepadLookSensitivityMax);
	SetGamepadLookSensitivityLabelString(fSensitivity);
#endif

	////////////////////////////////
	// Sound
#ifndef USE_ONLIVE
	float fVolume = aObj.GetVarFloat("SoundVolume");
	SetSliderValue(mpSVolume, fVolume, false, mfVolumeMin, mfVolumeMax);
	SetVolumeLabelString(fVolume);

	int lSndDev = aObj.GetVarInt("SoundDeviceID");
	int lSndDevIdx = -1;
	for(size_t i=0;i<mvSoundDevices.size();++i)
	{
		if(lSndDev==mvSoundDevices[i]->GetID())
		{
			lSndDevIdx = (int)i;
			break;
		}
	}


	if(lSndDevIdx==-1)
        mpCBSndDevice->SetSelectedItem(mpCBSndDevice->GetItemNum()-1, true, false);
	else
		mpCBSndDevice->SetSelectedItem(lSndDevIdx, true, false);

	mpChBHRTF->SetChecked(aObj.GetVarBool("HRTFActive"), false);
	#endif

	RefreshRenderScaleControl();
	RefreshRendererBackendControl();
	mbSettingInitialValues = false;
}

//-----------------------------------------------------------------------

void cLuxMainMenu_Options::RebuildTemporalUpscalerQualityList()
{
	if(mpCBTemporalUpscaler == NULL || mpCBTemporalUpscalerQuality == NULL)
		return;

	mbRebuildingTemporalUpscaler = true;
	mpCBTemporalUpscalerQuality->SetSelectedItem(-1, false, false);
	mpCBTemporalUpscalerQuality->ClearItems();

	bool bProviderAvailable =
		(mSuperSamplingRequestedProvider == TemporalUpscalerProvider::Fsr && mbTemporalUpscalerFsrAvailable) ||
		(mSuperSamplingRequestedProvider == TemporalUpscalerProvider::XeSS && mbTemporalUpscalerXeSSAvailable);
	TemporalUpscalerProvider aEffectiveProvider = bProviderAvailable ? mSuperSamplingRequestedProvider : TemporalUpscalerProvider::Off;

	if(aEffectiveProvider == TemporalUpscalerProvider::Off)
	{
		cWidgetItem* pItem = mpCBTemporalUpscalerQuality->AddItem(
			TranslateOrDefault("OptionsMenu", "SuperSamplingQualityUnavailable", _W("Not available")));
		pItem->SetSelectable(false);
		pItem->SetUserValue((int)mSuperSamplingRequestedQuality);
		mpCBTemporalUpscalerQuality->SetSelectedItem(0, true, false);
		mpCBTemporalUpscalerQuality->SetEnabled(false);
	}
	else
	{
		TemporalUpscalerQuality vQualities[8];
		uint32_t lQualityNum = TemporalUpscalerAvailableQualities(aEffectiveProvider, vQualities, 8);
		if(lQualityNum == 0)
		{
			cWidgetItem* pItem = mpCBTemporalUpscalerQuality->AddItem(
				TranslateOrDefault("OptionsMenu", "SuperSamplingQualityUnavailable", _W("Not available")));
			pItem->SetSelectable(false);
			pItem->SetUserValue((int)mSuperSamplingRequestedQuality);
			mpCBTemporalUpscalerQuality->SetSelectedItem(0, true, false);
			mpCBTemporalUpscalerQuality->SetEnabled(false);
		}
		else
		{
			int lSelectedQuality = -1;
			int lDefaultQuality = -1;
			for(uint32_t i=0; i<lQualityNum; ++i)
			{
				cWidgetItem* pItem = mpCBTemporalUpscalerQuality->AddItem(TemporalUpscalerQualityToDisplay(vQualities[i]));
				pItem->SetUserValue((int)vQualities[i]);
				if(vQualities[i] == mSuperSamplingRequestedQuality)
					lSelectedQuality = (int)i;
				if(vQualities[i] == TemporalUpscalerQuality::Quality)
					lDefaultQuality = (int)i;
			}
			if(lSelectedQuality < 0 || lSelectedQuality >= (int)lQualityNum)
				lSelectedQuality = lDefaultQuality >= 0 ? lDefaultQuality : 0;
			mSuperSamplingRequestedQuality = vQualities[lSelectedQuality];
			mpCBTemporalUpscalerQuality->SetSelectedItem(lSelectedQuality, true, false);
			mpCBTemporalUpscalerQuality->SetEnabled(true);
		}
	}

	bool bQualityEnabled = mpCBTemporalUpscalerQuality->IsEnabled();
	bool bRenderScaleEnabled = mSuperSamplingRequestedProvider == TemporalUpscalerProvider::Off;
	mpChBImageTrail->SetFocusNavigation(eUIArrow_Down, mpCBTemporalUpscaler);
	mpChBRadialBlur->SetFocusNavigation(eUIArrow_Down, bQualityEnabled ? mpCBTemporalUpscalerQuality : mpCBTemporalUpscaler);
	mpCBTemporalUpscaler->SetFocusNavigation(eUIArrow_Up, mpChBImageTrail);
	mpCBTemporalUpscalerQuality->SetFocusNavigation(eUIArrow_Up, mpChBRadialBlur);
	// Right only leads somewhere while Quality is selectable; a disabled
	// Quality is skipped rather than focused.
	mpCBTemporalUpscaler->SetFocusNavigation(eUIArrow_Right, bQualityEnabled ? (iWidget*)mpCBTemporalUpscalerQuality : NULL);
	mpCBTemporalUpscalerQuality->SetFocusNavigation(eUIArrow_Left, mpCBTemporalUpscaler);
	mpCBTemporalUpscaler->SetFocusNavigation(eUIArrow_Down, bRenderScaleEnabled ? (iWidget*)mpCBRenderScale : mpChEdgeSmooth);
	mpCBTemporalUpscalerQuality->SetFocusNavigation(eUIArrow_Down, bRenderScaleEnabled ? (iWidget*)mpCBRenderScale : mpChEdgeSmooth);
	mpCBRenderScale->SetFocusNavigation(eUIArrow_Up, mpCBTemporalUpscaler);
	mpCBRenderScale->SetFocusNavigation(eUIArrow_Down, mpChEdgeSmooth);
	mpChEdgeSmooth->SetFocusNavigation(eUIArrow_Up, bRenderScaleEnabled ? (iWidget*)mpCBRenderScale : mpCBTemporalUpscaler);
	mpChBWorldReflection->SetFocusNavigation(eUIArrow_Up,
		bRenderScaleEnabled ? (iWidget*)mpCBRenderScale : (bQualityEnabled ? (iWidget*)mpCBTemporalUpscalerQuality : mpCBTemporalUpscaler));

	mbRebuildingTemporalUpscaler = false;
}

//-----------------------------------------------------------------------

void cLuxMainMenu_Options::RefreshTemporalUpscalerStatusLabel()
{
	if(mpCBTemporalUpscaler == NULL || mpCBTemporalUpscalerQuality == NULL || mpLTemporalUpscalerStatus == NULL)
		return;

	TemporalUpscalerSettings aSelected;
	aSelected.provider = mSuperSamplingRequestedProvider;
	aSelected.quality = mSuperSamplingRequestedQuality;

	bool bAppliedMode = gpBase != NULL && gpBase->mpConfigHandler != NULL &&
		mSuperSamplingRequestedProvider == gpBase->mpConfigHandler->mSuperSampling.provider &&
		mSuperSamplingRequestedQuality == gpBase->mpConfigHandler->mSuperSampling.quality;
	TemporalUpscalerStatus aStatus = {};
	TemporalUpscalerStatus aCapabilityStatus = {};
	bool bStatusUsable = false;
	bool bHaveCapabilityStatus = false;

	if(bAppliedMode)
	{
		cViewport* pViewport = gpBase->mpMainMenu ? gpBase->mpMainMenu->GetBackgroundViewport() : NULL;
		if(pViewport)
		{
			aStatus = pViewport->GetTemporalUpscalerStatus();
			bStatusUsable = aStatus.requestedProvider == aSelected.provider &&
				aStatus.requestedQuality == aSelected.quality;
		}

		bool bNeedsCapabilityStatus = bStatusUsable == false ||
			(aStatus.available == false && aStatus.unavailableReason == NULL) ||
			(aStatus.available && aStatus.effectiveProvider == TemporalUpscalerProvider::Off);
		if(aSelected.provider != TemporalUpscalerProvider::Off && bNeedsCapabilityStatus)
		{
			aCapabilityStatus = TemporalUpscalerQuery(aSelected);
			bHaveCapabilityStatus = true;
		}
	}
	else
	{
		aStatus = TemporalUpscalerQuery(aSelected);
		bStatusUsable = true;
	}

	const char* pUnavailableReason = NULL;
	if(bAppliedMode)
	{
		if(bStatusUsable)
			pUnavailableReason = aStatus.unavailableReason;
		else if(bHaveCapabilityStatus && aCapabilityStatus.available == false)
			pUnavailableReason = aCapabilityStatus.unavailableReason;
	}
	else if(aStatus.available == false)
	{
		pUnavailableReason = aStatus.unavailableReason;
	}

	if(pUnavailableReason != NULL)
	{
		bool bReasonChanged = pUnavailableReason != mpSuperSamplingLoggedReason;
		if(bReasonChanged && pUnavailableReason != NULL && mpSuperSamplingLoggedReason != NULL)
			bReasonChanged = std::strcmp(pUnavailableReason, mpSuperSamplingLoggedReason) != 0;
		if(bReasonChanged)
			Warning("Temporal upscaler status: %s\n", pUnavailableReason);
		mpSuperSamplingLoggedReason = pUnavailableReason;
	}
	else
	{
		mpSuperSamplingLoggedReason = NULL;
	}

	tWString sStatus;
	tWString sUnavailable = TemporalUpscalerProviderToDisplay(aSelected.provider) +
		_W(" is unavailable on this system; using Off.");
	if(bAppliedMode)
	{
		if(aSelected.provider == TemporalUpscalerProvider::Off)
		{
			sStatus = TranslateOrDefault("OptionsMenu", "SuperSamplingStatusOff", _W("Supersampling: Off."));
		}
		else if(bStatusUsable && aStatus.available && aStatus.effectiveProvider != TemporalUpscalerProvider::Off)
		{
			sStatus = TranslateOrDefault("OptionsMenu", "SuperSamplingStatus", _W("Supersampling: ")) +
				TemporalUpscalerProviderToDisplay(aStatus.effectiveProvider) + _W(" (") +
				TemporalUpscalerQualityToDisplay(aStatus.effectiveQuality) + _W(").");
		}
		else if(bStatusUsable && aStatus.available == false && aStatus.unavailableReason != NULL)
		{
			sStatus = sUnavailable;
		}
		else if(bHaveCapabilityStatus && aCapabilityStatus.available == false)
		{
			sStatus = sUnavailable;
		}
		else
		{
			sStatus = TranslateOrDefault("OptionsMenu", "SuperSamplingStatusPending",
				TemporalUpscalerProviderToDisplay(aSelected.provider) +
				_W(" selected; not active in this view."));
		}
	}
	else
	{
		if(aSelected.provider == TemporalUpscalerProvider::Off)
		{
			sStatus = TranslateOrDefault("OptionsMenu", "SuperSamplingStatusOff", _W("Supersampling: Off.")) +
				TranslateOrDefault("OptionsMenu", "SuperSamplingStatusAppliesOnOk", _W(" Applies when you press OK."));
		}
		else if(aStatus.available == false)
		{
			sStatus = sUnavailable;
		}
		else
		{
			sStatus = TranslateOrDefault("OptionsMenu", "SuperSamplingStatusPreview",
				TranslateOrDefault("OptionsMenu", "SuperSamplingStatus", _W("Supersampling: ")) +
				TemporalUpscalerProviderToDisplay(aSelected.provider) + _W(" (") +
				TemporalUpscalerQualityToDisplay(aSelected.quality) +
				_W(") applies when you press OK."));
		}
	}

	if(sStatus != msSuperSamplingStatusText)
	{
		mpLTemporalUpscalerStatus->SetText(sStatus);
		msSuperSamplingStatusText = sStatus;
	}
}

//-----------------------------------------------------------------------

void cLuxMainMenu_Options::RefreshRenderScaleControl()
{
	if(mpCBRenderScale == NULL)
		return;

	bool bRenderScaleEnabled = mSuperSamplingRequestedProvider == TemporalUpscalerProvider::Off;
	mpCBRenderScale->SetEnabled(bRenderScaleEnabled);

	if(mpCBTemporalUpscaler == NULL || mpCBTemporalUpscalerQuality == NULL ||
		mpChEdgeSmooth == NULL || mpChBWorldReflection == NULL)
		return;

	bool bQualityEnabled = mpCBTemporalUpscalerQuality->IsEnabled();
	mpCBTemporalUpscaler->SetFocusNavigation(eUIArrow_Down,
		bRenderScaleEnabled ? (iWidget*)mpCBRenderScale : mpChEdgeSmooth);
	mpCBTemporalUpscalerQuality->SetFocusNavigation(eUIArrow_Down,
		bRenderScaleEnabled ? (iWidget*)mpCBRenderScale : mpChEdgeSmooth);
	mpCBRenderScale->SetFocusNavigation(eUIArrow_Up, mpCBTemporalUpscaler);
	mpCBRenderScale->SetFocusNavigation(eUIArrow_Down, mpChEdgeSmooth);
	mpChEdgeSmooth->SetFocusNavigation(eUIArrow_Up,
		bRenderScaleEnabled ? (iWidget*)mpCBRenderScale : mpCBTemporalUpscaler);
	mpChBWorldReflection->SetFocusNavigation(eUIArrow_Up,
		bRenderScaleEnabled ? (iWidget*)mpCBRenderScale :
		(bQualityEnabled ? (iWidget*)mpCBTemporalUpscalerQuality : mpCBTemporalUpscaler));
}

//-----------------------------------------------------------------------

void cLuxMainMenu_Options::RefreshRendererBackendControl()
{
	if(mpCBRendererBackend == NULL || mpCBResolution == NULL ||
		mpChBVSync == NULL || mpCBTextureSizeLevel == NULL)
		return;

	// The engine already started Standard on a GPU without ray tracing, so lock
	// the choice there and say why instead of offering a backend that can't run.
	bool bOverdriveSupported = gpBase->mpEngine->GetGraphics()->IsOverdriveSupported();
	if(bOverdriveSupported == false)
		SelectRendererBackend(mpCBRendererBackend, hpl::eRendererBackend_Standard, false);
	mpCBRendererBackend->SetEnabled(bOverdriveSupported);
	if(mpLRendererBackendHelp)
		mpLRendererBackendHelp->SetVisible(bOverdriveSupported == false);

	cLuxOption_ExtData* pData = (cLuxOption_ExtData*)mpCBRendererBackend->GetUserData();
	if(pData)
	{
		pData->msMessage = RendererBackendTip(bOverdriveSupported);
		pData->mbNeedsRestart = bOverdriveSupported;
	}

	// A disabled widget cannot take focus, so route around it.
	iWidget* pBelowScreen = bOverdriveSupported ? (iWidget*)mpCBRendererBackend : mpCBTextureSizeLevel;
	mpCBResolution->SetFocusNavigation(eUIArrow_Down, pBelowScreen);
	mpChBVSync->SetFocusNavigation(eUIArrow_Down, pBelowScreen);
	mpCBTextureSizeLevel->SetFocusNavigation(eUIArrow_Up,
		bOverdriveSupported ? (iWidget*)mpCBRendererBackend : mpCBResolution);
}

//-----------------------------------------------------------------------

void cLuxMainMenu_Options::SetUpInput(cWidgetLabel* apLabel, iWidget* apInput, bool abNeedsRestart, const tWString& asMessage)
{
	cLuxOption_ExtData* pData = AddOptionData(abNeedsRestart, asMessage);
	if(apLabel)
	{
		apLabel->SetUserData(pData);
		apLabel->AddCallback(eGuiMessage_MouseMove, this, kGuiCallback(Option_OnMouseOver));
		apLabel->AddCallback(eGuiMessage_GetUINavFocus, this, kGuiCallback(Option_OnMouseOver));
	}
	apInput->SetUserData(pData);
	apInput->AddCallback(eGuiMessage_MouseMove, this, kGuiCallback(Option_OnMouseOver));
	apInput->AddCallback(eGuiMessage_GetUINavFocus, this, kGuiCallback(Option_OnMouseOver));

	eGuiMessage message = eGuiMessage_LastEnum;
	switch(apInput->GetType())
	{
	case eWidgetType_CheckBox:
		message = eGuiMessage_CheckChange;
		break;
	case eWidgetType_ComboBox:
		message = eGuiMessage_SelectionChange;
		break;
	case eWidgetType_Slider:
		message = eGuiMessage_SliderMove;
		break;
	case eWidgetType_Button:
		message = eGuiMessage_ButtonPressed;
		break;
	}
	if(message!=eGuiMessage_LastEnum)
		apInput->AddCallback(message, this, kGuiCallback(Option_OnChangeValue));
}

//-----------------------------------------------------------------------

cLuxOption_ExtData* cLuxMainMenu_Options::AddOptionData(bool abNeedsRestart, const tWString& asMessage)
{
	mvOptionData.push_back(hplNew(cLuxOption_ExtData,(abNeedsRestart, asMessage)));
	return mvOptionData.back();
}

//-----------------------------------------------------------------------

void cLuxMainMenu_Options::SetCurrentTipWidget(iWidget* apWidget)
{
	if(mpCurrentTipWidget==apWidget)
	{
		mbTipFadeRestart = true;
		return;
	}

	mbTipWidgetUpdated = true;
	mpCurrentTipWidget = apWidget;
}

//-----------------------------------------------------------------------


void cLuxMainMenu_Options::ApplyChanges()
{
	// Reset initial values on next enter
	mInitialValues.AddVarBool("InitialValuesSet", false);


	cLuxConfigHandler* pCfgHdr = gpBase->mpConfigHandler;
	///////////////////////////
	// Game
	{
		pCfgHdr->msLangFile = cString::To8Char(mvLangFiles[mpCBLanguage->GetSelectedItem()]);
		gpBase->mpMessageHandler->SetShowSubtitles(mpChBShowSubtitles->IsChecked());
		gpBase->mpMessageHandler->SetShowEffectSubtitles(mpChBShowEffectSubtitles->IsChecked());
		gpBase->mpHintHandler->SetActive(mpChBShowHints->IsChecked());
		gpBase->mpPlayer->GetHelperDeath()->SetShowHint(mpChBShowDeathHints->IsChecked());

		if(mbShowCommentary)
		{
			gpBase->mpMapHandler->SetShowCommentary(mpChBShowCommentary->IsChecked());
		}

		gpBase->mpPlayer->SetShowCrosshair(mpChBShowCrosshair->IsChecked());

		gpBase->mpPlayer->SetFocusIconStyle((eLuxFocusIconStyle)mpCBFocusIconStyle->GetSelectedItem());
	}


	///////////////////////////
	// Graphics
	{
		cGraphics* pGfx = gpBase->mpEngine->GetGraphics();
		cRenderSettings* pRenderSettings = gpBase->mpMapHandler->GetViewport()->GetRenderSettings();
		cMaterialManager* pMatMgr = gpBase->mpEngine->GetResources()->GetMaterialManager();

        const cVideoMode vidMode = mvScreenSizes[mpCBResolution->GetSelectedItem()];
		pCfgHdr->mvScreenSize = vidMode.mvScreenSize;
        pCfgHdr->mlDisplay = vidMode.mlDisplay;
		pCfgHdr->mbFullscreen = mpChBFullScreen->IsChecked();
		pCfgHdr->mbVSync = mpChBVSync->IsChecked();
//		pCfgHdr->mbAdaptiveVSync = mpChBAdaptiveVSync->IsChecked();
		gpBase->mpConfigHandler->SetGamma(GetGamma());

		// Apply resolution / fullscreen / vsync live: resize the OS window, then
		// recreate the swapchain at the actual new size. Viewports (via
		// GetTargetSize) and GUI sets (via GetVirtualSize) pull the new size
		// on their next use — no notification needed.
		pGfx->GetWindow()->SetSize(pCfgHdr->mvScreenSize, pCfgHdr->mbFullscreen);
		// Defer the swapchain rebuild to the next BeginActiveSet boundary (before
		// acquire) rather than rebuilding synchronously here in OnDraw. A mid-frame
		// rebuild strands the already-acquired image's semaphore and deadlocks the
		// next submit; the deferred path reads the actual window size at the
		// boundary and applies the new vsync.
		pGfx->SetVsync(pCfgHdr->mbVSync);

		// Parallax
		//int lParallax = (int)mpCBParallaxQuality->GetSelectedItem() - 1;
		//pCfgHdr->mlParallaxQuality = lParallax < 0  ? 0 : lParallax;
		//pCfgHdr->mbParallaxEnabled = lParallax <0 ? false : true;
		pCfgHdr->mbParallaxEnabled = (mpCBParallaxQuality->GetSelectedItem()==1);


		// Texture
		pCfgHdr->mlTextureQuality = (mpCBTextureSizeLevel->GetItemNum()-1) - mpCBTextureSizeLevel->GetSelectedItem();
		pCfgHdr->mlTextureFilter = mpCBTextureFilter->GetSelectedItem();
		pCfgHdr->mfTextureAnisotropy = GetAnisotropyFromIndex(mpCBAnisotropy->GetSelectedItem());

		pMatMgr->SetTextureAnisotropy(pCfgHdr->mfTextureAnisotropy);
		pMatMgr->SetTextureFilter((eTextureFilter)pCfgHdr->mlTextureFilter);

		// Shadows
		pCfgHdr->mbShadowsActive = mpChBShadows->IsChecked();
		pCfgHdr->mlShadowQuality = mpCBShadowQuality->GetSelectedItem();
		iRenderer::SetShadowMapQuality((eShadowMapQuality)pCfgHdr->mlShadowQuality);
		pCfgHdr->mlShadowRes = mpCBShadowRes->GetSelectedItem();

		// Water
		pCfgHdr->mbWorldReflection = mpChBWorldReflection->IsChecked();
		pCfgHdr->mbRefraction = mpChBRefraction->IsChecked();
		pCfgHdr->mSuperSampling.provider = mSuperSamplingRequestedProvider;
		pCfgHdr->mSuperSampling.quality = mSuperSamplingRequestedQuality;
		pCfgHdr->mRendererBackend = GetSelectedRendererBackend(mpCBRendererBackend);
		pCfgHdr->SetRenderScale(mfRenderScaleRequested);

		//Update the viewport stuff
		gpBase->mpMapHandler->UpdateViewportRenderProperties();

		// The main-menu background scene has its own viewport, so it needs the
		// new upscaler setting too; both write through to their render settings.
		if(gpBase->mpMainMenu)
			gpBase->mpMainMenu->RefreshSuperSamplingSettings();

		/////////////////////////
		// Smoothing
		{
			//Enabled
			pCfgHdr->mbEdgeSmooth = mpChEdgeSmooth->IsChecked();
		}

		/////////////////////
		// PostEffects
		{
			cLuxMapHandler* pMapHdlr = gpBase->mpMapHandler;
			cLuxPostEffectHandler *pPostEffects = gpBase->mpPostEffectHandler;

			// Bloom
			pMapHdlr->GetPostEffect_Bloom()->SetDisabled(mpChBBloom->IsChecked()==false);
			// ImageTrail
			pMapHdlr->GetPostEffect_ImageTrail()->SetDisabled(mpChBImageTrail->IsChecked()==false);
			// Sepia
			pMapHdlr->GetPostEffect_Sepia()->SetDisabled(mpChBSepia->IsChecked()==false);
			// RadialBlur
			pMapHdlr->GetPostEffect_RadialBlur()->SetDisabled(mpChBRadialBlur->IsChecked()==false);
			//Insanity
			pPostEffects->GetInsanity()->SetDisabled(mpChBInsanity->IsChecked()==false);
		}
	}


	//////////////////////////////
	// Input
	gpBase->mpInputHandler->SetInvertMouse(mpChBInvertMouse->IsChecked());
	gpBase->mpInputHandler->SetSmoothMouse(mpChBSmoothMouse->IsChecked());

	gpBase->mpInputHandler->SetMouseSensitivity(GetSensitivity());

#ifdef USE_GAMEPAD
	gpBase->mpInputHandler->SetGamepadLookSensitivity(GetGamepadLookSensitivity());
	gpBase->mpInputHandler->SetInvertGamepadLook(mpChBGamepadInvertLook->IsChecked());
#endif

	/////////////////////////////
    // Sound
	pCfgHdr->mbHRTFActive = mpChBHRTF->IsChecked();

	/* gpBase->mpEngine->GetSound()->GetLowLevel()->SetVolume(GetVolume());
	cWidgetItem* pItem = mpCBSndDevice->GetItem(mpCBSndDevice->GetSelectedItem());
	if(pItem)
	{
		iSoundDeviceIdentifier* pSndDev = (iSoundDeviceIdentifier*)pItem->GetUserData();
		gpBase->mpConfigHandler->mlSoundDevID = pSndDev->GetID();
	}
	*/

	//////////////////////////////////////////////////////////////
	//--- REMOVED - Language is now loaded only at startup
	//if(gpBase->LoadLanguage(cString::To8Char(mvLangFiles[mpCBLanguage->GetSelectedItem()])))
	//	gpBase->mpMainMenu->RecreateGui();
}


//-----------------------------------------------------------------------

void cLuxMainMenu_Options::SetGammaLabelString(float afX)
{
	SetSliderLabelString(mpLGamma, afX, mfGammaMin, mfGammaMax);
}

//-----------------------------------------------------------------------

void cLuxMainMenu_Options::SetSensitivityLabelString(float afX)
{
	SetSliderLabelString(mpLMouseSensitivity, afX, mfMouseSensitivityMin, mfMouseSensitivityMax);
}

//-----------------------------------------------------------------------

#ifdef USE_GAMEPAD
void cLuxMainMenu_Options::SetGamepadLookSensitivityLabelString(float afX)
{
	SetSliderLabelString(mpLGamepadLookSensitivity, afX, mfGamepadLookSensitivityMin, mfGamepadLookSensitivityMax);
}
#endif

//-----------------------------------------------------------------------

void cLuxMainMenu_Options::SetVolumeLabelString(float afX)
{
	SetSliderLabelString(mpLVolume, afX*10.0f, mfVolumeMin*10.0f, mfVolumeMax*10.0f);
}

//-----------------------------------------------------------------------

bool cLuxMainMenu_Options::ChangeLanguage(iWidget* apWidget, const cGuiMessageData& aData)
{
	///////////////////////////////////////////////////////
	// --- REMOVED - Language is now loaded only at startup

	//mInitialValues.AddVarBool("SettingLanguage", true);

	//DumpCurrentValues(mCurrentValues);

	//gpBase->LoadLanguage(cString::To8Char(mvLangFiles[mpCBLanguage->GetSelectedItem()]));
	//gpBase->mpMainMenu->RecreateGui();

	return true;
}
kGuiCallbackDeclaredFuncEnd(cLuxMainMenu_Options, ChangeLanguage);

//-----------------------------------------------------------------------

void cLuxMainMenu_Options::PopulateLanguageList()
{
	mpCBLanguage->ClearItems();
	mvLangFiles.clear();

	tWStringList lstLangs;
	tWString sPath = cString::To16Char(gpBase->msGameLanguageFolder);
	cPlatform::FindFilesInDir(lstLangs, sPath, _W("*.lang"));

	tWStringListIt it = lstLangs.begin();
	for(;it!=lstLangs.end();++it)
	{
		tWString sLang = *it;

		tWString sLangEntry = cString::ToLowerCaseW(cString::SubW(sLang, 0,
																	cString::GetLastStringPosW(sLang, _W("."))));

		mpCBLanguage->AddItem(kTranslate("Languages", cString::To8Char(sLangEntry)));
		mvLangFiles.push_back(sLang);
	}
}

//-----------------------------------------------------------------------

void cLuxMainMenu_Options::PopulateSoundDevices()
{
	iLowLevelSound* pLowLevelSound = gpBase->mpEngine->GetSound()->GetLowLevel();
	iSoundDeviceIdentifier* pCurSndDev = pLowLevelSound->GetCurrentSoundDevice();

	mpCBSndDevice->ClearItems();
	mvSoundDevices.clear();

	mvSoundDevices = pLowLevelSound->GetFilteredSoundDevices();
	if(mvSoundDevices.empty())
	{
		cWidgetItem* pItem = mpCBSndDevice->AddItem(pCurSndDev->GetName());
		pItem->SetUserData(pCurSndDev);
	}
	else
	{
		bool bCurrentDevFound = false;
		for(int i=0;i<(int)mvSoundDevices.size();++i)
		{
			iSoundDeviceIdentifier* pSndDev = mvSoundDevices[i];
			if(bCurrentDevFound==false && pSndDev==pCurSndDev)
				bCurrentDevFound = true;

			cWidgetItem* pItem = mpCBSndDevice->AddItem(pSndDev->GetName());
			pItem->SetUserData(pSndDev);
		}
		if(bCurrentDevFound==false)
		{
			cWidgetItem* pItem = mpCBSndDevice->AddItem("(Unsupported) " + pCurSndDev->GetName());
			pItem->SetUserData(pCurSndDev);
		}
	}
}

//-----------------------------------------------------------------------

void cLuxMainMenu_Options::SetUpSlider(cWidgetSlider* apSlider, float afMinValue, float afMaxValue, float afStepValue, tGuiCallbackFunc apCallback, cWidgetLabel** apValueDisplay)
{
	int lMaxValue = cMath::RoundToInt((afMaxValue-afMinValue)/afStepValue);
	apSlider->SetMaxValue(lMaxValue);
	apSlider->SetBarValueSize(cMath::RoundToInt(0.25f*(float)lMaxValue));

	if(apCallback)
	{
		apSlider->AddCallback(eGuiMessage_SliderMove, this, apCallback);
	}

	if(apValueDisplay)
	{
		cWidgetLabel* pLabel = mpGuiSet->CreateWidgetLabel(cVector3f(apSlider->GetSize().x*0.5f,2,1), -1, _W(""), apSlider);
		pLabel->SetTextAlign(eFontAlign_Center);

		*apValueDisplay = pLabel;
	}
}

//-----------------------------------------------------------------------

void cLuxMainMenu_Options::SetSliderValue(cWidgetSlider* apSlider, float afValue, bool abGenCallback, float afMinValue, float afMaxValue)
{
	afValue = cMath::Clamp(afValue, afMinValue, afMaxValue);

	float fMaxSliderValue = (float) apSlider->GetMaxValue();
	float fRange = afMaxValue-afMinValue;

	int lValue = cMath::RoundToInt((afValue-afMinValue)*fMaxSliderValue/fRange);

	apSlider->SetValue(lValue, abGenCallback);
}

//-----------------------------------------------------------------------

float cLuxMainMenu_Options::GetSliderValue(cWidgetSlider* apSlider, float afMinValue, float afMaxValue)
{
	float fSliderRelValue = ((float)apSlider->GetValue())/(float)apSlider->GetMaxValue();
	float fRange = afMaxValue-afMinValue;

	return afMinValue + fRange*fSliderRelValue;
}

//-----------------------------------------------------------------------

void cLuxMainMenu_Options::SetSliderLabelString(cWidgetLabel* apLabel, float afValue,
												float afMinValue, float afMaxValue,
												const tWString& asMin, const tWString& asMax)
{
	tWString sText;

	if(afValue<=afMinValue)
	{
		if(asMin.empty())
			afValue = afMinValue;
		else
			sText = asMin;
	}
	else if(afValue>=afMaxValue)
	{
		if(asMax.empty())
			afValue = afMaxValue;
		else
			sText = asMax;
	}

	if(sText.empty())
		sText = cString::ToStringW(afValue, 3, true);

	apLabel->SetText(sText);
}

//-----------------------------------------------------------------------

void cLuxMainMenu_Options::DumpInitialValues(cResourceVarsObject &aObj)
{
	////////////////////////////////
	// Game options
	{
		// Show hints
		aObj.AddVarBool("ShowHints", gpBase->mpHintHandler->IsActive());
		aObj.AddVarBool("ShowDeathHints", gpBase->mpPlayer->GetHelperDeath()->ShowHint());
		aObj.AddVarBool("ShowSubtitles", gpBase->mpMessageHandler->ShowSubtitles());
		aObj.AddVarBool("ShowEffectSubtitles", gpBase->mpMessageHandler->ShowEffectSubtitles());

		aObj.AddVarBool("ShowCrosshair", gpBase->mpPlayer->GetShowCrosshair());
		aObj.AddVarInt("FocusIconStyle", gpBase->mpPlayer->GetFocusIconStyle());
		aObj.AddVarBool("ShowCommentary", gpBase->mpMapHandler->GetShowCommentary());

		// Language
		aObj.AddVarString("Language", gpBase->mpConfigHandler->msLangFile);
	}
	////////////////////////////////
	// Graphics options
	{
		/////////////////////////
		// Resolution
		const cVector2l& vResolution = gpBase->mpConfigHandler->mvScreenSize;
		cVector2f vResolutionf = cVector2f((float)vResolution.x, (float)vResolution.y);
		aObj.AddVarVector2f("Resolution", vResolutionf);
        aObj.AddVarInt("Display", gpBase->mpConfigHandler->mlDisplay);

		/////////////////////////
		// Fullscreen & vsync
		aObj.AddVarBool("FullScreen", gpBase->mpConfigHandler->mbFullscreen);
		aObj.AddVarBool("VSync", gpBase->mpConfigHandler->mbVSync);
		aObj.AddVarBool("AdaptiveVsync", gpBase->mpConfigHandler->mbAdaptiveVSync);

		/////////////////////////
		// Texture quality and filtering
		aObj.AddVarInt("TextureQuality", gpBase->mpConfigHandler->mlTextureQuality);
		aObj.AddVarInt("TextureFilter", gpBase->mpConfigHandler->mlTextureFilter);
		aObj.AddVarFloat("TextureAnisotropy", gpBase->mpConfigHandler->mfTextureAnisotropy);
		aObj.AddVarString("SuperSamplingProvider", cLuxConfigHandler::SuperSamplingProviderToString(gpBase->mpConfigHandler->mSuperSampling.provider));
		aObj.AddVarString("SuperSamplingQuality", cLuxConfigHandler::SuperSamplingQualityToString(gpBase->mpConfigHandler->mSuperSampling.quality));
		aObj.AddVarString("RendererBackend", cLuxConfigHandler::RendererBackendToString(gpBase->mpConfigHandler->mRendererBackend));
		aObj.AddVarFloat("RenderScale", gpBase->mpConfigHandler->GetRenderScale());

		/////////////////////////
		// Smoothing
		aObj.AddVarBool("EdgeSmooth", gpBase->mpConfigHandler->mbEdgeSmooth);

		/////////////////////////
		// Shadows & Parallax
		aObj.AddVarBool("ShadowsActive", gpBase->mpConfigHandler->mbShadowsActive);
		aObj.AddVarInt("ShadowQuality", gpBase->mpConfigHandler->mlShadowQuality);
		aObj.AddVarInt("ShadowResolution", gpBase->mpConfigHandler->mlShadowRes);
		aObj.AddVarBool("ParallaxEnabled", gpBase->mpConfigHandler->mbParallaxEnabled);
		aObj.AddVarInt("ParallaxQuality", gpBase->mpConfigHandler->mlParallaxQuality);

		/////////////////////////
		// Water
		aObj.AddVarBool("WorldReflection", gpBase->mpConfigHandler->mbWorldReflection);
		aObj.AddVarBool("Refraction", gpBase->mpConfigHandler->mbRefraction);

		/////////////////
		// PostEffects
		cLuxMapHandler* pMapHdlr = gpBase->mpMapHandler;
		cLuxPostEffectHandler *pPostEffects = gpBase->mpPostEffectHandler;
		aObj.AddVarBool("BloomActive", pMapHdlr->GetPostEffect_Bloom()->IsDisabled()==false);
		aObj.AddVarBool("ImageTrailActive", pMapHdlr->GetPostEffect_ImageTrail()->IsDisabled()==false);
		aObj.AddVarBool("SepiaActive", pMapHdlr->GetPostEffect_Sepia()->IsDisabled()==false);
		aObj.AddVarBool("RadialBlurActive", pMapHdlr->GetPostEffect_RadialBlur()->IsDisabled()==false);
		aObj.AddVarBool("InsanityActive", pPostEffects->GetInsanity()->IsDisabled()==false);


		///////////////////
		// Gamma
		aObj.AddVarFloat("Gamma", gpBase->mpConfigHandler->GetGamma());
	}

	////////////////////////////////
	// Input
	aObj.AddVarBool("InvertMouse", gpBase->mpInputHandler->GetInvertMouse());
	aObj.AddVarBool("SmoothMouse", gpBase->mpInputHandler->GetSmoothMouse());
	aObj.AddVarFloat("MouseSensitivity", gpBase->mpInputHandler->GetMouseSensitivity());

#ifdef USE_GAMEPAD
	aObj.AddVarBool("GamepadInvertLook", gpBase->mpInputHandler->GetInvertGamepadLook());
	aObj.AddVarFloat("GamepadLookSensitivity", gpBase->mpInputHandler->GetGamepadLookSensitivity());
#endif

	////////////////////////////////
	// Sound
	aObj.AddVarFloat("SoundVolume", gpBase->mpEngine->GetSound()->GetLowLevel()->GetVolume());
	aObj.AddVarInt("SoundDeviceID", gpBase->mpConfigHandler->mlSoundDevID);
	aObj.AddVarBool("HRTFActive", gpBase->mpConfigHandler->mbHRTFActive);
}

//-----------------------------------------------------------------------

void cLuxMainMenu_Options::DumpCurrentValues(cResourceVarsObject &aObj)
{
	////////////////////////////////
	// Game options
	{
		aObj.AddVarBool("ShowHints",		mpChBShowHints->IsChecked());
		aObj.AddVarBool("ShowDeathHints",	mpChBShowDeathHints->IsChecked());
		aObj.AddVarBool("ShowSubtitles",	mpChBShowSubtitles->IsChecked());
		aObj.AddVarBool("ShowEffectSubtitles", mpChBShowEffectSubtitles->IsChecked());

		aObj.AddVarBool("ShowCrosshair",	mpChBShowCrosshair->IsChecked());
		aObj.AddVarInt("FocusIconStyle",	mpCBFocusIconStyle->GetSelectedItem());

		aObj.AddVarString("Language",		cString::To8Char(mvLangFiles[mpCBLanguage->GetSelectedItem()]));
	}

	////////////////////////////////
	// Graphics options
	{
		/////////////////////////
		// Resolution
		const cVideoMode& vResolution = mvScreenSizes[mpCBResolution->GetSelectedItem()];
		cVector2f vResolutionf = cVector2f((float)vResolution.mvScreenSize.x, (float)vResolution.mvScreenSize.y);
		aObj.AddVarVector2f("Resolution", vResolutionf);
        aObj.AddVarInt("Display", vResolution.mlDisplay);

		/////////////////////////
		// Fullscreen & vsync
		aObj.AddVarBool("FullScreen",	mpChBFullScreen->IsChecked());
		aObj.AddVarBool("VSync",		mpChBVSync->IsChecked());

		/////////////////////////
		// Texture quality and filtering
		aObj.AddVarInt("TextureQuality", (mpCBTextureSizeLevel->GetItemNum()-1) - mpCBTextureSizeLevel->GetSelectedItem());
		aObj.AddVarInt("TextureFilter", mpCBTextureFilter->GetSelectedItem());
		aObj.AddVarFloat("TextureAnisotropy", GetAnisotropyFromIndex(mpCBAnisotropy->GetSelectedItem()));
		aObj.AddVarString("SuperSamplingProvider", cLuxConfigHandler::SuperSamplingProviderToString(mSuperSamplingRequestedProvider));
		aObj.AddVarString("SuperSamplingQuality", cLuxConfigHandler::SuperSamplingQualityToString(mSuperSamplingRequestedQuality));
		aObj.AddVarString("RendererBackend", cLuxConfigHandler::RendererBackendToString(GetSelectedRendererBackend(mpCBRendererBackend)));
		aObj.AddVarFloat("RenderScale", mfRenderScaleRequested);

		/////////////////////////
		// Smoothing
		aObj.AddVarBool("EdgeSmooth", mpChEdgeSmooth->IsChecked());

		/////////////////////////
		// Shadows & Parallax
		aObj.AddVarBool("ShadowsActive", mpChBShadows->IsChecked());
		aObj.AddVarInt("ShadowQuality",  mpCBShadowQuality->GetSelectedItem());
		aObj.AddVarInt("ShadowResolution", mpCBShadowRes->GetSelectedItem());
		aObj.AddVarBool("ParallaxEnabled", mpCBParallaxQuality->GetSelectedItem()==1);
		//aObj.AddVarInt("ParallaxQuality",  mpCBParallaxQuality->GetSelectedItem()-1);

		/////////////////////////
		// Water
		aObj.AddVarBool("WorldReflection", mpChBWorldReflection->IsChecked());
		aObj.AddVarBool("Refraction", mpChBRefraction->IsChecked());

		/////////////////
		// PostEffects
		aObj.AddVarBool("BloomActive", mpChBBloom->IsChecked());
		aObj.AddVarBool("ImageTrailActive", mpChBImageTrail->IsChecked());
		aObj.AddVarBool("SepiaActive", mpChBSepia->IsChecked());
		aObj.AddVarBool("RadialBlurActive", mpChBRadialBlur->IsChecked());
		aObj.AddVarBool("InsanityActive", mpChBInsanity->IsChecked());

		///////////////////
		// Gamma
		aObj.AddVarFloat("Gamma", GetGamma());
	}

	////////////////////////////////
	// Input
	aObj.AddVarBool("InvertMouse", mpChBInvertMouse->IsChecked());
	aObj.AddVarBool("SmoothMouse", mpChBSmoothMouse->IsChecked());
	aObj.AddVarFloat("MouseSensitivity", GetSensitivity());

#ifdef USE_GAMEPAD
	aObj.AddVarBool("GamepadInvertLook", mpChBGamepadInvertLook->IsChecked());
	aObj.AddVarFloat("GamepadLookSensitivity", GetGamepadLookSensitivity());
#endif

	////////////////////////////////
	// Sound
	aObj.AddVarFloat("SoundVolume", GetVolume());
	cWidgetItem* pItem = mpCBSndDevice->GetItem(mpCBSndDevice->GetSelectedItem());
	int lDevID=-1;
	if(pItem)
	{
		iSoundDeviceIdentifier* pSndDev = (iSoundDeviceIdentifier*)pItem->GetUserData();
		lDevID = pSndDev->GetID();
	}

	aObj.AddVarInt("SoundDeviceID", lDevID);
	aObj.AddVarBool("HRTFActive", mpChBHRTF->IsChecked());
}


//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// CALLBACKS
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

bool cLuxMainMenu_Options::Window_OnUpdate(iWidget* apWidget, const cGuiMessageData& aData)
{
	RefreshTemporalUpscalerStatusLabel();
	RefreshRenderScaleControl();

	///////////////////////////////////////////////////
	// If there is a popup active, dont update tips
	if(mpGuiSet->PopUpIsActive())
		return true;

	////////////////////////////////////////////////////
	// Update Tip label fade
	cColor labelCol = mpLTip->GetDefaultFontColor();
	if(mbTipFadeRestart)
	{
		if(mbTipTextReset)
		{
			mbTipTextReset = false;
			mpLTip->SetScrollOffset(0);
		}
		labelCol = labelCol + cColor(0, aData.mfVal*3);
		if(labelCol.a>=1.0f)
		{
			labelCol.a = 1.0f;
			mbTipFadeRestart = false;
		}
		mpLTip->SetDefaultFontColor(labelCol);
	}
	else
	{
		if(labelCol.a>0.0f)
		{
			mpLTip->SetDefaultFontColor(labelCol-cColor(0, aData.mfVal*0.8f));
		}
		else if(mbTipTextReset==false)
		{
			mbTipTextReset = true;
		}
	}

	///////////////////////////////////////////////////
	// Update Tip label text

	if(mbTipWidgetUpdated)
	{
		mbTipWidgetUpdated = false;

		if(mpCurrentTipWidget && mpCurrentTipWidget->GetUserData())
		{
			cLuxOption_ExtData* pData = (cLuxOption_ExtData*)mpCurrentTipWidget->GetUserData();
			tWString sRestart = pData->mbNeedsRestart? _W(" (") + kTranslate("OptionsMenu", "ReqRestart") + _W(")"): _W("");

			mpLTip->SetText(pData->msMessage + sRestart);
		}
	}

	return true;
}
kGuiCallbackDeclaredFuncEnd(cLuxMainMenu_Options, Window_OnUpdate);

//-----------------------------------------------------------------------

bool cLuxMainMenu_Options::Option_OnMouseOver(iWidget* apWidget, const cGuiMessageData& aData)
{
	if(apWidget->GetUserData()==NULL)
		return true;

    SetCurrentTipWidget(apWidget);

	/////////////
	// Make sure the focused widget is visible
	if(aData.mMessage == eGuiMessage_GetUINavFocus)
	{
		iWidget* pParent = apWidget->GetParent();

		if(pParent)
		{
			iWidget* pParent2 = pParent->GetParent();
			cWidgetFrame* pFrame = dynamic_cast<cWidgetFrame*>(pParent2);

			if(pFrame == NULL
			&& pParent2 != NULL)
			{
				pFrame = dynamic_cast<cWidgetFrame*>(pParent2->GetParent());
			}

			if(pFrame)
			{
				cVector3f vFramePos = pFrame->GetGlobalPosition();
				cVector3f vWidgedPos = apWidget->GetGlobalPosition();
				cVector3f vScrollAmount = pFrame->GetScrollAmount();
				cVector3f vScrollPos = vWidgedPos + vScrollAmount - vFramePos;
				vScrollPos.y -= 150;
				pFrame->ScrollToPosition(cVector2f(vScrollPos.x, vScrollPos.y));
			}
		}
	}
	return true;
}
kGuiCallbackDeclaredFuncEnd(cLuxMainMenu_Options, Option_OnMouseOver);

//-----------------------------------------------------------------------

bool cLuxMainMenu_Options::Option_OnChangeValue(iWidget* apWidget, const cGuiMessageData& aData)
{
	if(mbSettingInitialValues)
		return true;

	if(apWidget->GetUserData())
	{
		cLuxOption_ExtData* pData = (cLuxOption_ExtData*)apWidget->GetUserData();
		if(pData->mbNeedsRestart)
			gpBase->mpConfigHandler->SetGameNeedsRestart();
	}
	return true;
}
kGuiCallbackDeclaredFuncEnd(cLuxMainMenu_Options, Option_OnChangeValue);

//-----------------------------------------------------------------------

bool cLuxMainMenu_Options::TemporalUpscaler_OnProviderChange(iWidget* apWidget, const cGuiMessageData& aData)
{
	if(mbSettingInitialValues || mbRebuildingTemporalUpscaler)
		return true;

	if(mpCBTemporalUpscaler == NULL)
		return true;
	int lSelectedItem = mpCBTemporalUpscaler->GetSelectedItem();
	if(lSelectedItem < 0 || lSelectedItem >= mpCBTemporalUpscaler->GetItemNum())
		return true;
	cWidgetItem* pItem = mpCBTemporalUpscaler->GetItem(lSelectedItem);
	if(pItem == NULL)
		return true;

	int lProvider = pItem->GetUserValue();
	if(lProvider == -1)
	{
		if(mSuperSamplingUnavailableProvider != TemporalUpscalerProvider::Off)
			mSuperSamplingRequestedProvider = mSuperSamplingUnavailableProvider;
	}
	else if(lProvider >= (int)TemporalUpscalerProvider::Off && lProvider <= (int)TemporalUpscalerProvider::XeSS)
	{
		mSuperSamplingRequestedProvider = (TemporalUpscalerProvider)lProvider;
	}
	else
	{
		return true;
	}

	RebuildTemporalUpscalerQualityList();
	RefreshTemporalUpscalerStatusLabel();
	RefreshRenderScaleControl();
	return true;
}
kGuiCallbackDeclaredFuncEnd(cLuxMainMenu_Options, TemporalUpscaler_OnProviderChange);

//-----------------------------------------------------------------------

bool cLuxMainMenu_Options::TemporalUpscaler_OnQualityChange(iWidget* apWidget, const cGuiMessageData& aData)
{
	if(mbSettingInitialValues || mbRebuildingTemporalUpscaler)
		return true;

	if(mpCBTemporalUpscalerQuality == NULL || mpCBTemporalUpscalerQuality->IsEnabled() == false)
		return true;
	int lSelectedItem = mpCBTemporalUpscalerQuality->GetSelectedItem();
	if(lSelectedItem < 0 || lSelectedItem >= mpCBTemporalUpscalerQuality->GetItemNum())
		return true;
	cWidgetItem* pItem = mpCBTemporalUpscalerQuality->GetItem(lSelectedItem);
	if(pItem == NULL)
		return true;
	int lQuality = pItem->GetUserValue();
	if(lQuality < (int)TemporalUpscalerQuality::NativeAA || lQuality > (int)TemporalUpscalerQuality::UltraPerformance)
		return true;
	mSuperSamplingRequestedQuality = (TemporalUpscalerQuality)lQuality;
	RefreshTemporalUpscalerStatusLabel();
	RefreshRenderScaleControl();
	return true;
}
kGuiCallbackDeclaredFuncEnd(cLuxMainMenu_Options, TemporalUpscaler_OnQualityChange);

//-----------------------------------------------------------------------

bool cLuxMainMenu_Options::RenderScale_OnChange(iWidget* apWidget, const cGuiMessageData& aData)
{
	if(mbSettingInitialValues || mbRebuildingTemporalUpscaler)
		return true;

	if(mpCBRenderScale == NULL || mpCBRenderScale->IsEnabled() == false)
		return true;
	int lSelectedItem = mpCBRenderScale->GetSelectedItem();
	if(lSelectedItem < 0 || lSelectedItem >= mpCBRenderScale->GetItemNum())
		return true;
	cWidgetItem* pItem = mpCBRenderScale->GetItem(lSelectedItem);
	if(pItem == NULL)
		return true;
	int lRenderScale = pItem->GetUserValue();
	if(lRenderScale < 0 || lRenderScale >= cLuxConfigHandler::GetRenderScalePresetNum())
		return true;
	mfRenderScaleRequested = cLuxConfigHandler::GetRenderScalePreset(lRenderScale);
	return true;
}
kGuiCallbackDeclaredFuncEnd(cLuxMainMenu_Options, RenderScale_OnChange);

//-----------------------------------------------------------------------

bool cLuxMainMenu_Options::GammaSlider_OnMove(iWidget* apWidget, const cGuiMessageData& aData)
{
	float fGamma = GetGamma();
	SetGammaLabelString(fGamma);

	gpBase->mpConfigHandler->SetGamma(fGamma);

	return true;
}
kGuiCallbackDeclaredFuncEnd(cLuxMainMenu_Options, GammaSlider_OnMove);

//-----------------------------------------------------------------------

bool cLuxMainMenu_Options::MouseSensitivitySlider_OnMove(iWidget* apWidget, const cGuiMessageData& aData)
{
	float fSensitivity = GetSensitivity();
	SetSensitivityLabelString(fSensitivity);

	gpBase->mpInputHandler->SetMouseSensitivity(fSensitivity);

	return true;
}
kGuiCallbackDeclaredFuncEnd(cLuxMainMenu_Options, MouseSensitivitySlider_OnMove);

//-----------------------------------------------------------------------

#ifdef USE_GAMEPAD
bool cLuxMainMenu_Options::GamepadLookSensitivitySlider_OnMove(iWidget* apWidget, const cGuiMessageData& aData)
{
	float fSensitivity = GetGamepadLookSensitivity();
	SetGamepadLookSensitivityLabelString(fSensitivity);

	gpBase->mpInputHandler->SetGamepadLookSensitivity(fSensitivity);

	return true;
}
kGuiCallbackDeclaredFuncEnd(cLuxMainMenu_Options, GamepadLookSensitivitySlider_OnMove);
#endif

//-----------------------------------------------------------------------

bool cLuxMainMenu_Options::SoundSlider_OnMove(iWidget* apWidget, const cGuiMessageData& aData)
{
	float fSndVol = GetVolume();
	SetVolumeLabelString(fSndVol);

	cSound *pSound = gpBase->mpEngine->GetSound();

	pSound->GetLowLevel()->SetVolume(fSndVol);
	pSound->GetSoundHandler()->PlayGui("ui_use_sanity.ogg", false, 1);

	return true;
}
kGuiCallbackDeclaredFuncEnd(cLuxMainMenu_Options, SoundSlider_OnMove);

//-----------------------------------------------------------------------

bool cLuxMainMenu_Options::PressToggleShowGfxOptions(iWidget* apWidget, const cGuiMessageData& aData)
{
	bool bBasicVisible = mpDBasicGfxOptions->IsVisible();
	tWString sText[] = { kTranslate("OptionsMenu", "BasicOptions"), kTranslate("OptionsMenu", "AdvancedOptions") };

	mpDBasicGfxOptions->SetVisible(bBasicVisible==false);
	mpDBasicGfxOptions->SetEnabled(bBasicVisible==false);

	mpDAdvancedGfxOptions->SetVisible(bBasicVisible);
	mpDAdvancedGfxOptions->SetEnabled(bBasicVisible);

	cWidgetTab* pTab = mpTabGraphics;

	if(bBasicVisible)
	{
		mpBToggleShowGfxOptions->SetFocusNavigation(eUIArrow_Up, mpChBRefraction);

		pTab->GetTabLabel()->SetUserData(mpBToggleShowGfxOptions);
	}
	else
	{
		mpBToggleShowGfxOptions->SetFocusNavigation(eUIArrow_Up, mpSGamma);

		pTab->GetTabLabel()->SetUserData(mpCBResolution);
	}

	apWidget->SetText(sText[bBasicVisible==false]);

	return true;
}
kGuiCallbackDeclaredFuncEnd(cLuxMainMenu_Options, PressToggleShowGfxOptions);

//-----------------------------------------------------------------------

bool cLuxMainMenu_Options::PressKeyConfig(iWidget* apWidget, const cGuiMessageData& aData)
{
	mbKeyConfigOpen = true;
	gpBase->mpMainMenu->SetWindowActive(eLuxMainMenuWindow_KeyConfig);

	return true;
}
kGuiCallbackDeclaredFuncEnd(cLuxMainMenu_Options, PressKeyConfig);

//-----------------------------------------------------------------------

bool cLuxMainMenu_Options::PressOK(iWidget* apWidget, const cGuiMessageData& aData)
{
	ApplyChanges();
	gpBase->SaveConfig();
	//If some major stuff (that needs restart) have been changed. Then say so!
	if(gpBase->mpConfigHandler->ShowRestartWarning(mpGuiSet, this, kGuiCallback(MessageBoxCallback))==false)
		gpBase->mpMainMenu->SetWindowActive(eLuxMainMenuWindow_LastEnum);

	return true;
}
kGuiCallbackDeclaredFuncEnd(cLuxMainMenu_Options, PressOK);

//-----------------------------------------------------------------------

bool cLuxMainMenu_Options::PressCancel(iWidget* apWidget, const cGuiMessageData& aData)
{
	//Cancel any changes made.

	/////////////////////////////////////////////////////////////
	// --- REMOVED - Language is now loaded only at startup
	//if(gpBase->msCurrentLanguage!=mInitialValues.GetVarString("Language"))
	//{
	//	gpBase->LoadLanguage(mInitialValues.GetVarString("Language"));
	//	gpBase->mpMainMenu->RecreateGui();
	//}
	gpBase->mpConfigHandler->SetGamma(mInitialValues.GetVarFloat("Gamma"));
	gpBase->mpInputHandler->SetMouseSensitivity(mInitialValues.GetVarFloat("MouseSensitivity"));
#ifdef USE_GAMEPAD
	gpBase->mpInputHandler->SetGamepadLookSensitivity(mInitialValues.GetVarFloat("GamepadLookSensitivity"));
#endif
	gpBase->mpEngine->GetSound()->GetLowLevel()->SetVolume(mInitialValues.GetVarFloat("SoundVolume"));

	MessageBoxCallback(apWidget, aData);

	return true;
}
kGuiCallbackDeclaredFuncEnd(cLuxMainMenu_Options, PressCancel);

//-----------------------------------------------------------------------

bool cLuxMainMenu_Options::UIPressCancel(iWidget* apWidget, const cGuiMessageData& aData)
{
	if(aData.mlVal == eUIButton_Secondary)
	{
		return PressCancel(apWidget, aData);
	}

	return false;
}
kGuiCallbackDeclaredFuncEnd(cLuxMainMenu_Options, UIPressCancel);


//-----------------------------------------------------------------------

void cLuxMainMenu_Options::OnSetActive(bool abX)
{
	// If the key config menu is open, dont touch anything. Else, set up initial values.
	if(abX)
	{
		SetTabNavigation(mpTabGame->GetParentTabFrame()->GetTabOnTop(), true);

		if(mbKeyConfigOpen==false)
		{
			if(mInitialValues.GetVarBool("SettingLanguage", false))
			{
				SetInputValues(mCurrentValues);
				mInitialValues.AddVarBool("SettingLanguage", false);
			}
			else
			{
				if(mInitialValues.GetVarBool("InitialValuesSet")==false)
					DumpInitialValues(mInitialValues);

				SetInputValues(mInitialValues);
				mInitialValues.AddVarBool("InitialValuesSet", true);
			}
		}
		else
			mbKeyConfigOpen = false;
	}

#ifdef USE_GAMEPAD
	if(mpShoulderHint[0])
	{
		mpShoulderHint[0]->SetVisible(gpBase->mpInputHandler->IsGamepadPresent());
		mpShoulderHint[1]->SetVisible(gpBase->mpInputHandler->IsGamepadPresent());
	}
#endif
}

//-----------------------------------------------------------------------

bool cLuxMainMenu_Options::MessageBoxCallback(iWidget* apWidget, const cGuiMessageData& aData)
{
	gpBase->mpMainMenu->SetWindowActive(eLuxMainMenuWindow_LastEnum);

	return true;
}
kGuiCallbackDeclaredFuncEnd(cLuxMainMenu_Options, MessageBoxCallback);

//-----------------------------------------------------------------------

bool cLuxMainMenu_Options::TabFrame_OnPageChange(iWidget* apWidget, const cGuiMessageData& aData)
{
	cWidgetTabFrame* pTabFrame = static_cast<cWidgetTabFrame*>(apWidget);
	cWidgetTab* pTab = pTabFrame->GetTab(aData.mlVal);

	SetTabNavigation(pTab, true);

	return true;
}
kGuiCallbackDeclaredFuncEnd(cLuxMainMenu_Options, TabFrame_OnPageChange);

//-----------------------------------------------------------------------

void cLuxMainMenu_Options::SetTabNavigation(cWidgetTab* apTab, bool abSetFocus)
{
	iWidget* pFirstWidget = static_cast<iWidget*>(apTab->GetTabLabel()->GetUserData());
	iWidget* pLastWidget = static_cast<iWidget*>(apTab->GetUserData());

	mpGuiSet->SetDefaultFocusNavWidget(pFirstWidget);
	if(abSetFocus)
		mpGuiSet->SetFocusedWidget(pFirstWidget);

	mpBOK->SetFocusNavigation(eUIArrow_Up, pLastWidget);
	mpBCancel->SetFocusNavigation(eUIArrow_Up, pLastWidget);
}

//-----------------------------------------------------------------------
