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

#include "graphics/Material.h"
#include "scene/RenderInterpolation.h"

#include <cmath>

#include "system/LowLevelSystem.h"
#include "system/String.h"

#include "resources/Resources.h"
#include "resources/TextureManager.h"
#include "resources/ImageManager.h"

#include "graphics/Graphics.h"
#include "graphics/Image.h"
#include "graphics/MaterialType.h"

#include "math/Math.h"

#include <type_traits>


namespace hpl {

	//////////////////////////////////////////////////////////////////////////
	// CONSTRUCTORS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	cMaterial::cMaterial(const tString& asName, const tWString& asFullPath, cGraphics *apGraphics, cResources *apResources, iMaterialType *apType)
		: iResourceBase(asName, asFullPath, 0)
	{
		mpGraphics = apGraphics;
		mpResources = apResources;

		mpType = NULL;
		SetType(apType);

		mbAutoDestroyTextures = true;

		mlRenderFrameCount = -1;

		mbWorldReflectionOcclusionTest = true;

		mbLargeTransperantSurface = false;

		mfAnimTime = 0;
		m_mtxUV = cMatrixf::Identity;

		///////////////////////
		//Set up depending in type
		if(mpType->IsTranslucent())
		{
			mBlendMode = eMaterialBlendMode_Add;
		}
		else
		{
			mBlendMode = eMaterialBlendMode_None;
		}
		mbDepthTest = true;
	}

	//-----------------------------------------------------------------------

	cMaterial::~cMaterial()
	{
		// Texture images own their own lifecycle via the SharedResourceHandle<Image>
		// members inside the m_data variant alternative — destroying the variant
		// drops each handle's reference (freeing the image when it was the last one).
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PUBLIC METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	void cMaterial::SetType(iMaterialType* apType)
	{
		if(mpType==apType) return;

		mpType = apType;

		// Establish the variant alternative up front so SetImage (called by the
		// loader before LoadVariables) has a slot to route textures into.
		switch(mpType ? mpType->GetMaterialID() : MaterialID::Unknown)
		{
		case MaterialID::SolidDiffuse: m_data.emplace<MaterialDiffuseSolid>(); break;
		case MaterialID::Translucent:  m_data.emplace<MaterialTranslucent>();  break;
		case MaterialID::Water:        m_data.emplace<MaterialWater>();        break;
		case MaterialID::Decal:        m_data.emplace<MaterialDecal>();        break;
		default:                       m_data.emplace<std::monostate>();       break;
		}
	}

	//-----------------------------------------------------------------------

	MaterialID cMaterial::GetMaterialID() const
	{
		return std::visit([](auto&& a) -> MaterialID {
			using T = std::decay_t<decltype(a)>;
			if constexpr (std::is_same_v<T, MaterialDiffuseSolid>) return MaterialID::SolidDiffuse;
			else if constexpr (std::is_same_v<T, MaterialTranslucent>) return MaterialID::Translucent;
			else if constexpr (std::is_same_v<T, MaterialWater>) return MaterialID::Water;
			else if constexpr (std::is_same_v<T, MaterialDecal>) return MaterialID::Decal;
			else return MaterialID::Unknown;
		}, m_data);
	}

	//-----------------------------------------------------------------------

	// Maps an eMaterialTexture slot onto the matching SharedResourceHandle<Image> field
	// of whatever variant alternative is currently held. Returns nullptr for slots the
	// alternative does not have (incl. eMaterialTexture_Special and monostate), so
	// Get/SetImage stay no-ops there and every existing enum-based caller keeps
	// working unchanged.
	SharedResourceHandle<Image>* cMaterial::SlotForTexture(eMaterialTexture aType)
	{
		return std::visit([aType](auto& a) -> SharedResourceHandle<Image>* {
			using T = std::decay_t<decltype(a)>;
			if constexpr (std::is_same_v<T, MaterialDiffuseSolid>)
			{
				switch(aType)
				{
				case eMaterialTexture_Diffuse:      return &a.m_diffuse;
				case eMaterialTexture_NMap:         return &a.m_nmap;
				case eMaterialTexture_Specular:     return &a.m_specular;
				case eMaterialTexture_Alpha:        return &a.m_alpha;
				case eMaterialTexture_Height:       return &a.m_height;
				case eMaterialTexture_Illumination: return &a.m_illumination;
				case eMaterialTexture_DissolveAlpha:return &a.m_dissolveAlpha;
				case eMaterialTexture_CubeMap:      return &a.m_cubeMap;
				case eMaterialTexture_CubeMapAlpha: return &a.m_cubeMapAlpha;
				default: return nullptr;
				}
			}
			else if constexpr (std::is_same_v<T, MaterialTranslucent>)
			{
				switch(aType)
				{
				case eMaterialTexture_Diffuse:      return &a.m_diffuse;
				case eMaterialTexture_NMap:         return &a.m_nmap;
				case eMaterialTexture_CubeMap:      return &a.m_cubeMap;
				case eMaterialTexture_CubeMapAlpha: return &a.m_cubeMapAlpha;
				default: return nullptr;
				}
			}
			else if constexpr (std::is_same_v<T, MaterialWater>)
			{
				switch(aType)
				{
				case eMaterialTexture_Diffuse: return &a.m_diffuse;
				case eMaterialTexture_NMap:    return &a.m_nmap;
				case eMaterialTexture_CubeMap: return &a.m_cubeMap;
				default: return nullptr;
				}
			}
			else if constexpr (std::is_same_v<T, MaterialDecal>)
			{
				return aType == eMaterialTexture_Diffuse ? &a.m_diffuse : nullptr;
			}
			else
			{
				return nullptr; // monostate
			}
		}, m_data);
	}

	//-----------------------------------------------------------------------

	bool cMaterial::GetUseAlphaDissolveFilter() const
	{
		if(auto* d = std::get_if<MaterialDiffuseSolid>(&m_data))
			return d->m_alphaDissolveFilter;
		return false;
	}

	//-----------------------------------------------------------------------

	// Derived from the data blob (global refraction toggle assumed on): translucent
	// refracts per its authored flag, water always refracts.
	bool cMaterial::HasRefraction() const
	{
		if(std::get_if<MaterialWater>(&m_data))
			return true;
		if(auto* t = std::get_if<MaterialTranslucent>(&m_data))
			return t->m_refraction;
		return false;
	}

	// World (planar) reflection: water only, when authored and no cubemap is bound.
	bool cMaterial::HasWorldReflection() const
	{
		if(auto* w = std::get_if<MaterialWater>(&m_data))
			return w->m_hasReflection && GetImage(eMaterialTexture_CubeMap) == nullptr;
		return false;
	}

	//-----------------------------------------------------------------------

	void cMaterial::SetImage(eMaterialTexture aType, Image* apImage)
	{
		if(SharedResourceHandle<Image>* pSlot = SlotForTexture(aType))
			*pSlot = SharedResourceHandle<Image>(mpResources->GetTextureManager(), apImage);
	}

	Image* cMaterial::GetImage(eMaterialTexture aType) const
	{
		if(SharedResourceHandle<Image>* pSlot = const_cast<cMaterial*>(this)->SlotForTexture(aType))
			return pSlot->Get();
		return nullptr;
	}

	//-----------------------------------------------------------------------

	void cMaterial::setTextureWrap(eTextureWrap aWrap)
	{
		m_textureWrap = aWrap;
	}

	void cMaterial::setTextureFilter(eTextureFilter aFilter)
	{
		m_textureFilter = aFilter;
	}

	void cMaterial::SetTextureAnisotropy(float afX)
	{
		m_textureAnisotropy = afX;
	}

	//-----------------------------------------------------------------------

	cResourceVarsObject* cMaterial::GetVarsObject()
	{
		cResourceVarsObject* pVarsObject = hplNew(cResourceVarsObject,());
		mpType->GetVariableValues(this, pVarsObject);

		return pVarsObject;
	}

	//-----------------------------------------------------------------------

	void cMaterial::LoadVariablesFromVarsObject(cResourceVarsObject* apVarsObject)
	{
		// Editor-side vars refresh: textures are already bound via SetImage, so this
		// only re-reads the per-type vars (texture assignment lives at the loader).
		mpType->LoadVariables(this, apVarsObject);
	}

	//-----------------------------------------------------------------------

	void cMaterial::SetBlendMode(eMaterialBlendMode aBlendMode)
	{
		if(mpType->IsTranslucent()==false) return;

		mBlendMode = aBlendMode;
	}

	// Derived from the data blob + alpha binding: a SolidDiffuse material with a
	// bound alpha texture is alpha-tested (Trans); every other case is Solid.
	eMaterialAlphaMode cMaterial::GetAlphaMode() const
	{
		if(std::get_if<MaterialDiffuseSolid>(&m_data) && GetImage(eMaterialTexture_Alpha))
			return eMaterialAlphaMode_Trans;
		return eMaterialAlphaMode_Solid;
	}

	void cMaterial::SetDepthTest(bool abDepthTest)
	{
		if(mpType->IsTranslucent()==false) return;

		mbDepthTest = abDepthTest;
	}

	//-----------------------------------------------------------------------

	void cMaterial::UpdateBeforeRendering(float afTimeStep)
	{
		if(HasUvAnimation()) UpdateAnimations(afTimeStep);
	}

	//-----------------------------------------------------------------------

	void cMaterial::AddUvAnimation(eMaterialUvAnimation aType, float afSpeed, float afAmp, eMaterialAnimationAxis aAxis)
	{
		mvUvAnimations.push_back(cMaterialUvAnimation(aType, afSpeed, afAmp, aAxis));
	}

	//-----------------------------------------------------------------------

	void cMaterial::ClearUvAnimations()
	{
		mvUvAnimations.clear();

		m_mtxUV = cMatrixf::Identity;
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PRIVATE METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	static cVector3f GetAxisVector(eMaterialAnimationAxis aAxis)
	{
		switch(aAxis)
		{
		case eMaterialAnimationAxis_X: return cVector3f(1,0,0);
		case eMaterialAnimationAxis_Y: return cVector3f(0,1,0);
		case eMaterialAnimationAxis_Z: return cVector3f(0,0,1);
		}
		return 0;
	}

	//-----------------------------------------------------------------------

	void cMaterial::UpdateAnimations(float afTimeStep)
	{
		m_mtxUV = cMatrixf::Identity;

		//Animation time is simulated game time at the moment the frame represents.
		//This is smooth at any frame rate and follows the game speed (fast forward,
		//slow motion), instead of accumulating real frame time.
		const double dTime = cRenderInterpolation::GetRenderGameTime();
		mfAnimTime = (float)dTime;

        for(size_t i=0; i<mvUvAnimations.size(); ++i)
		{
			cMaterialUvAnimation *pAnim = &mvUvAnimations[i];

			///////////////////////////
			// Translate
			if(pAnim->mType == eMaterialUvAnimation_Translate)
			{
				cVector3f vDir = GetAxisVector(pAnim->mAxis);

				//Wrap at whole texture repeats, so precision does not degrade over a long session.
				float fOffset = (float)fmod((double)pAnim->mfSpeed * dTime, 1.0);

				cMatrixf mtxAdd = cMath::MatrixTranslate(vDir * fOffset);
				m_mtxUV = cMath::MatrixMul(m_mtxUV, mtxAdd);
			}
			///////////////////////////
			// Sin
			else if(pAnim->mType == eMaterialUvAnimation_Sin)
			{
				cVector3f vDir = GetAxisVector(pAnim->mAxis);

				float fSin = (float)sin(dTime * (double)pAnim->mfSpeed);

				cMatrixf mtxAdd = cMath::MatrixTranslate(vDir * fSin * pAnim->mfAmp);
				m_mtxUV = cMath::MatrixMul(m_mtxUV, mtxAdd);
			}
			///////////////////////////
			// Rotate
			else if(pAnim->mType == eMaterialUvAnimation_Rotate)
			{
				cVector3f vDir = GetAxisVector(pAnim->mAxis);

				float fAngle = (float)fmod((double)pAnim->mfSpeed * dTime, 2.0 * 3.14159265358979323846);

				cMatrixf mtxRot = cMath::MatrixRotate(vDir * fAngle,eEulerRotationOrder_XYZ);
				m_mtxUV = cMath::MatrixMul(m_mtxUV, mtxRot);
			}
		}
	}

	//-----------------------------------------------------------------------

}
