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

#include "resources/MaterialManager.h"
#include "graphics/Image.h"

#include "system/LowLevelSystem.h"
#include "system/String.h"
#include "system/System.h"
#include "system/Platform.h"

#include "graphics/Graphics.h"
#include "graphics/Material.h"
#include "graphics/MaterialType.h"

#include "resources/TextureManager.h"
#include "resources/Resources.h"
#include "resources/LowLevelResources.h"
#include "resources/XmlHelper.h"

#include <tinyxml2.h>



namespace hpl {

	//////////////////////////////////////////////////////////////////////////
	// BLANK MATERIAL
	//////////////////////////////////////////////////////////////////////////
	
	//-----------------------------------------------------------------------

	class cMaterialManagerBlankMaterialType : public iMaterialType
	{
	public:
		cMaterialManagerBlankMaterialType() : iMaterialType(NULL, NULL){}

		void LoadData(){}
		void DestroyData(){}

		bool SupportsHWSkinning(){ return false; }

		void LoadVariables(cMaterial *apMaterial, cResourceVarsObject *apVars){ }
		void GetVariableValues(cMaterial *apMaterial, cResourceVarsObject *apVars){ }
	};
	
	cMaterialManagerBlankMaterialType gBlankMaterialType;
    

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// CONSTRUCTORS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	cMaterialManager::cMaterialManager(cGraphics* apGraphics,cResources *apResources)
		: iResourceManager(apResources->GetFileSearcher(), apResources->GetLowLevel(),apResources->GetLowLevelSystem(), true)
	{
		mpGraphics = apGraphics;
		mpResources = apResources;

		mlTextureSizeDownScaleLevel =0;
		mTextureFilter = eTextureFilter_Bilinear;
		mfTextureAnisotropy = 1.0f;

		mbDisableRenderDataLoading = false;

		mlIdCounter =0;
	}

	cMaterialManager::~cMaterialManager()
	{
        DestroyAll();

		Log(" Done with materials\n");
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PUBLIC METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	SharedResourceHandle<cMaterial> cMaterialManager::CreateMaterial(const tString& asName)
	{
		if(asName=="")
			return {};

		tWString sPath;
		cMaterial* pMaterial;
		tString asNewName;

		BeginLoad(asName);

		asNewName = cString::SetFileExt(asName,"mat");

		pMaterial = static_cast<cMaterial*>(this->FindLoadedResource(asNewName,sPath));

		if(pMaterial==NULL && sPath!=_W(""))
		{
			pMaterial = LoadFromFile(asNewName,sPath);

			if(pMaterial==NULL){
				Error("Couldn't load material '%s'\n",asNewName.c_str());
				EndLoad();
				return {};
			}

			AddResource(pMaterial);
		}

		if(!pMaterial)
			Error("Couldn't create material '%s'\n",asNewName.c_str());

		EndLoad();
		return AcquireResource<cMaterial>(this, pMaterial); // handle takes the reference (empty if null)
	}

	//-----------------------------------------------------------------------

	void cMaterialManager::Update(float afTimeStep)
	{
		
	}

	//-----------------------------------------------------------------------

	void cMaterialManager::Unload(iResourceBase* apResource)
	{

	}
	//-----------------------------------------------------------------------

	//-----------------------------------------------------------------------

	void cMaterialManager::SetTextureFilter(eTextureFilter aFilter)
	{
		if(aFilter == mTextureFilter) return;
		mTextureFilter = aFilter;

		tResourceBaseMapIt it = m_mapResources.begin();
		for(; it != m_mapResources.end(); ++it)
		{
			cMaterial *pMat = static_cast<cMaterial*>(it->second);
			pMat->setTextureFilter(aFilter);
		}
	}

	//-----------------------------------------------------------------------

	tString cMaterialManager::GetPhysicsMaterialName(const tString& asName)
	{
		tWString sPath;
		cMaterial* pMaterial;
		tString asNewName;

		asNewName = cString::SetFileExt(asName,"mat");

		pMaterial = static_cast<cMaterial*>(this->FindLoadedResource(asNewName,sPath));

		if(pMaterial==NULL && sPath!=_W(""))
		{
			tinyxml2::XMLDocument xmlDoc;
			if(LoadXmlFile(xmlDoc, sPath)==false || xmlDoc.RootElement()==NULL)
			{
				return "";
			}

			tinyxml2::XMLElement *pRoot = xmlDoc.RootElement();

			tinyxml2::XMLElement *pMain = pRoot->FirstChildElement("Main");
			if(pMain==NULL){
				Error("Main child not found in '%s'\n",sPath.c_str());
				return "";
			}

			tString sPhysicsName = cString::ToString(pMain->Attribute("PhysicsMaterial"),"Default");

			return sPhysicsName;
		}

		if(pMaterial)
			return pMaterial->GetPhysicsMaterial();
		else
			return "";
	}

	//-----------------------------------------------------------------------

	SharedResourceHandle<cMaterial> cMaterialManager::CreateCustomMaterial(const tString& asName, iMaterialType *apMaterialType)
	{
		cMaterial* pMat = hplNew( cMaterial, (asName, cString::To16Char(asName), mpGraphics, mpResources, apMaterialType) );
		AddResource(pMat);
		return AcquireResource<cMaterial>(this, pMat);
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PRIVATE METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------
	cMaterial* cMaterialManager::LoadFromFile(const tString& asName,const tWString& asPath)
	{
		//Log("Load material: %s\n", asName.c_str());

		tinyxml2::XMLDocument xmlDoc;
		if(LoadXmlFile(xmlDoc, asPath)==false || xmlDoc.RootElement()==NULL)
		{
			return NULL;
		}
		tinyxml2::XMLElement* pDoc = xmlDoc.RootElement();

		tinyxml2::XMLElement* pMain = pDoc->FirstChildElement("Main");
		if(pMain==NULL)
		{
			Error("Main child not found.\n");
			return NULL;
		}

		tString sType = GetAttributeString(pMain, "Type");
		if(sType=="")
		{
			Error("Type not found.\n");
			return NULL;
		}

		/////////////////////////////
		// Get General Propertries
		bool bDepthTest = GetAttributeBool(pMain, "DepthTest", true);
		float fValue = GetAttributeFloat(pMain, "Value", 1);
		tString sPhysicsMatName = GetAttributeString(pMain, "PhysicsMaterial", "Default");
		tString sBlendMode = GetAttributeString(pMain, "BlendMode", "Add");

		/////////////////////////////
		// Make a "fake" material, with a blank type
		if(mbDisableRenderDataLoading)
		{
			cMaterial* pMat = hplNew( cMaterial, (asName, asPath, mpGraphics, mpResources, &gBlankMaterialType) );
			pMat->SetPhysicsMaterial(sPhysicsMatName);

			return pMat;
		}

		/////////////////////////////
		// CreateType
		iMaterialType *pMatType = mpGraphics->GetMaterialType(sType);
		if(pMatType ==NULL)
		{
			Error("Invalid material type '%s'!\n",sType.c_str());
			return NULL;
		}
		cMaterial* pMat = hplNew( cMaterial, (asName, asPath, mpGraphics, mpResources, pMatType) );
		
		pMat->SetDepthTest(bDepthTest);
		pMat->SetPhysicsMaterial(sPhysicsMatName);
        if(pMatType->IsTranslucent())
			pMat->SetBlendMode(GetBlendMode(sBlendMode));

		///////////////////////////
		//Textures
		tinyxml2::XMLElement* pTexRoot = pDoc->FirstChildElement("TextureUnits");
		if(pTexRoot==NULL){
			Error("TextureUnits child not found.\n");
			return NULL;
		}

		//Log("Material %s\n",asName.c_str());
		
		// Per-material sampler state (applied uniformly to all bindings).
		// Wrap is taken from the first texture entry that specifies it; filter and
		// anisotropy come from the cMaterialManager's global setting.
		pMat->setTextureFilter(mTextureFilter);
		pMat->SetTextureAnisotropy(mfTextureAnisotropy);

		for(int i=0; i< pMatType->GetUsedTextureNum(); ++i)
		{
			cMaterialUsedTexture* pUsedTexture = pMatType->GetUsedTexture(i);
			SharedResourceHandle<Image> pImage;

			tString sTextureType = GetTextureString(pUsedTexture->mType);

			tinyxml2::XMLElement* pTexChild = pTexRoot->FirstChildElement(sTextureType.c_str());
			if(pTexChild==NULL){
				continue;
			}

			eTextureType type = GetType(GetAttributeString(pTexChild, "Type", ""));
			tString sFile = GetAttributeString(pTexChild, "File", "");
			bool bMipMaps = GetAttributeBool(pTexChild, "MipMaps", true);
			bool bCompress = GetAttributeBool(pTexChild, "Compress", false);
			eTextureWrap wrap = GetWrap(GetAttributeString(pTexChild, "Wrap", ""));

			eTextureAnimMode animMode = GetAnimMode(GetAttributeString(pTexChild, "AnimMode", "None"));
			float fFrameTime = GetAttributeFloat(pTexChild, "AnimFrameTime", 1.0f);

			if(sFile=="") continue;

			if(cString::GetFilePath(sFile).length() <= 1)
			{
				sFile = cString::SetFilePath(sFile, cString::To8Char(cString::GetFilePathW(asPath)));
			}

			// Diffuse and Illumination are authored as perceptual color in sRGB;
			// every other slot (normals, packed specular, height, alpha, dissolve,
			// cubemap alpha) carries linear data and must keep a UNORM view.
			// Decals included: they blend into the LINEAR albedo buffer before
			// lighting, so their diffuse must be sRGB-decoded to linear like any
			// other albedo.
			const bool bSRGB = (pUsedTexture->mType == eMaterialTexture_Diffuse)
							|| (pUsedTexture->mType == eMaterialTexture_Illumination);

			if(animMode != eTextureAnimMode_None)
			{
				pImage = mpResources->GetTextureManager()->CreateAnimImage(sFile,bMipMaps,type,eTextureUsage_Normal,mlTextureSizeDownScaleLevel, bSRGB, animMode, fFrameTime);
			}
			else
			{
				if(type == eTextureType_1D)
				{
					pImage = mpResources->GetTextureManager()->Create1DImage(sFile,bMipMaps,
																				eTextureUsage_Normal,
																				mlTextureSizeDownScaleLevel, bSRGB);
				}
				else if(type == eTextureType_2D)
				{
					pImage = mpResources->GetTextureManager()->Create2DImage(sFile,bMipMaps, eTextureType_2D,
																			eTextureUsage_Normal,
																			mlTextureSizeDownScaleLevel, bSRGB);
				}
				else if(type == eTextureType_3D)
				{
					pImage = mpResources->GetTextureManager()->Create3DImage(sFile,bMipMaps,
																			eTextureUsage_Normal,
																			mlTextureSizeDownScaleLevel, bSRGB);
				}
				else if(type == eTextureType_CubeMap)
				{
					pImage = mpResources->GetTextureManager()->CreateCubeMapImage(sFile,bMipMaps,
																					eTextureUsage_Normal,
																					mlTextureSizeDownScaleLevel, bSRGB);
				}
			}

			if(!pImage)
			{
				hplDelete(pMat);
				return NULL;
			}


			// Wrap is per-material in the new model; record the last texture entry's wrap.
			pMat->setTextureWrap(wrap);

			// Manager-loaded materials adopt the freshly-created texture reference
			// (mbAutoDestroyTextures stays true), so SetImage takes ownership.
			pMat->SetImage(pUsedTexture->mType, pImage.Release());
		}

		///////////////////////////
		//Animations
		tinyxml2::XMLElement* pUvAnimRoot = pDoc->FirstChildElement("UvAnimations");
		if(pUvAnimRoot)
		{
			for(tinyxml2::XMLElement* pAnimElem = pUvAnimRoot->FirstChildElement(); pAnimElem != NULL; pAnimElem = pAnimElem->NextSiblingElement())
			{
				eMaterialUvAnimation animType = GetUvAnimType(GetAttributeString(pAnimElem, "Type").c_str());
				eMaterialAnimationAxis animAxis = GetAnimAxis(GetAttributeString(pAnimElem, "Axis").c_str());
				float fSpeed = GetAttributeFloat(pAnimElem, "Speed",0);
				float fAmp = GetAttributeFloat(pAnimElem, "Amplitude",0);

				pMat->AddUvAnimation(animType,fSpeed,fAmp, animAxis);
			}
		}


		///////////////////////////
		//Variables
		tinyxml2::XMLElement* pUserVarsRoot = pDoc->FirstChildElement("SpecificVariables");
		cResourceVarsObject userVars;
		if(pUserVarsRoot) userVars.LoadVariables(pUserVarsRoot);

		pMatType->LoadVariables(pMat, &userVars);


		///////////////////////////
		//End

		return pMat;
	}

	//-----------------------------------------------------------------------

	eTextureType cMaterialManager::GetType(const tString& asType)
	{
		if(cString::ToLowerCase(asType) == "cube") return eTextureType_CubeMap;
		else if(cString::ToLowerCase(asType) == "1d") return eTextureType_1D;
		else if(cString::ToLowerCase(asType) == "2d") return eTextureType_2D;
		else if(cString::ToLowerCase(asType) == "3d") return eTextureType_3D;

		return eTextureType_2D;
	}
	//-----------------------------------------------------------------------
	
	tString cMaterialManager::GetTextureString(eMaterialTexture aType)
	{
		switch(aType)
		{
			case eMaterialTexture_Diffuse: return "Diffuse";
			case eMaterialTexture_Alpha: return "Alpha";
			case eMaterialTexture_NMap: return "NMap";
			case eMaterialTexture_Height: return "Height";
			case eMaterialTexture_Illumination: return "Illumination";
			case eMaterialTexture_Specular: return "Specular";
			case eMaterialTexture_CubeMap: return "CubeMap";
			case eMaterialTexture_DissolveAlpha: return "DissolveAlpha";
			case eMaterialTexture_CubeMapAlpha: return "CubeMapAlpha";
		}

		return "";
	}
	
	//-----------------------------------------------------------------------

	eTextureWrap cMaterialManager::GetWrap(const tString& asType)
	{
		if(cString::ToLowerCase(asType) == "repeat") return eTextureWrap_Repeat;
		else if(cString::ToLowerCase(asType) == "clamp") return eTextureWrap_Clamp;
		else if(cString::ToLowerCase(asType) == "clamptoedge") return eTextureWrap_ClampToEdge;

		return eTextureWrap_Repeat;
	}

	eTextureAnimMode cMaterialManager::GetAnimMode(const tString& asType)
	{
		if(cString::ToLowerCase(asType) == "none") return eTextureAnimMode_None;
		else if(cString::ToLowerCase(asType) == "loop") return eTextureAnimMode_Loop;
		else if(cString::ToLowerCase(asType) == "oscillate") return eTextureAnimMode_Oscillate;

		return eTextureAnimMode_None;
	}

	//-----------------------------------------------------------------------

	eMaterialBlendMode cMaterialManager::GetBlendMode(const tString& asType)
	{
		tString sLow = cString::ToLowerCase(asType);
		if(sLow == "add")	return eMaterialBlendMode_Add;
		if(sLow == "mul")	return eMaterialBlendMode_Mul;
		if(sLow == "mulx2")	return eMaterialBlendMode_MulX2;
		if(sLow == "alpha")	return eMaterialBlendMode_Alpha;
		if(sLow == "premulalpha")	return eMaterialBlendMode_PremulAlpha;

		Warning("Material BlendMode '%s' does not exist!\n",asType.c_str());

		return eMaterialBlendMode_Add;
	}

	//-----------------------------------------------------------------------
	
	eMaterialUvAnimation cMaterialManager::GetUvAnimType(const char* apString)
	{
		if(apString==NULL){ 
			Error("Uv animation attribute Type does not exist!\n");
			return eMaterialUvAnimation_LastEnum;
		}
		
		tString sLow = cString::ToLowerCase(apString);
		
		if(sLow == "translate") return eMaterialUvAnimation_Translate;
		if(sLow == "sin") return eMaterialUvAnimation_Sin;
		if(sLow == "rotate") return eMaterialUvAnimation_Rotate;
		
		Error("Invalid uv animation type %s\n",apString);
		return eMaterialUvAnimation_LastEnum;
	}
	
	eMaterialAnimationAxis cMaterialManager::GetAnimAxis(const char* apString)
	{
		if(apString==NULL){ 
			Error("Uv animation attribute Axis does not exist!\n");
			return eMaterialAnimationAxis_LastEnum;
		}

		tString sLow = cString::ToLowerCase(apString);

		if(sLow == "x") return eMaterialAnimationAxis_X;
		if(sLow == "y") return eMaterialAnimationAxis_Y;
		if(sLow == "z") return eMaterialAnimationAxis_Z;

		Error("Invalid animation axis %s\n",apString);
		return eMaterialAnimationAxis_LastEnum;
	}

	//-----------------------------------------------------------------------
}
