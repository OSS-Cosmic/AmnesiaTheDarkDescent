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

#ifndef HPLEDITOR_EDITOR_RENDER_MODE_H
#define HPLEDITOR_EDITOR_RENDER_MODE_H

#include "graphics/GraphicsTypes.h"

//---------------------------------------------------------------------------

// The View > Render mode submenu. Its items are NOT the eRenderer enum: the
// lit renderer appears twice, once per backend, so one pick sets both the
// per-viewport mode and the global lit backend.
//
// eRenderer stays the per-viewport choice and is what the layout file stores
// (cEditorWindowViewport::Load/Save), so old layouts keep loading. The backend
// is global - one RIDevice, one active lit renderer - and persists as the
// "RendererBackend" editor setting instead.
enum eEditorRenderModeItem
{
	eEditorRenderModeItem_ShadedStandard = 0,
	eEditorRenderModeItem_ShadedRayTraced,
	eEditorRenderModeItem_WireFrame,
	eEditorRenderModeItem_Simple,
	eEditorRenderModeItem_LastEnum,
};

//---------------------------------------------------------------------------

struct cEditorRenderModeChoice
{
	hpl::eRenderer mRenderer;
	hpl::eRendererBackend mBackend;
	// Only the two Shaded items carry a backend; Wireframe and Simple leave
	// the active one alone.
	bool mbSetsBackend;
};

//---------------------------------------------------------------------------

inline cEditorRenderModeChoice EditorRenderModeFromMenuItem(int alItem)
{
	switch(alItem)
	{
	case eEditorRenderModeItem_ShadedRayTraced:
		return cEditorRenderModeChoice{ hpl::eRenderer_Main, hpl::eRendererBackend_RayTraced, true };
	case eEditorRenderModeItem_WireFrame:
		return cEditorRenderModeChoice{ hpl::eRenderer_WireFrame, hpl::eRendererBackend_Standard, false };
	case eEditorRenderModeItem_Simple:
		return cEditorRenderModeChoice{ hpl::eRenderer_Simple, hpl::eRendererBackend_Standard, false };
	case eEditorRenderModeItem_ShadedStandard:
	default:
		return cEditorRenderModeChoice{ hpl::eRenderer_Main, hpl::eRendererBackend_Standard, true };
	}
}

//---------------------------------------------------------------------------

// Which item is checked, given a viewport's mode and the active backend. Only
// a Shaded viewport tracks the backend.
inline int EditorRenderModeToMenuItem(hpl::eRenderer aRenderer, hpl::eRendererBackend aActiveBackend)
{
	switch(aRenderer)
	{
	case hpl::eRenderer_WireFrame:	return eEditorRenderModeItem_WireFrame;
	case hpl::eRenderer_Simple:		return eEditorRenderModeItem_Simple;
	case hpl::eRenderer_Main:
	default:
		return aActiveBackend==hpl::eRendererBackend_RayTraced
					? eEditorRenderModeItem_ShadedRayTraced
					: eEditorRenderModeItem_ShadedStandard;
	}
}

//---------------------------------------------------------------------------

#endif // HPLEDITOR_EDITOR_RENDER_MODE_H
