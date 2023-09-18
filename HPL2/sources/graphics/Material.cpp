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

#include "graphics/Enum.h"
#include "graphics/GraphicsTypes.h"
#include "graphics/Image.h"
#include "stl/transform.h"
#include "system/LowLevelSystem.h"
#include "system/String.h"

#include "resources/Resources.h"
#include "resources/TextureManager.h"
#include "resources/ImageManager.h"

#include "graphics/Renderable.h"

#include "math/Math.h"
#include <optional>
#include <utility>



#include "tinyimageformat_query.h"

namespace hpl {

        eMaterialBlendMode cMaterial::GetBlendMode() const {
            // for water we enforce a blend mode
            switch(m_descriptor.m_id) {
                case MaterialID::Translucent:
                    return m_descriptor.m_translucent.m_blend;
                case MaterialID::Decal:
                    return m_descriptor.m_decal.m_blend;
                default:
                    break;
            }
            ASSERT(false && "material type does not have a blend mode");
            return eMaterialBlendMode_LastEnum;
        }
        bool cMaterial::IsAffectedByLightLevel() const {
            switch(m_descriptor.m_id) {
                case MaterialID::Translucent:
                    return m_descriptor.m_translucent.m_isAffectedByLightLevel;
                default:
                    break;
            }
            return false;
        }

        eMaterialAlphaMode cMaterial::GetAlphaMode() const {
            switch(m_descriptor.m_id) {
                case MaterialID::SolidDiffuse:
                    if(GetImage(eMaterialTexture_Alpha))  {
                        return eMaterialAlphaMode_Trans;
                    }
                    break;
                default:
                    break;
            }
	        return eMaterialAlphaMode_Solid;
        }
        bool cMaterial::GetHasRefraction() const {
            switch(m_descriptor.m_id) {
                case MaterialID::Translucent:
                    return m_descriptor.m_translucent.m_hasRefraction;
                case MaterialID::Water:
                    return true;
                default:
                    break;
            }
            return false;
        }
        bool cMaterial::GetHasReflection() const {
            switch(m_descriptor.m_id) {
                case MaterialID::Water:
                    return m_descriptor.m_water.m_hasReflection;
                default:
                    break;
            }
            return false;
        }

        bool cMaterial::GetHasWorldReflections() const {
	        return m_descriptor.m_id == MaterialID::Water &&
		        GetHasReflection() &&
		        !GetImage(eMaterialTexture_CubeMap);
        }

        bool cMaterial::GetLargeTransperantSurface() const {

            switch(m_descriptor.m_id) {
                case MaterialID::Water:
                    return m_descriptor.m_water.m_isLargeSurface;
                default:
                    break;
            }
            return false;
        }

        float cMaterial::maxReflectionDistance() const {
            switch(m_descriptor.m_id) {
                case MaterialID::Water:
                    return m_descriptor.m_water.m_maxReflectionDistance;
                default:
                    break;
            }
            return 0.0f;
        }


	cMaterial::cMaterial(const tString& asName, const tWString& asFullPath, cGraphics *apGraphics, cResources *apResources)
		: iResourceBase(asName, asFullPath, 0)
	{
        m_generation = rand();
		mpResources = apResources;

		mbAutoDestroyTextures = true;

		mlRenderFrameCount = -1;

	   // mbHasRefraction = false;
	   // mlRefractionTextureUnit =0;
	   // mbUseRefractionEdgeCheck = false;

	   // mlWorldReflectionTextureUnit =0;
	   // mfMaxReflectionDistance = 0;

	   // mbLargeTransperantSurface = false;

	   // mbUseAlphaDissolveFilter = false;


		mfAnimTime = 0;
		m_mtxUV = cMatrixf::Identity;
		mbHasUvAnimation = false;

		mbDepthTest = true;

	}

	//-----------------------------------------------------------------------

	cMaterial::~cMaterial()
	{
	}


	void cMaterial::Compile()
	{
	}

    void cMaterial::SetTextureAnisotropy(float afx) {
        if(afx >= 16.0f) {
            m_antistropy = Antistropy_16;
        } else if(afx >= 8.0f) {
            m_antistropy = Antistropy_8;
        } else {
            m_antistropy = Antistropy_None ;
        }
        IncreaseGeneration();
    }

	void cMaterial::SetImage(eMaterialTexture aType, iResourceBase *apTexture)
	{
		// increase version number to dirty material
		m_image[aType].SetAutoDestroyResource(false);
		if(apTexture) {
			ASSERT(TypeInfo<Image>().IsType(*apTexture) || TypeInfo<AnimatedImage>().IsType(*apTexture) && "cMaterial::SetImage: apTexture is not an Image");
			m_image[aType] = std::move(ImageResourceWrapper(mpResources->GetTextureManager(), apTexture, mbAutoDestroyTextures));
		} else {
			m_image[aType] = ImageResourceWrapper();
		}
        IncreaseGeneration();
	}

	Image* cMaterial::GetImage(eMaterialTexture aType)
	{
		return m_image[aType].GetImage();
	}

    const Image* cMaterial::GetImage(eMaterialTexture aType) const
	{
		return m_image[aType].GetImage();
	}


	void cMaterial::SetDepthTest(bool abDepthTest)
	{
	    // strange ..
        if(hpl::stl::TransformOrDefault(Meta(), false,
                            [](auto& meta) {
                                return !meta->m_isTranslucent;
                            })) {
            return;
        }

		mbDepthTest = abDepthTest;
	}

	void cMaterial::UpdateBeforeRendering(float afTimeStep)
	{
		if(mbHasUvAnimation) UpdateAnimations(afTimeStep);
	}

	void cMaterial::AddUvAnimation(eMaterialUvAnimation aType, float afSpeed, float afAmp, eMaterialAnimationAxis aAxis)
	{
		mvUvAnimations.push_back(cMaterialUvAnimation(aType, afSpeed, afAmp, aAxis));

		mbHasUvAnimation = true;
	}

	void cMaterial::ClearUvAnimations()
	{
		mvUvAnimations.clear();

		mbHasUvAnimation = false;

		m_mtxUV = cMatrixf::Identity;
	}

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

    std::optional<const cMaterial::MaterialMeta*> cMaterial::Meta() {
	    auto metaInfo = std::find_if(cMaterial::MaterialMetaTable.begin(), cMaterial::MaterialMetaTable.end(), [&](auto& meta) {
	        return m_descriptor.m_id == meta.m_id;
	    });
	    return (metaInfo == cMaterial::MaterialMetaTable.end()) ?
            std::nullopt : std::optional(metaInfo);

    }
	void cMaterial::UpdateAnimations(float afTimeStep) {
		m_mtxUV = cMatrixf::Identity;

        for(size_t i=0; i<mvUvAnimations.size(); ++i)
		{
			cMaterialUvAnimation *pAnim = &mvUvAnimations[i];

			///////////////////////////
			// Translate
			if(pAnim->mType == eMaterialUvAnimation_Translate)
			{
				cVector3f vDir = GetAxisVector(pAnim->mAxis);

				cMatrixf mtxAdd = cMath::MatrixTranslate(vDir * pAnim->mfSpeed * mfAnimTime);
				m_mtxUV = cMath::MatrixMul(m_mtxUV, mtxAdd);
			}
			///////////////////////////
			// Sin
			else if(pAnim->mType == eMaterialUvAnimation_Sin)
			{
				cVector3f vDir = GetAxisVector(pAnim->mAxis);

				cMatrixf mtxAdd = cMath::MatrixTranslate(vDir * sin(mfAnimTime * pAnim->mfSpeed) * pAnim->mfAmp);
				m_mtxUV = cMath::MatrixMul(m_mtxUV, mtxAdd);
			}
			///////////////////////////
			// Rotate
			else if(pAnim->mType == eMaterialUvAnimation_Rotate)
			{
				cVector3f vDir = GetAxisVector(pAnim->mAxis);

				cMatrixf mtxRot = cMath::MatrixRotate(vDir * pAnim->mfSpeed * mfAnimTime,eEulerRotationOrder_XYZ);
				m_mtxUV = cMath::MatrixMul(m_mtxUV, mtxRot);
			}
		}

		m_generation++;
		mfAnimTime += afTimeStep;
	}

	//-----------------------------------------------------------------------

}
