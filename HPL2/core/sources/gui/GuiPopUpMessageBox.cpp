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

#include "gui/GuiPopUpMessageBox.h"

#include "system/LowLevelSystem.h"
#include "system/String.h"

#include "math/Math.h"

#include "graphics/FontData.h"

#include "gui/Gui.h"
#include "gui/GuiSkin.h"
#include "gui/GuiSet.h"

#include "gui/WidgetButton.h"
#include "gui/WidgetLabel.h"
#include "gui/WidgetWindow.h"

namespace hpl {

	//////////////////////////////////////////////////////////////////////////
	// CONSTRUCTORS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	// Message texts can contain [br] tags for line breaks
	static tWString ReplaceLineBreakTags(const tWString& asText)
	{
		const tWString sTag = _W("[br]");
		tWString sRet;
		size_t lStart = 0;
		while(true)
		{
			size_t lPos = asText.find(sTag, lStart);
			if(lPos == tWString::npos)
			{
				sRet += asText.substr(lStart);
				break;
			}
			sRet += asText.substr(lStart, lPos - lStart);
			sRet += _W("\n");
			lStart = lPos + sTag.size();
		}
		return sRet;
	}

	//-----------------------------------------------------------------------

	cGuiPopUpMessageBox::cGuiPopUpMessageBox(cGuiSet *apSet,
											const tWString& asLabel, const tWString& asText,
											const tWString& asButton1, const tWString& asButton2,
											void *apCallbackObject, tGuiCallbackFunc apCallback) 
						: iGuiPopUp(apSet, false, 0)
	{
		//////////////////////////
		// Set up variables
		mpCallback = apCallback;
		mpCallbackObject = apCallbackObject;

		cGuiSkinFont *pFont = mpSkin->GetFont(eGuiSkinFont_Default);
		
		tWString sText = ReplaceLineBreakTags(asText);

		cVector2f vVirtSize = mpSet->GetVirtualSize();
		float fMaxTextWidth = cMath::Max(cMath::Min(560.0f, vVirtSize.x - 80.0f), 160.0f);

		// Widest line; long lines are word wrapped at the max width
		float fTextWidth = 0;
		{
			tWStringVec vLines;
			tWString sSep = _W("\n");
			cString::GetStringVecW(sText, vLines, &sSep);
			for(size_t i=0; i<vLines.size(); ++i)
				fTextWidth = cMath::Max(fTextWidth, pFont->mpFont->GetLength(pFont->mvSize, vLines[i].c_str()));
		}
		if(fTextWidth > fMaxTextWidth) fTextWidth = fMaxTextWidth;

		float fRowHeight = pFont->mvSize.y + 2; // same row height as cWidgetLabel
		tWStringVec vRows;
		pFont->mpFont->GetWordWrapRows(fTextWidth + 1, fRowHeight, pFont->mvSize, sText, &vRows);
		int lRows = cMath::Max((int)vRows.size(), 1);
		float fExtraHeight = fRowHeight * (float)(lRows - 1);

		float fWindowMinLength = pFont->mpFont->GetLength(pFont->mvSize,asLabel.c_str());
		if(fTextWidth > fWindowMinLength) fWindowMinLength = fTextWidth;

		float fWindowWidth = fWindowMinLength+40 > 200 ? fWindowMinLength+40 : 200;

		float fWindowHeight = 90 + pFont->mvSize.y + fExtraHeight;
		
		cVector3f vPos = cVector3f(vVirtSize.x/2 - fWindowWidth/2,vVirtSize.y/2- fWindowHeight/2,100);

		//////////////////////////
		// Window
		mpWindow->SetText(asLabel);
		mpWindow->SetPosition(vPos);
		mpWindow->SetSize(cVector2f(fWindowWidth, fWindowHeight));
		
		//////////////////////////
		// Buttons
		if(asButton2 == _W(""))
		{
			vPos = cVector3f(fWindowWidth/2 - 40, 50 + pFont->mvSize.y + fExtraHeight,1);
			mvButtons[0] = mpSet->CreateWidgetButton(vPos,cVector2f(80,30),asButton1,mpWindow);
			mvButtons[0]->AddCallback(eGuiMessage_ButtonPressed,this, kGuiCallback(ButtonPress));
			mvButtons[0]->AddCallback(eGuiMessage_UIButtonPress,this, kGuiCallback(GamepadButtonPress));
			mvButtons[0]->SetGlobalUIInputListener(true);

			mvButtons[1] = NULL;
		}
		else
		{
			vPos = cVector3f(fWindowWidth/2 - (80*2+20)/2, 50 + pFont->mvSize.y + fExtraHeight,1);
			mvButtons[0] = mpSet->CreateWidgetButton(vPos,cVector2f(80,30),asButton1,mpWindow);
			mvButtons[0]->AddCallback(eGuiMessage_ButtonPressed,this, kGuiCallback(ButtonPress));
			mvButtons[0]->AddCallback(eGuiMessage_UIButtonPress,this, kGuiCallback(GamepadButtonPress));
			mvButtons[0]->SetGlobalUIInputListener(true);

			vPos.x += 80+20;
			mvButtons[1] = mpSet->CreateWidgetButton(vPos,cVector2f(80,30),asButton2,mpWindow);
			mvButtons[1]->AddCallback(eGuiMessage_ButtonPressed,this, kGuiCallback(ButtonPress));
			mvButtons[1]->AddCallback(eGuiMessage_UIButtonPress,this, kGuiCallback(GamepadButtonPress));
			mvButtons[1]->SetGlobalUIInputListener(true);
			
			mvButtons[0]->SetFocusNavigation(eUIArrow_Right, mvButtons[1]);
			mvButtons[1]->SetFocusNavigation(eUIArrow_Left, mvButtons[0]);

		}

		SetUpDefaultFocus(mvButtons[0]);
		
		//////////////////////////
		// Label
		vPos = cVector3f(20, 30,1);
		mpLabel = mpSet->CreateWidgetLabel(vPos,cVector2f(fTextWidth + 1, fRowHeight * (float)lRows),
											sText,mpWindow);
		mpLabel->SetWordWrap(true);
		mpLabel->SetAutoScroll(false);

		SetUpDefaultFocus(mvButtons[0]);
	}

	//-----------------------------------------------------------------------

	cGuiPopUpMessageBox::~cGuiPopUpMessageBox()
	{
		if(mvButtons[0]) mpSet->DestroyWidget(mvButtons[0]);
		if(mvButtons[1]) mpSet->DestroyWidget(mvButtons[1]);
		if(mpLabel) mpSet->DestroyWidget(mpLabel);
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PUBLIC METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------
	

	//-----------------------------------------------------------------------


	//////////////////////////////////////////////////////////////////////////
	// PROTECTED METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------
	
	bool cGuiPopUpMessageBox::ButtonPress(iWidget* apWidget,const cGuiMessageData& aData)
	{
		int lButton = apWidget == mvButtons[0] ? 0 : 1;

		RunCallback(mpCallbackObject, mpCallback, apWidget, cGuiMessageData(lButton), true);

		SelfDestruct();
		
		return true;
	}
	kGuiCallbackDeclaredFuncEnd(cGuiPopUpMessageBox,ButtonPress)

	
	//-----------------------------------------------------------------------

	bool cGuiPopUpMessageBox::GamepadButtonPress(iWidget* apWidget,const cGuiMessageData& aData)
	{
		if(!(aData.mlVal == eUIButton_Primary || aData.mlVal == eUIButton_Secondary)) return false;

		if(aData.mlVal == eUIButton_Secondary && mvButtons[1]) apWidget = mvButtons[1];

		return ButtonPress(apWidget, aData);
	}
	kGuiCallbackDeclaredFuncEnd(cGuiPopUpMessageBox,GamepadButtonPress)

	
	//-----------------------------------------------------------------------


}
