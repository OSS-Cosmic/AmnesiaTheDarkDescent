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

#include "scene/LightSpot.h"
#include "graphics/Image.h"

#include "resources/XmlHelper.h"
#include "math/Math.h"
#include "math/Frustum.h"
#include "resources/TextureManager.h"
#include "resources/Resources.h"
#include "scene/Camera.h"

#include "scene/World.h"
#include "scene/Scene.h"
#include "engine/Engine.h"

#include "system/String.h"

namespace hpl {
	
	static const cMatrixf g_mtxTextureUnitFix(	0.5f,0,   0,   0.5f,
												0,   0.5f,0,   0.5f,
												0,   0,   0.5f,0.5f,
												0,   0,   0,   1.0f
												);
			
	//////////////////////////////////////////////////////////////////////////
	// CONSTRUCTORS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------
	
	iLightSpot::iLightSpot(tString asName, cResources *apResources) : iLight(asName,apResources)
	{
		mbProjectionUpdated = true;
		mbViewProjUpdated = true;
		mbFrustumUpdated = true;
		
        mLightType = eLightType_Spot;

		mpFrustum = hplNew( cFrustum, () );

		mlViewProjMatrixCount =-1;
		mlViewMatrixCount =-1;
		mlFrustumMatrixCount =-1;

		mfFOV = cMath::ToRad(60.0f);
		mfAspect = 1.0f;
		mfNearClipPlane = 0.1f;
		mfIntensity = 100.0f;

		mfTanHalfFOV = tan(mfFOV*0.5f);
		mfCosHalfFOV = cos(mfFOV*0.5f);

		mbFovUpdated = true;
		
		m_mtxView = cMatrixf::Identity;
		m_mtxViewProj = cMatrixf::Identity;
		m_mtxProjection = cMatrixf::Identity;

		// Forward+ uses a clamp-to-edge static sampler at the spot-falloff binding,
		// so the per-texture wrap call from the legacy path is unnecessary.
		SetSpotFalloffMap(mpTextureManager->Create1DImage("core_falloff_linear", false).Release());

		UpdateBoundingVolume();
	}
	
	iLightSpot::~iLightSpot()
	{
		// m_spotFalloffMap (SharedResourceHandle) frees itself.
		hplDelete(mpFrustum);
	}

	cLightSpotLegacy::cLightSpotLegacy(tString asName, cResources *apResources)
		: iLightSpot(asName, apResources)
	{
	}

	//-----------------------------------------------------------------------

	cLightSpot::cLightSpot(tString asName, cResources *apResources)
		: iLightSpot(asName, apResources)
	{
		mLightModel = eLightModel_Overdrive;
	}
	
	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PUBLIC METHODS
	//////////////////////////////////////////////////////////////////////////
	
	//-----------------------------------------------------------------------

	

	void iLightSpot::SetRadius(float afX)
	{
		mbProjectionUpdated = true;
		iLight::SetRadius(afX);
	}

	

	

	void iLightSpot::SetFOV(float afAngle)
	{ 
		mfFOV = afAngle;
		mbProjectionUpdated = true;

		mfTanHalfFOV = tan(mfFOV*0.5f);
		mfCosHalfFOV = cos(mfFOV*0.5f);
	}
	
	//-----------------------------------------------------------------------
	
	const cMatrixf& iLightSpot::GetViewMatrix()
	{
		if(mlViewMatrixCount != GetTransformUpdateCount())
		{
			mlViewMatrixCount = GetTransformUpdateCount();
			m_mtxView = cMath::MatrixInverse(GetWorldMatrix());
		}

		return m_mtxView;
	}

	//-----------------------------------------------------------------------


	const cMatrixf& iLightSpot::GetProjectionMatrix()
	{
		if(mbProjectionUpdated)
		{
			// VK clip convention (z in [0,1]), same form as the camera path —
			// cFrustum::UpdatePlanes extracts the near plane as row2 alone.
			m_mtxProjection = cMath::MatrixPerspectiveProjection(
				mfNearClipPlane, GetReach(), mfFOV, mfAspect, false);

			mbProjectionUpdated = false;
			mbViewProjUpdated = true;
			mbFrustumUpdated = true;
		}

		return m_mtxProjection;
	}

	//-----------------------------------------------------------------------
	
	const cMatrixf& iLightSpot::GetViewProjMatrix()
	{
		if(mlViewProjMatrixCount != GetTransformUpdateCount() || mbViewProjUpdated || mbProjectionUpdated)
		{
			m_mtxViewProj = cMath::MatrixMul(GetProjectionMatrix(),GetViewMatrix());
			m_mtxViewProj = cMath::MatrixMul(g_mtxTextureUnitFix, m_mtxViewProj);
			
			mlViewProjMatrixCount = GetTransformUpdateCount();
			mbViewProjUpdated = false;
		}

		return m_mtxViewProj;
	}

	//-----------------------------------------------------------------------

	cFrustum* iLightSpot::GetFrustum()
	{
		if(mlFrustumMatrixCount != GetTransformUpdateCount() || mbFrustumUpdated || mbProjectionUpdated)
		{
			mpFrustum->SetupPerspectiveProj(GetProjectionMatrix(),
											GetViewMatrix(),
											GetReach(),mfNearClipPlane,
											mfFOV,mfAspect,GetWorldPosition(),false);
			mbFrustumUpdated = false;
			mlFrustumMatrixCount = GetTransformUpdateCount();
		}

		return mpFrustum;
	}

	//-----------------------------------------------------------------------

	void iLightSpot::SetSpotFalloffMap(Image* apImage)
	{
		m_spotFalloffMap = SharedResourceHandle<Image>(mpTextureManager, apImage); // adopt transferred ref
	}

	Image* iLightSpot::GetSpotFalloffImage() const
	{
		return m_spotFalloffMap.Get();
	}

	//-----------------------------------------------------------------------


	bool iLightSpot::CollidesWithBV(cBoundingVolume *apBV)
	{
		if(cMath::CheckBVIntersection(*GetBoundingVolume(), *apBV)==false) return false;

		return GetFrustum()->CollideBoundingVolume(apBV)!= eCollision_Outside;
	}
	
	//-----------------------------------------------------------------------

	bool iLightSpot::CollidesWithFrustum(cFrustum *apFrustum)
	{
		return apFrustum->CollideFrustum(GetFrustum())!=eCollision_Outside;
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PRIVATE METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	
	static eTextureAnimMode GetAnimMode(const tString& asType)
	{
		if(cString::ToLowerCase(asType) == "none") return eTextureAnimMode_None;
		else if(cString::ToLowerCase(asType) == "loop") return eTextureAnimMode_Loop;
		else if(cString::ToLowerCase(asType) == "oscillate") return eTextureAnimMode_Oscillate;

		return eTextureAnimMode_None;
	}

	void iLightSpot::ExtraXMLProperties(tinyxml2::XMLElement *apMainElem)
	{
		tString sTexture = GetAttributeString(apMainElem, "ProjectionImage");

		eTextureAnimMode animMode = GetAnimMode(GetAttributeString(apMainElem, "ProjectionAnimMode", "None"));
		float fFrameTime = GetAttributeFloat(apMainElem, "ProjectionFrameTime", 1.0f);
		SharedResourceHandle<Image> pImage;

		if(animMode != eTextureAnimMode_None)
		{
			pImage = mpTextureManager->CreateAnimImage(sTexture,true,eTextureType_2D,
				eTextureUsage_Normal,0,false,animMode,fFrameTime);
		}
		else
		{
			pImage = mpTextureManager->Create2DImage(sTexture,true);
		}

		if(pImage)
		{
			SetGoboTexture(pImage.Release());
		}

		mfAspect = GetAttributeFloat(apMainElem, "Aspect", mfAspect);

		mfNearClipPlane = GetAttributeFloat(apMainElem, "NearClipPlane", mfNearClipPlane);
	}

	//-----------------------------------------------------------------------

	void iLightSpot::UpdateBoundingVolume()
	{
		mBoundingVolume = GetFrustum()->GetBoundingVolume();
	}

	//-----------------------------------------------------------------------
	
}
