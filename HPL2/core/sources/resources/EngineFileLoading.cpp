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

#include "resources/EngineFileLoading.h"
#include "graphics/Image.h"

#include <tinyxml2.h>
#include "resources/XmlHelper.h"
#include "resources/Resources.h"
#include "resources/TextureManager.h"
#include "resources/MaterialManager.h"

#include "math/Math.h"

#include "system/String.h"

#include "scene/World.h"
#include "scene/LightPoint.h"
#include "scene/LightSpot.h"
#include "scene/LightArea.h"
#include "scene/LightBox.h"
#include "scene/LightParameters.h"
#include "scene/MeshEntity.h"
#include "scene/SoundEntity.h"
#include "scene/ParticleEmitter.h"
#include "scene/ParticleSystem.h"
#include "scene/BillBoard.h"
#include "scene/Beam.h"
#include "scene/GuiSetEntity.h"
#include "scene/RopeEntity.h"
#include "scene/FogArea.h"

#include "graphics/Graphics.h"
#include "graphics/VertexBuffer.h"
#include "graphics/Mesh.h"
#include "graphics/SubMesh.h"


namespace hpl {

	//////////////////////////////////////////////////////////////////////////
	// DEFINES
	//////////////////////////////////////////////////////////////////////////
	
	//-----------------------------------------------------------------------

	#define kBeginWorldEntityLoad()		\
		tString sName = GetAttributeString(apElement, "Name");
	
	#define kEndWorldEntityLoad(pEntity)		\
		SetupWorldEntity(pEntity, apElement);	\
		return pEntity;

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// CREATE ENTITIES
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	unsigned cEngineFileLoading::GetElementRendererMask(tinyxml2::XMLElement* apElement)
	{
		const cLightElementInfo lightInfo = GetLightElementInfo(apElement->Value());
		unsigned lMask = apElement->Attribute("RendererMask")
			? SanitizeRendererMask(static_cast<unsigned>(GetAttributeInt(apElement, "RendererMask", 0)))
			: (lightInfo.mbValid ? GetDefaultLightRendererMask(lightInfo) : kRendererMaskAll);
		// Overdrive light classes never load on Standard.
		if(lightInfo.mbValid && lightInfo.mbOverdrive) lMask &= kRendererMaskOverdrive;
		return lMask;
	}

	bool cEngineFileLoading::IsElementEnabledForWorld(tinyxml2::XMLElement* apElement, cWorld *apWorld)
	{
		if(cResources::GetRendererMaskFilterEnabled()==false || apWorld==NULL) return true;
		return IsRendererMaskEnabled(GetElementRendererMask(apElement), apWorld->GetRendererMaskBit());
	}

	//-----------------------------------------------------------------------

	cFogArea* cEngineFileLoading::LoadFogArea(tinyxml2::XMLElement* apElement, const tString& asNamePrefix, cWorld *apWorld, bool abStatic)
	{
		kBeginWorldEntityLoad();

		cFogArea *pFog = apWorld->CreateFogArea(asNamePrefix+sName, abStatic);

		if(pFog)
		{
			pFog->SetRendererMask(GetElementRendererMask(apElement));
			pFog->SetColor(GetAttributeColor(apElement, "Color",cColor(1,1)));
			pFog->SetStart(GetAttributeFloat(apElement, "Start", 0));
			pFog->SetEnd(GetAttributeFloat(apElement, "End", 0));
			pFog->SetFalloffExp(GetAttributeFloat(apElement, "FalloffExp", 0));
			pFog->SetShowBacksideWhenInside(GetAttributeBool(apElement, "ShownBacksideWhenInside", true));
			pFog->SetShowBacksideWhenOutside(GetAttributeBool(apElement, "ShownBacksideWhenOutside", true));
		}

		kEndWorldEntityLoad(pFog);
	}

	//-----------------------------------------------------------------------

	cParticleSystem* cEngineFileLoading::LoadParticleSystem(tinyxml2::XMLElement* apElement, const tString& asNamePrefix, cWorld *apWorld)
	{
		kBeginWorldEntityLoad();

		tString sFile = GetAttributeString(apElement, "File");

		cParticleSystem *pPS = apWorld->CreateParticleSystem(asNamePrefix+sName,sFile,1);

		if(pPS)
		{
			pPS->SetColor(GetAttributeColor(apElement, "Color",cColor(1,1)));
			pPS->SetFadeAtDistance(GetAttributeBool(apElement, "FadeAtDistance", false));
			pPS->SetMinFadeDistanceStart(GetAttributeFloat(apElement, "MinFadeDistanceStart"));
			pPS->SetMinFadeDistanceEnd(GetAttributeFloat(apElement, "MinFadeDistanceEnd"));
			pPS->SetMaxFadeDistanceStart(GetAttributeFloat(apElement, "MaxFadeDistanceStart"));
			pPS->SetMaxFadeDistanceEnd(GetAttributeFloat(apElement, "MaxFadeDistanceEnd"));
		}
		
		kEndWorldEntityLoad(pPS);
	}
	
	//-----------------------------------------------------------------------

	cSoundEntity* cEngineFileLoading::LoadSound(tinyxml2::XMLElement* apElement, const tString& asNamePrefix, cWorld *apWorld)
	{
		kBeginWorldEntityLoad();

		tString sSoundFile = GetAttributeString(apElement, "SoundEntityFile");
		bool bUseDefault = GetAttributeBool(apElement, "UseDefault");

		cSoundEntity *pSound = apWorld->CreateSoundEntity(asNamePrefix+sName,sSoundFile,false);
		if(pSound==NULL) return NULL;

		if(bUseDefault==false)
		{
			pSound->SetMinDistance(GetAttributeFloat(apElement, "MinDistance"));
			pSound->SetMaxDistance(GetAttributeFloat(apElement, "MaxDistance"));
			pSound->SetVolume(GetAttributeFloat(apElement, "Volume"));
		}


		kEndWorldEntityLoad(pSound);
	}

	
	//-----------------------------------------------------------------------

	static eBillboardType ToBillboardType(const tString& asType)
	{
		if(asType == "Axis") return eBillboardType_Axis;
		if(asType == "Point") return eBillboardType_Point;
		if(asType == "FixedAxis") return eBillboardType_FixedAxis;

		return eBillboardType_Point;
	}

	cBillboard* cEngineFileLoading::LoadBillboard(tinyxml2::XMLElement* apElement, const tString& asNamePrefix, cWorld *apWorld, cResources *apResources, bool abStatic,
													tEFL_LightBillboardConnectionList *apLightBillboardList)
	{
		kBeginWorldEntityLoad();

		cVector2f vSize = GetAttributeVector2f(apElement, "BillboardSize");
		tString sMat = GetAttributeString(apElement, "MaterialFile");
		eBillboardType bbType = ToBillboardType(GetAttributeString(apElement, "BillboardType"));

		cBillboard *pBillboard = apWorld->CreateBillboard(asNamePrefix+sName,vSize,bbType,sMat, abStatic);
		if(pBillboard==NULL) return NULL;
		pBillboard->SetRendererMask(GetElementRendererMask(apElement));

		pBillboard->SetForwardOffset(GetAttributeFloat(apElement, "BillboardOffset"));
		pBillboard->SetColor(GetAttributeColor(apElement, "BillboardColor",cColor(1,1)));

		pBillboard->SetIsHalo(GetAttributeBool(apElement, "IsHalo",false));
		pBillboard->SetHaloSourceSize(GetAttributeVector3f(apElement, "HaloSourceSize",1));

		tString sConnectLight = GetAttributeString(apElement, "ConnectLight");
		if(apLightBillboardList && sConnectLight!="")
		{
			cEFL_LightBillboardConnection lightBBConnection;
			lightBBConnection.msBillboardID = GetAttributeInt(apElement, "ID");
			lightBBConnection.msLightName = asNamePrefix+sConnectLight;
			apLightBillboardList->push_back(lightBBConnection);
		}

		kEndWorldEntityLoad(pBillboard);
	}

	//-----------------------------------------------------------------------
	
	static eShadowMapResolution ToShadowMapResolution(const tString& asType)
	{
		tString sLowType = cString::ToLowerCase(asType);

        if(sLowType == "high") return eShadowMapResolution_High;
		if(sLowType == "medium") return eShadowMapResolution_Medium;
		if(sLowType == "low") return eShadowMapResolution_Low;
		return eShadowMapResolution_High;
	}

	static eTextureAnimMode ToTextureAnimMode(const tString& asType)
	{
		if(cString::ToLowerCase(asType) == "none") return eTextureAnimMode_None;
		else if(cString::ToLowerCase(asType) == "loop") return eTextureAnimMode_Loop;
		else if(cString::ToLowerCase(asType) == "oscillate") return eTextureAnimMode_Oscillate;

		return eTextureAnimMode_None;
	}
	
	iLight* cEngineFileLoading::LoadLight(	tinyxml2::XMLElement* apElement, const tString& asNamePrefix, cWorld *apWorld, cResources *apResources, bool abStatic)
	{
		kBeginWorldEntityLoad();

		iLight *pLight = NULL;

		const cLightElementInfo info = GetLightElementInfo(apElement->Value());
		if(info.mbValid==false)
		{
			Error("Unknown light type '%s'\n", apElement->Value());
			return NULL;
		}

		// Retail lights with no Overdrive replacement load as the Overdrive class
		// on Overdrive, with values derived from their Radius. The class choice
		// ignores the load filter so editors preview the backend they run.
		const unsigned int lElementMask = GetElementRendererMask(apElement);
		const bool bOverdriveBackend = apWorld->GetRendererBackend() != eRendererBackend_Standard;
		const bool bOverdriveClass = info.mbOverdrive ||
			ShouldPromoteLegacyLight(info, lElementMask, bOverdriveBackend);

		bool bStatic = abStatic;

		//////////////////////////
		// Area Light
		if(info.mShape == eLightElementShape_Area)
		{
			cLightArea *pLightArea = apWorld->CreateLightArea(asNamePrefix+sName, bStatic);
			pLight = pLightArea;

			pLightArea->SetWidth(GetAttributeFloat(apElement, "SourceWidth", 1.0f));
			pLightArea->SetHeight(GetAttributeFloat(apElement, "SourceHeight", 1.0f));
			pLightArea->SetBarnDoorAngle(GetAttributeFloat(apElement, "BarnDoorAngle", cMath::ToRad(45.0f)));
			pLightArea->SetBarnDoorLength(GetAttributeFloat(apElement, "BarnDoorLength", 0.0f));

			//Optional source texture (stored in the base gobo slot — a 2D image). Tints emission.
			tString sSourceTex = GetAttributeString(apElement, "SourceTexture");
			if(sSourceTex != "")
			{
				Image *pTex = apResources->GetTextureManager()->Create2DImage(sSourceTex,true).Release();
				if(pTex) pLightArea->SetGoboTexture(pTex);
			}
		}
		//////////////////////////
		// Box Light
		else if(info.mShape == eLightElementShape_Box)
		{
			cLightBoxLegacy *pLightBox = apWorld->CreateLightBoxLegacy(asNamePrefix+sName, bStatic);
			pLight = pLightBox;

			pLightBox->SetSize(GetAttributeVector3f(apElement, "Size", cVector3f(1,1,1)));
			pLightBox->SetBlendFunc((eLightBoxBlendFunc)GetAttributeInt(apElement, "BlendFunc", (int)eLightBoxBlendFunc_Add));
		}
		//////////////////////////
		// Spotlightt
		else if(info.mShape == eLightElementShape_Spot)
		{
			iLightSpot *pLightSpot = bOverdriveClass
				? static_cast<iLightSpot*>(apWorld->CreateLightSpot(asNamePrefix+sName,"", bStatic))
				: static_cast<iLightSpot*>(apWorld->CreateLightSpotLegacy(asNamePrefix+sName,"", bStatic));
			pLight = pLightSpot;

			//Frustum related
			pLightSpot->SetFOV(GetAttributeFloat(apElement, "FOV", 1.0f));
			pLightSpot->SetAspect(GetAttributeFloat(apElement, "Aspect", 1.0f));
			pLightSpot->SetNearClipPlane(GetAttributeFloat(apElement, "NearClipPlane", 0.1f));

			//Spot fall off
			tString sSpotFalloffMap = GetAttributeString(apElement, "SpotFalloffMap");
			if(sSpotFalloffMap != "")
			{
				Image *pFalloff = apResources->GetTextureManager()->Create1DImage(sSpotFalloffMap,true).Release();
				if(pFalloff) pLightSpot->SetSpotFalloffMap(pFalloff);
			}
		}
		//////////////////////////
		// Point Light
		else
		{
			if(bOverdriveClass)
				pLight = apWorld->CreateLightPoint(asNamePrefix+sName,"", bStatic);
			else
				pLight = apWorld->CreateLightPointLegacy(asNamePrefix+sName,"", bStatic);
		}

		//////////////////////////
		// General properties
		eLightType lightType = pLight->GetLightType();
		const bool bSpotLight = lightType == eLightType_Spot;

		//Spot and point
		if(lightType == eLightType_Point || bSpotLight)
		{
			//Falloff
			tString sFalloffMap = GetAttributeString(apElement, "FalloffMap");
			if(sFalloffMap != "")
			{
				Image *pFalloff = apResources->GetTextureManager()->Create1DImage(sFalloffMap,true).Release();
				if(pFalloff) pLight->SetFalloffMap(pFalloff);
			}

			//Gobo
			tString sGobo = GetAttributeString(apElement, "Gobo","");
			if(sGobo  != "")
			{
				eTextureAnimMode animMode = ToTextureAnimMode(GetAttributeString(apElement, "GoboAnimMode",""));
				float fAnimFrameTime = GetAttributeFloat(apElement, "GoboAnimFrameTime", 1);

				Image *pGoboTex=NULL;
				if(bSpotLight)
				{
					if(animMode == eTextureAnimMode_None)
						pGoboTex = apResources->GetTextureManager()->Create2DImage(sGobo,true).Release();
					else
						pGoboTex = apResources->GetTextureManager()->CreateAnimImage(sGobo, true, eTextureType_2D,
								eTextureUsage_Normal,0,false,animMode,fAnimFrameTime).Release();
				}
				else
				{
					if(animMode == eTextureAnimMode_None)
						pGoboTex = apResources->GetTextureManager()->CreateCubeMapImage(sGobo,true).Release();
					else
						pGoboTex = apResources->GetTextureManager()->CreateAnimImage(sGobo,true, eTextureType_CubeMap,
								eTextureUsage_Normal,0,false,animMode,fAnimFrameTime).Release();
				}

				if(pGoboTex)
				{
					pLight->SetGoboTexture(pGoboTex);
				}
			}
		}

		//All types
		pLight->SetCastShadows(GetAttributeBool(apElement, "CastShadows", false));
		pLight->SetDiffuseColor(GetAttributeColor(apElement, "DiffuseColor", cColor(1)));
		pLight->SetDefaultDiffuseColor(pLight->GetDiffuseColor());
		const cColor diffuseColor = pLight->GetDiffuseColor();
		const bool bHasRendererMask = apElement->Attribute("RendererMask") != NULL;
		const unsigned int lAuthoredMask = static_cast<unsigned int>(GetAttributeInt(apElement, "RendererMask", 0));
		float fFlickerOffValue = 0.0f;
		if(info.mbOverdrive)
		{
			cOverdriveLightInput input;
			input.mbHasIntensity = apElement->Attribute("Intensity") != NULL;
			input.mfIntensity = GetAttributeFloat(apElement, "Intensity", 1.0f);
			input.mbHasRadius = apElement->Attribute("Radius") != NULL;
			input.mfRadius = GetAttributeFloat(apElement, "Radius", 0.0f);
			input.mbHasSourceRadius = apElement->Attribute("SourceRadius") != NULL;
			input.mfSourceRadius = GetAttributeFloat(apElement, "SourceRadius", 0.0f);
			input.mbHasFlickerOffIntensity = apElement->Attribute("FlickerOffIntensity") != NULL;
			input.mfFlickerOffIntensity = GetAttributeFloat(apElement, "FlickerOffIntensity", 0.0f);
			input.mfRed = diffuseColor.r;
			input.mfGreen = diffuseColor.g;
			input.mfBlue = diffuseColor.b;
			input.mbHasRendererMask = bHasRendererMask;
			input.mlRendererMask = lAuthoredMask;
			const cOverdriveLightParameters params = ResolveOverdriveLightParameters(input);
			if(params.mbStrippedStandardBit)
				Warning("Overdrive light '%s' sets the Standard renderer bit; it only loads on Overdrive\n", sName.c_str());
			pLight->SetIntensity(params.mfIntensity);
			pLight->SetRadius(params.mfRadius);
			pLight->SetSourceRadius(params.mfSourceRadius);
			pLight->SetReachFollowsIntensity(input.mbHasRadius==false);
			pLight->SetRendererMask(params.mlRendererMask);
			fFlickerOffValue = params.mfFlickerOffIntensity;
		}
		else
		{
			cLegacyLightInput input;
			input.mbHasRadius = apElement->Attribute("Radius") != NULL;
			input.mfRadius = GetAttributeFloat(apElement, "Radius", 1.0f);
			input.mbHasFlickerOffRadius = apElement->Attribute("FlickerOffRadius") != NULL;
			input.mfFlickerOffRadius = GetAttributeFloat(apElement, "FlickerOffRadius", 0.0f);
			input.mbHasRendererMask = bHasRendererMask;
			input.mlRendererMask = lAuthoredMask;
			const cLegacyLightParameters legacy = ResolveLegacyLightParameters(input);
			if(bOverdriveClass)
			{
				const cOverdriveLightParameters promoted =
					PromoteLegacyLightParameters(legacy, diffuseColor.r, diffuseColor.g, diffuseColor.b);
				pLight->SetIntensity(promoted.mfIntensity);
				pLight->SetRadius(promoted.mfRadius);
				pLight->SetSourceRadius(promoted.mfSourceRadius);
				// Scripts fade retail lights by radius; the promoted reach follows.
				pLight->SetReachFollowsIntensity(true);
				pLight->SetRendererMask(promoted.mlRendererMask);
				fFlickerOffValue = promoted.mfFlickerOffIntensity;
			}
			else
			{
				pLight->SetRadius(legacy.mfRadius);
				pLight->SetRendererMask(legacy.mlRendererMask);
				fFlickerOffValue = legacy.mfFlickerOffRadius;
			}
		}

		pLight->SetShadowMapResolution( ToShadowMapResolution(GetAttributeString(apElement, "ShadowResolution", "High")) );

		bool bShadowsAffectDynamic = GetAttributeBool(apElement, "ShadowsAffectDynamic", true);
		bool bShadowsAffectStatic = GetAttributeBool(apElement, "ShadowsAffectStatic", true);
		tObjectVariabilityFlag lFlags =0;
		if(bShadowsAffectDynamic)	lFlags |= eObjectVariabilityFlag_Dynamic;
		if(bShadowsAffectStatic)	lFlags |= eObjectVariabilityFlag_Static;
		pLight->SetShadowCastersAffected(lFlags);

		//////////////////////
		// Backwards compitabilty:
		float fDefaultFadeOn = GetAttributeFloat(apElement, "FlickerOnFadeLength",0);
		float fDefaultFadeOff = GetAttributeFloat(apElement, "FlickerOffFadeLength",0);

		pLight->SetFlickerActive(GetAttributeBool(apElement, "FlickerActive", false));
		pLight->SetFlicker(
			GetAttributeColor(apElement, "FlickerOffColor"),
			fFlickerOffValue,

			GetAttributeFloat(apElement, "FlickerOnMinLength"),
			GetAttributeFloat(apElement, "FlickerOnMaxLength"),
			GetAttributeString(apElement, "FlickerOnSound"),
			GetAttributeString(apElement, "FlickerOnPS"),

			GetAttributeFloat(apElement, "FlickerOffMaxLength"),
			GetAttributeFloat(apElement, "FlickerOffMinLength"),
			GetAttributeString(apElement, "FlickerOffSound"),
			GetAttributeString(apElement, "FlickerOffPS"),

			GetAttributeBool(apElement, "FlickerFade"),
			GetAttributeFloat(apElement, "FlickerOnFadeMinLength", fDefaultFadeOn),
			GetAttributeFloat(apElement, "FlickerOnFadeMaxLength", fDefaultFadeOn),

			GetAttributeFloat(apElement, "FlickerOffFadeMinLength", fDefaultFadeOff),
			GetAttributeFloat(apElement, "FlickerOffFadeMaxLength", fDefaultFadeOff)
			);
 

		kEndWorldEntityLoad(pLight);
	}
	

	//-----------------------------------------------------------------------

	int glDecalNumOfElements[4] = {4,3,3,4};
	eVertexBufferElement glDecalElementType[4] = {	eVertexBufferElement_Position, 
													eVertexBufferElement_Normal,
													eVertexBufferElement_Texture0,
													eVertexBufferElement_Texture1Tangent};
	cMesh* cEngineFileLoading::LoadDecalMeshHelper(tinyxml2::XMLElement* apElement, cGraphics* apGraphics, cResources* apResources, const tString& asName, const tString& asMaterial, const cColor& aColor)
	{
		////////////////////////////////
		//Load Vertex data
		if(apElement==NULL)return NULL;

		int lNumOfVtx = GetAttributeInt(apElement, "NumVerts", 0);
		int lNumOfIdx = GetAttributeInt(apElement, "NumInds", 0);

		if(lNumOfIdx <=0 || lNumOfVtx<=0)
		{
			Warning("Decal %s is missing geometry, skipping!\n", asName.c_str());
			return NULL;
		}

		tinyxml2::XMLElement *pDataArrayElem[4];
		pDataArrayElem[0] = apElement->FirstChildElement("Positions");
		pDataArrayElem[1] = apElement->FirstChildElement("Normals");
		pDataArrayElem[2] = apElement->FirstChildElement("TexCoords");
		pDataArrayElem[3] = apElement->FirstChildElement("Tangents");
		tinyxml2::XMLElement *pIndicesElem = apElement->FirstChildElement("Indices");

		tFloatVec vDataArrays[4];
		tIntVec vIdxArray;
		tString sSepp=" ";
		for(int i=0; i<4; ++i)
		{
			vDataArrays->reserve(lNumOfVtx * glDecalNumOfElements[i]);
			cString::GetFloatVec(GetAttributeString(pDataArrayElem[i], "Array"), vDataArrays[i],&sSepp);
		}
		vIdxArray.reserve(lNumOfIdx);
		cString::GetIntVec(GetAttributeString(pIndicesElem, "Array"), vIdxArray,&sSepp);
	
		//////////////////////////////////
		// Create vertex buffer
		cVertexBuffer *pVtxBuffer = new cVertexBuffer(eVertexBufferType_Software, eVertexBufferDrawType_Tri, 
																					eVertexBufferUsageType_Static,lNumOfVtx, lNumOfIdx);

		//Create arrays	
		for(int i=0; i<4; ++i)
			pVtxBuffer->CreateElementArray(glDecalElementType[i],eVertexBufferElementFormat_Float, glDecalNumOfElements[i]);
		pVtxBuffer->CreateElementArray(eVertexBufferElement_Color0,eVertexBufferElementFormat_Float,4);
		
		//Copy the data!
		// TODO: This needs to be made faster so that data is loaded directly into mesh!
		for(int vtx=0; vtx<lNumOfVtx; ++vtx)
		{
			for(int i=0; i<4; ++i)
			{
				float *pData = &vDataArrays[i][vtx*glDecalNumOfElements[i]];

				if(glDecalNumOfElements[i]==2)
					pVtxBuffer->AddVertexVec3f(glDecalElementType[i], cVector3f(pData[0],pData[1],0) );
				else if(glDecalNumOfElements[i]==3)
					pVtxBuffer->AddVertexVec3f(glDecalElementType[i], cVector3f(pData[0],pData[1],pData[2]) );
				else if(glDecalNumOfElements[i]==4)
					pVtxBuffer->AddVertexVec4f(glDecalElementType[i], cVector3f(pData[0],pData[1],pData[2]),pData[3]);
			}

			pVtxBuffer->AddVertexColor(eVertexBufferElement_Color0, aColor);
		}

		for(int i=0; i<lNumOfIdx; ++i)
			pVtxBuffer->AddIndex(vIdxArray[i]);

		//Compile
		pVtxBuffer->Compile(0);
		
		/////////////////////////
		// Create the mesh
		cMesh *pMesh = hplNew( cMesh, (asName, _W(""), apResources->GetMaterialManager(), apResources->GetAnimationManager()) );
		pMesh->AddReference(); // hand-built mesh: take the one owning reference the entity drops

		cSubMesh *pSubMesh = pMesh->CreateSubMesh("Main");
		
		pSubMesh->SetMaterial(apResources->GetMaterialManager()->CreateMaterial(asMaterial));
		pSubMesh->SetVertexBuffer(pVtxBuffer);
		pSubMesh->SetMaterialName(asMaterial);


		return pMesh;
	}

	//////////////////////////////////////////////////////////////////////////
	// PRIVATE METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	void cEngineFileLoading::SetupWorldEntity(iEntity3D *apEntity, tinyxml2::XMLElement* apElement)
	{
		if(apEntity==NULL) return;

		int lID = GetAttributeInt(apElement, "ID");
		cVector3f vPosition = GetAttributeVector3f(apElement, "WorldPos",0);
		cVector3f vScale = GetAttributeVector3f(apElement, "Scale",1);
		cVector3f vRotation = GetAttributeVector3f(apElement, "Rotation",0);

		cMatrixf mtxTransform = cMath::MatrixMul(cMath::MatrixRotate(vRotation, eEulerRotationOrder_XYZ),cMath::MatrixScale(vScale));
		mtxTransform.SetTranslation(vPosition);

		apEntity->SetMatrix(mtxTransform);
		apEntity->SetUniqueID(lID);
	}

    //-----------------------------------------------------------------------
}
