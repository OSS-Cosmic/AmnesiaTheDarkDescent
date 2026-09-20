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

#ifndef LUX_POST_EFFECTS_H
#define LUX_POST_EFFECTS_H

//----------------------------------------------

#include "LuxBase.h"
#include "graphics/Image.h"
#include "graphics/RIProgram.h"
#include "graphics/PostEffectHelpers.h"

//----------------------------------------

class iLuxPostEffect : public iPostEffect
{
public:
	iLuxPostEffect(cGraphics *apGraphics, cResources *apResources) : iPostEffect(apGraphics, apResources, NULL){}
	virtual ~iLuxPostEffect(){}

	virtual void Update(float afTimeStep){}

protected:
	void OnSetParams(){}
	iPostEffectParams *GetTypeSpecificParams() { return NULL; }
};


//----------------------------------------

class cLuxPostEffect_Insanity : public iLuxPostEffect
{
public:
	cLuxPostEffect_Insanity(cGraphics *apGraphics, cResources *apResources);
	~cLuxPostEffect_Insanity();

	void Update(float afTimeStep);

	void SetWaveAlpha(float afX){ mfWaveAlpha = afX;}
	void SetZoomAlpha(float afX){ mfZoomAlpha = afX;}
	void SetWaveSpeed(float afX){ mfWaveSpeed = afX;}

private:
	void RenderEffect(const hpl::PostEffectRenderCtx &ctx) override;

	hpl::RIProgram m_program;
	// Vulkan-bindless port: textures load through cTextureManager::Create2DImage
	// (cTexture-backed) instead of the legacy iTexture* GL path that crashed
	// in cSDLTexture::CopyTextureDataToGL with no GL context.
	std::vector<Image*> mvAmpMaps;
	Image* mpZoomMap;

	float mfT;
	float mfAnimCount;
	float mfWaveAlpha;
	float mfZoomAlpha;
	float mfWaveSpeed;
};


//----------------------------------------

// Live backdrop for the inventory / journal / escape menu. Replaces the old
// cLuxScreenCapture snapshot + cLuxScreenEffect GUI quads: the gameplay
// viewport keeps rendering (world simulation paused) and this effect blurs or
// desaturate/darkens the final display-space image in place, at negative
// priority (post-tonemap). The active menu drives mfStrength (0 = untouched
// scene, 1 = full effect) for the fade-in crossfade, and toggles SetActive.
//
// It reuses the shaders the snapshot path used:
//   Blur            -> posteffect_bloom_blur.frag (separable H/V ping-pong)
//   DesaturateDarken-> inventory_post.frag (single pass, strength-lerped)

class cLuxPostEffect_MenuBackdrop : public iLuxPostEffect
{
public:
	enum class Mode { Blur, DesaturateDarken };

	cLuxPostEffect_MenuBackdrop(cGraphics *apGraphics, cResources *apResources);
	~cLuxPostEffect_MenuBackdrop();

	void SetMode(Mode aMode){ mMode = aMode; }
	void SetStrength(float afX){ mfStrength = afX; }

private:
	void RenderEffect(const hpl::PostEffectRenderCtx &ctx) override;
	void RenderDesaturate(const hpl::PostEffectRenderCtx &ctx);
	void RenderBlur(const hpl::PostEffectRenderCtx &ctx);
	void EnsureScratch(const hpl::PostEffectRenderCtx &ctx);

	hpl::RIProgram m_blurProgram;  // posteffect_bloom_blur.frag
	hpl::RIProgram m_desatProgram; // inventory_post.frag

	// Ping-pong scratch for the separable blur (Blur mode only). Re-created when
	// the pogo dimensions change, so window resize is handled by the normal path.
	hpl::PostEffectColorTarget m_scratchA;
	hpl::PostEffectColorTarget m_scratchB;

	Mode  mMode      = Mode::Blur;
	float mfStrength = 0.0f;
};


//----------------------------------------------


class cLuxPostEffectHandler : public iLuxUpdateable
{
public:	
	cLuxPostEffectHandler();
	~cLuxPostEffectHandler();

	void OnStart();
	void Update(float afTimeStep);
	void Reset();

	cLuxPostEffect_Insanity* GetInsanity(){ return mpInsanity; }

private:
	void LoadMainConfig();
	void SaveMainConfig();

	void AddEffect(iLuxPostEffect *apPostEffect, int alPrio);

	cLuxPostEffect_Insanity *mpInsanity;

	std::vector<iLuxPostEffect*> mvPostEffects;
	
};

//----------------------------------------------


#endif // LUX_POST_EFFECTS_H
