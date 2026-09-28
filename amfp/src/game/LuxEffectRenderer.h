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

#ifndef LUX_EFFECT_RENDERER_H
#define LUX_EFFECT_RENDERER_H

//----------------------------------------------

#include "LuxBase.h"

#include "graphics/PostEffectHelpers.h"
#include "graphics/RIProgram.h"
#include "system/Event.h"

namespace hpl { struct PostTranslucenceDrawCtx; struct PostWorldDrawCtx; }

//----------------------------------------------

class cGlowObject
{
public:
	cGlowObject() {}
	cGlowObject(iRenderable* apObject, float afAlpha, cColor acColor)
		: mpObject(apObject), mfAlpha(afAlpha), mcColor(acColor) {}

	iRenderable* mpObject;
	float mfAlpha;
	cColor mcColor;
};

//----------------------------------------------

// Hover-outline + pickup-flash + enemy-darkness-glow renderer. Gameplay
// collects renderables each frame via the Add*/Clear* API (unchanged from the
// legacy GL path). The draws run as cViewport handlers: flash + enemy glow are
// TRUE additive into the linear-HDR scene on OnPostTranslucenceDraw (so bloom +
// tonemap soften them, like the original), while the hover outline composites
// over the final tone-mapped pogo read half on OnPostWorldDraw. See
// LuxEffectRenderer.cpp for the pass breakdown.
class cLuxEffectRenderer : public iLuxUpdateable
{
public:
	cLuxEffectRenderer();
	~cLuxEffectRenderer();

	void Reset();

	void Update(float afTimeStep);

	void ClearRenderLists();

	// Connect the OnPostWorldDraw handler to the game viewport (called once
	// from cLuxMapHandler::OnStart). Re-attaching swaps the target viewport.
	void AttachViewport(cViewport* apViewport);

	void AddOutlineObject(iRenderable *apObject, cColor acColor);
	void ClearOutlineObjects();

	void AddFlashObject(iRenderable *apObject, float afAlpha, cColor acColor);
	void AddEnemyGlow(iRenderable *apObject, float afAlpha, cColor acColor);

private:
	// Flash + enemy glow: TRUE additive into the linear-HDR BackBuffer, BEFORE
	// the feed blit + post chain, so bloom + tonemap compress the result into a
	// soft halo (matching the original). Fires on OnPostTranslucenceDraw.
	void OnPostTranslucenceDraw(const hpl::PostTranslucenceDrawCtx& ctx);

	// Hover outline: composites a pre-blurred silhouette over the FINAL
	// tone-mapped image in the pogo read half. Fires on OnPostWorldDraw.
	void OnPostWorldDraw(const hpl::PostWorldDrawCtx& ctx);

	void EnsurePrograms();
	void EnsureTargets(uint32_t alWidth, uint32_t alHeight);

	// Outline: stencil silhouette -> rim color into m_outlineColor, then the
	// separable blur into m_blur[]. Leaves m_blur[1] SHADER_RESOURCE.
	void RenderOutline(const hpl::PostWorldDrawCtx& ctx, const RIDescriptor& aPassDesc);
	void BlurOutline(RICmd* apCmd, uint32_t alBlurW, uint32_t alBlurH);

	std::vector<cGlowObject> mvFlashObjects;
	std::vector<cGlowObject> mvEnemyGlowObjects;

	struct cOutlineObject {
		cOutlineObject(iRenderable *apObject, cColor acColor) : mpObject(apObject), mcColor(acColor) {}
		iRenderable *mpObject;
		cColor mcColor;
	};
	std::vector<cOutlineObject> mvOutlineObjects;

	cViewport* mpViewport;
	EventHandler<const hpl::PostTranslucenceDrawCtx&> mTranslucenceDrawHandler;
	EventHandler<const hpl::PostWorldDrawCtx&> mPostWorldDrawHandler;

	bool mbProgramsLoaded;
	RIProgram mGeomProgram;      // outline_geom.vert + outline_geom.frag (hover outline)
	RIProgram mAlphaProgram;     // outline_alpha.vert + outline_alpha.frag (cutout outline)
	RIProgram mGlowProgram;      // glow_object.vert + glow_object.frag (flash + enemy glow)
	RIProgram mBlurProgram;      // posteffect_fullscreen.vert + posteffect_bloom_blur.frag
	RIProgram mCompositeProgram; // posteffect_fullscreen.vert + outline_composite.frag

	PostEffectColorTarget m_outlineColor; // full-res silhouette/rim target
	PostEffectColorTarget m_blur[2];      // quarter-res ping-pong
	bool mbOutlineColorInit;
	bool mbBlurInit;

	cLinearOscillation mFlashOscill;
};

//----------------------------------------------


#endif // LUX_EFFECT_RENDERER_H
