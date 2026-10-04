/*
 * Copyright © 2011-2020 Frictional Games
 * 
 * This file is part of Amnesia: A Machine For Pigs.
 * 
 * Amnesia: A Machine For Pigs is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version. 

 * Amnesia: A Machine For Pigs is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with Amnesia: A Machine For Pigs.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef LUX_POST_EFFECTS_H
#define LUX_POST_EFFECTS_H

//----------------------------------------------

#include "LuxBase.h"
#include "graphics/Image.h"
#include "graphics/RIProgram.h"

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

class cLuxPostEffect_Infection : public iLuxPostEffect
{
public:
	cLuxPostEffect_Infection(cGraphics *apGraphics, cResources *apResources);
	~cLuxPostEffect_Infection();

	void Update(float afTimeStep);

	void SetWaveAlpha(float afX){ mfWaveAlpha = afX;}
	void SetZoomAlpha(float afX){ mfZoomAlpha = afX;}
	void SetInfectionGoal(float afX){ mfInfectionGoal = afX;}
	void SetVomitBlendFactor(float afX){ mfVomitBlendFactor = afX;}
	void SetWaveSpeed(float afX){ mfWaveSpeed = afX;}

private:
	void RenderEffect(const hpl::PostEffectRenderCtx &ctx) override;

	hpl::RIProgram m_program;
	std::vector<hpl::SharedResourceHandle<hpl::Image>> mvAmpMaps;
	hpl::SharedResourceHandle<hpl::Image> mpZoomMap;
	hpl::SharedResourceHandle<hpl::Image> mpInfectionNormalBlendMap;
	hpl::SharedResourceHandle<hpl::Image> mpInfectionOverlayBlendMap;
	hpl::SharedResourceHandle<hpl::Image> mpInfectionColorDodgeBlendMap;
	hpl::SharedResourceHandle<hpl::Image> mpInfectionGradientMap;
	float mfT;
	float mfAnimCount;
	float mfWaveAlpha;
	float mfZoomAlpha;
	float mfWaveSpeed;
	float mfInfectionFactor;
	float mfInfectionGoal;
	float mfInfectionGrowSpeed;
	float mfGradientThresholdOffset;
	float mfGradientFallofExponent;
	float mfInfectionMapZoom;
	float mfVomitBlendFactor;
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

	cLuxPostEffect_Infection* GetInfection(){ return mpInfection; }

private:
	void LoadMainConfig();
	void SaveMainConfig();

	void AddEffect(iLuxPostEffect *apPostEffect, int alPrio);

	cLuxPostEffect_Infection *mpInfection;

	std::vector<iLuxPostEffect*> mvPostEffects;
	
};

//----------------------------------------------


#endif // LUX_POST_EFFECTS_H
