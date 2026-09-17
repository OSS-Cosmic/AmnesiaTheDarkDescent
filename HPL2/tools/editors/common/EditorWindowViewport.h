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

#ifndef HPLEDITOR_EDITOR_WINDOW_VIEWPORT_H
#define HPLEDITOR_EDITOR_WINDOW_VIEWPORT_H

#include "../common/StdAfx.h"

using namespace hpl;

#include "EditorWindow.h"
#include "EditorViewport.h"
#include "EditorRenderMode.h"

//--------------------------------------------------------------------

class iEditorBase;

class cEditorGrid;
class cDrawGridCallback;
class cEditorWindowViewport;
class cEditorWindowViewportCamera;

namespace hpl { struct WorldDrawCtx; }

namespace tinyxml2 { class XMLElement; }



enum eEditorWindowViewportPreset
{
	eEditorWindowViewportPreset_Front,
	eEditorWindowViewportPreset_Right,
	eEditorWindowViewportPreset_Top,
	eEditorWindowViewportPreset_Perspective,

	eEditorWindowViewportPreset_LastEnum,
};

//--------------------------------------------------------------------

////////////////////////////////////////////////
// VIEWPORT RENDERER CALLBACK
////////////////////////////////////////////////

//--------------------------------------------------------------------

// The draw path on the RI backend is the cViewport::OnPreWorldDraw() event —
// OnPreWorldDraw enqueues the whole editor overlay (grid, clip planes,
// surface picker, edit modes, entity visuals) into the global DebugDraw
// batcher, which HybridRenderer flushes into the pane's offscreen target
// right before the delivery blit. The legacy iRendererCallback
// post-solid/post-translucent hooks are gone; depth behavior rides on
// DebugDrawOptions::m_depthTest.
class cViewportCallback
{
public:
	cViewportCallback();

	void OnPreWorldDraw();

	iEditorBase* mpEditor;
	cEditorWindowViewport* mpViewport;

	bool mbDrawDebug;
	bool mbDrawGrid;
};

//--------------------------------------------------------------------

////////////////////////////////////////////////
// VIEWPORT WINDOW
////////////////////////////////////////////////

//--------------------------------------------------------------------

class cEditorWindowViewport : public iEditorWindow, public iEditorViewport
{
public:
	cEditorWindowViewport(iEditorBase* apEditor, bool abAddViewMenu=true);
	~cEditorWindowViewport();

	void SetFocused(bool abX);
	bool IsFocused();

	void SetDrawGrid(bool abX) { mbDrawGrid = abX; }
	bool GetDrawGrid() { return mbDrawGrid; }

	void SetDrawDebug(bool abX) { mbDrawDebug = abX; }
	bool GetDrawDebug() { return mbDrawDebug; }

	void SetDrawAxes(bool abX) { mbDrawAxes = abX; }
	bool GetDrawAxes() { return mbDrawAxes; }

	void SetPreset(eEditorWindowViewportPreset aPreset);

	void LookAtEntity(iEntityWrapper* apEntity);
	void FocusOnSelection(cEditorSelection* apSelection);

	void Load(tinyxml2::XMLElement* apElement);
	void Save(tinyxml2::XMLElement* apElement);

	void SetNormalSize(const cVector2f& avX) { mvNormalSize = avX; }
	void SetNormalPosition(const cVector3f& avX) { mvNormalPosition = avX; }
	void SetEnlargedSize(const cVector2f& avX) { mvEnlargedSize = avX; }
	void SetEnlargedPosition(const cVector3f& avX) { mvEnlargedPosition = avX; }

	void SetEngineViewportNormalPosition(const cVector2l& avX) { mvNormalFBPos = avX; }
	void SetEngineViewportNormalSize(const cVector2l& avX) { mvNormalFBSize = avX; }

	void SetEnlarged(bool abX);
	bool IsEnlarged() { return (mvSize==mvEnlargedSize); }

	// Re-derives the View menu's check and enable states. Public because the
	// lit backend is global: iEditorBase refreshes every viewport's menu after
	// a switch, not just the one that was clicked.
	void UpdateMenu();

	/////////////////////////////////
	// DEBUG
	cVector3f vDebugLineStart;
	cVector3f vDebugLineEnd;
	cVector3f vDebugGridPos;
	cVector3f vDebugSnappedGridPos;
protected:
	///////////////////////////
	// Own functions
	
	// Menu callbacks
	bool MenuView_ShowGrid(iWidget *apWidget, const cGuiMessageData& aData);
	kGuiCallbackDeclarationEnd(MenuView_ShowGrid);
	bool MenuView_ShowAxes(iWidget *apWidget, const cGuiMessageData& aData);
	kGuiCallbackDeclarationEnd(MenuView_ShowAxes);
	bool MenuView_Presets(iWidget* apWidget, const cGuiMessageData& aData);
	kGuiCallbackDeclarationEnd(MenuView_Presets);
	bool MenuView_Rendering(iWidget* apWidget, const cGuiMessageData& aData);
	kGuiCallbackDeclarationEnd(MenuView_Rendering);

	// iEditorViewport Callbacks implementation
	bool OnViewportUpdate(const cGuiMessageData& aData);
	bool OnViewportGotFocus(const cGuiMessageData& aData);
	bool OnViewportLostFocus(const cGuiMessageData& aData);

	bool OnViewportDraw(const cGuiMessageData& aData);

	bool OnViewportMouseMove(const cGuiMessageData& aData);
	bool OnViewportMouseDown(const cGuiMessageData& aData);
	bool OnViewportMouseUp(const cGuiMessageData& aData);
	bool OnViewportKeyPress(const cGuiMessageData& aData);
	bool OnViewportKeyRelease(const cGuiMessageData& aData);

    ///////////////////////////
	// Implemented Functions
	void OnInit(){}
	void OnInitLayout();
	void OnLoadLevel();
	void OnUpdate(float afTimeStep);

	void OnChangeSize();
	void OnSetActive(bool abX);

	///////////////////////////////////
	// Data
	eEditorWindowViewportPreset mPreset;
	iWidget* mpPrevAttention;
	cViewportCallback mViewportCallback;
	EventHandler<const WorldDrawCtx&> mPreWorldDrawHandler;

	bool mbDrawGrid;
	bool mbDrawDebug;
	bool mbDrawAxes;

	bool mbAddViewMenu;

	// Menu Items
	cWidgetMainMenu* mpMainMenu;
	cWidgetMenuItem* mpMainMenuView;
	// Indexed by eEditorRenderModeItem, NOT eRenderer: Shaded has one entry
	// per backend.
	cWidgetMenuItem* mpMainMenuRenderModes[eEditorRenderModeItem_LastEnum];
	cWidgetMenuItem* mpMainMenuShowGrid;
	cWidgetMenuItem* mpMainMenuShowAxes;

	// Layout specific
	cWidgetLabel* mpPresetLabel;

	cGuiGfxElement* mpGfxRedFrame;
	cGuiGfxElement* mpGfxPointerTumble;
	cGuiGfxElement* mpGfxPointerTrack;
	cGuiGfxElement* mpGfxPointerZoom;

	cVector3f mvNormalPosition;
	cVector3f mvEnlargedPosition;
	cVector2f mvNormalSize;
	cVector2f mvEnlargedSize;

	cVector2l mvNormalFBPos;
	cVector2l mvNormalFBSize;

	cGuiGlobalShortcut* mpShortcutToggleGrid;
};

//---------------------------------------------------------

#endif //HPLEDITOR_EDITOR_WINDOW_VIEWPORT_H

