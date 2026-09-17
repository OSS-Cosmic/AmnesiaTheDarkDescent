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

#include "EditorWindowMaterialEditor.h"
#include "EditorWorld.h"

#include "EditorInput.h"
#include "EditorVar.h"
#include "EditorHelper.h"

#include "resources/XmlHelper.h"
#include <tinyxml2.h>

#include <algorithm>

//------------------------------------------------------------------------------------

static tString gsTextureTypeStrings[] = 
{
	"1D",
	"2D",
	"Rect",
	"Cube",
	"3D"
};

static tString gsWrapTypeStrings[] =
{
	"Repeat",
	"Clamp",
	"ClampToEdge",
	"ClampToBorder"
};

static tString gsAnimModeStrings[] =
{
	"None",
	"Loop",
	"Oscillate",
};

static tString gsBlendModeStrings[] = 
{
	"None",
	"Add",
	"Mul",
	"Mulx2",
	"Alpha",
	"PremulAlpha",

	""
};

//-------------------------------------------------------------------

cEditorClassMaterial::cEditorClassMaterial(cEditorWindowMaterialEditor* apWindow)
{
	mpWindow = apWindow;
}

//-------------------------------------------------------------------

bool cEditorClassMaterial::Create(void *apData)
{
	iMaterialType* pMat = (iMaterialType*)apData;
	bool bSuccessfullyCreated = true;

	cEngine* pEng = mpWindow->GetEditor()->GetEngine();
	cResources* pRes = pEng->GetResources();
	tinyxml2::XMLDocument xmlDoc;
	tinyxml2::XMLElement* pTempDoc = xmlDoc.NewElement("");
	xmlDoc.InsertEndChild(pTempDoc);

	for(int i=0;i<pMat->GetUserVariableNum();++i)
	{
		cMaterialUserVariable* pMatVar = pMat->GetUserVariable(i);
		tinyxml2::XMLElement* pVarData = pTempDoc->GetDocument()->NewElement("Var");
		pTempDoc->InsertEndChild(pVarData);

		hpl::SetAttributeString(pVarData, "Name", pMatVar->msName);
		hpl::SetAttributeString(pVarData, "Type", pEng->GetEngineTypeString(pMatVar->mType));
		hpl::SetAttributeString(pVarData, "DefaultValue", pMatVar->msValue);
		hpl::SetAttributeString(pVarData, "Description", pMatVar->msDescription);

		if(pMatVar->mvEnumValues.empty()==false)
		{
            tinyxml2::XMLElement* pEnumValues = pVarData->GetDocument()->NewElement("EnumValues");
            pVarData->InsertEndChild(pEnumValues);
			for(int j=0;j<(int)pMatVar->mvEnumValues.size();++j)
			{
				tinyxml2::XMLElement* pValue = pEnumValues->GetDocument()->NewElement("Value");
				pEnumValues->InsertEndChild(pValue);
				hpl::SetAttributeString(pValue, "Name", pMatVar->mvEnumValues[j]);
			}
		}

		iEditorVar* pVar = CreateVariableFromElement(pVarData);
		if(pVar==NULL)
		{
			bSuccessfullyCreated = false;
			break;
		}

		mvVariables.push_back(pVar);
	}

	return bSuccessfullyCreated;
}
//-------------------------------------------------------------------

cEditorClassInstance* cEditorClassMaterial::CreateInstance()
{
	cEditorClassInstance* pInstance = hplNew(cEditorClassInstance,(this));
	DumpVarsOnInstance(mvVariables, pInstance);

	return pInstance;
}

//-------------------------------------------------------------------
//-------------------------------------------------------------------



//-------------------------------------------------------------------
//-------------------------------------------------------------------
//-------------------------------------------------------------------


//------------------------------------------------------------------------------------

cTextureWrapper::cTextureWrapper(cMaterialWrapper* apMat, eMaterialTexture aUnit)
{
	mpMat = apMat;
	mUnit = aUnit;

	mpTexture = NULL;

	mTimeStamp.hours=0;
	mTimeStamp.minutes=0;
	mTimeStamp.seconds=0;
	mTimeStamp.month_day=0;
	mTimeStamp.year=0;
	mTimeStamp.year_day=0;
	mTimeStamp.week_day=0;
	mTimeStamp.month=0;

	Reset();
}

//------------------------------------------------------------------------------------

cTextureWrapper::~cTextureWrapper()
{
	if(mpTexture)
		mpMat->mpMatEditor->GetEditor()->GetEngine()->GetResources()->GetTextureManager()->Destroy(mpTexture);
}

//------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------

void cTextureWrapper::Reset()
{
	mbValid = false;
	mbEnabled = false;
	mbNeedsReload = true;
	mbUpdated = true;

	mbMipMaps=true;
	mbCompressed=false;

	msFile=_W("");
	msWrap="Repeat";
	msAnimMode = "None";
	mfFrameTime = 0;
	mType = eTextureType_2D;
}

//------------------------------------------------------------------------------------

void cTextureWrapper::Load(tinyxml2::XMLElement* apElement)
{
    mbMipMaps = hpl::GetAttributeBool(apElement, "MipMaps");
	mbCompressed = hpl::GetAttributeBool(apElement, "Compress");
	msFile = cString::To16Char(hpl::GetAttributeString(apElement, "File"));
	msWrap = hpl::GetAttributeString(apElement, "Wrap", "Repeat");
	msAnimMode = hpl::GetAttributeString(apElement, "AnimMode", "None");
	mfFrameTime = hpl::GetAttributeFloat(apElement, "AnimFrameTime");
}

//------------------------------------------------------------------------------------

void cTextureWrapper::Save(tinyxml2::XMLElement* apElement)
{
	cMaterialManager* pManMgr = mpMat->mpMatEditor->GetEditor()->GetEngine()->GetResources()->GetMaterialManager();
	apElement->SetValue(pManMgr->GetTextureString(mUnit).c_str());

	hpl::SetAttributeBool(apElement, "MipMaps", mbMipMaps);
	hpl::SetAttributeBool(apElement, "Compress", mbCompressed);
	hpl::SetAttributeString(apElement, "File", cString::To8Char(msFile));
	hpl::SetAttributeString(apElement, "Type", msType);
	hpl::SetAttributeString(apElement, "Wrap", msWrap);
	hpl::SetAttributeString(apElement, "AnimMode", msAnimMode);
	hpl::SetAttributeFloat(apElement, "AnimFrameTime", mfFrameTime);
}

//------------------------------------------------------------------------------------

void cTextureWrapper::Reload()
{
	if(mbNeedsReload)
	{
		mbValid = false;

		if(msFile==_W(""))
			return;

		cEngine* pEng = mpMat->mpMatEditor->GetEditor()->GetEngine();
		cResources* pRes = pEng->GetResources();
		cTextureManager* pTexMgr = pRes->GetTextureManager();
		cFileSearcher* pSearcher = pRes->GetFileSearcher();
		cBitmapLoaderHandler* pLoader = pRes->GetBitmapLoaderHandler();

		tWString sFullPath = pSearcher->GetFilePath(cString::To8Char(msFile));
		Log("Loading texture %s...", cString::To8Char(sFullPath).c_str());
		// The bitmap is loaded only to detect the texture type (1D/2D/Rect/
		// 3D/CubeMap) — the Image itself comes from the texture manager below.
		cBitmap* pBmp = pLoader->LoadBitmap(sFullPath, 0);
		if(pBmp==NULL)
		{
			Log("failed\n");
			return;
		}

		mTimeStamp = cPlatform::FileModifiedDate(sFullPath);
		eTextureType type = GetTextureTypeFromBitmap(pBmp);
		hplDelete(pBmp);

		// Drop the previous image first so the manager's refcount hits zero
		// and the resource is evicted — a re-Create then reloads from disk,
		// preserving the file-watch reload (CheckFileIsUpdated).
		if(mpTexture)
		{
			pTexMgr->Destroy(mpTexture);
			mpTexture = NULL;
		}

		tString sName = cString::To8Char(msFile);
		switch(type)
		{
		case eTextureType_1D:
			mpTexture = pTexMgr->Create1DImage(sName, mbMipMaps).Release();
			break;
		case eTextureType_2D:
			mpTexture = pTexMgr->Create2DImage(sName, mbMipMaps).Release();
			break;
		case eTextureType_Rect:
			mpTexture = pTexMgr->Create2DImage(sName, mbMipMaps, eTextureType_Rect).Release();
			break;
		case eTextureType_3D:
			mpTexture = pTexMgr->Create3DImage(sName, mbMipMaps).Release();
			break;
		case eTextureType_CubeMap:
			mpTexture = pTexMgr->CreateCubeMapImage(sName, mbMipMaps).Release();
			break;
		default:
			break;
		}

		mbValid = (mpTexture!=NULL);
		if(mbValid)
		{
			Log("success\n");
			mbNeedsReload = false;
			mType = type;
			msType = gsTextureTypeStrings[type];
		}
		else
		{
			Log("failed\n");
		}
	}
}

//------------------------------------------------------------------------------------

void cTextureWrapper::Update()
{
	cEngine* pEng = mpMat->mpMatEditor->GetEditor()->GetEngine();
	cGraphics* pGfx = pEng->GetGraphics();
	cResources* pRes = pEng->GetResources();
	cMaterialManager* pMatMgr = pRes->GetMaterialManager();

	if(mbUpdated)
	{
		mpMat->mbPreviewUpdated = true;

		Reload();
		if(mbValid==false)
			return;

		// Per-texture wrap state is gone in the Image/sampler model — msWrap
		// stays as the serialized .mat string only.
		mpTexture->SetAnimMode(pMatMgr->GetAnimMode(msAnimMode));
		mpTexture->SetFrameTime(mfFrameTime);

		mbUpdated = false;
	}
}

//------------------------------------------------------------------------------------

void cTextureWrapper::CreateFromImage(Image* apImage)
{
	Reset();
	SetNeedsReload();
	SetUpdated();

	if(apImage==NULL)
		return;

	mbEnabled = true;

	cTexture* pTex = apImage->GetTexture();
	const RIFormatProps* pProps = pTex ? GetRIFormatProps(pTex->format) : NULL;
	mbCompressed = pProps && pProps->isCompressed;
	mbMipMaps = pTex && pTex->mipNum > 1;
	msFile = apImage->GetFullPath();
	// Per-texture wrap state is gone in the Image/sampler model — keep the
	// Reset() default ("Repeat"); the .mat string is the source of truth.
	msAnimMode = gsAnimModeStrings[apImage->GetAnimMode()];
	mfFrameTime = apImage->GetFrameTime();
}

//------------------------------------------------------------------------------------

bool cTextureWrapper::CheckFileIsUpdated()
{
	if(msFile==_W(""))
		return false;

	tWString sFullPath = this->mpMat->mpMatEditor->GetEditor()->GetEngine()->GetResources()->GetFileSearcher()->GetFilePath(cString::To8Char(msFile));
	cDate newTimeStamp = cPlatform::FileModifiedDate(sFullPath);
	bool bUpdated = (mTimeStamp!=newTimeStamp);

	if(bUpdated)
	{
		SetNeedsReload();
		SetUpdated();
	}

	return bUpdated;
}

//------------------------------------------------------------------------------------

eTextureType cTextureWrapper::GetTextureTypeFromBitmap(cBitmap* apBmp)
{
	if(apBmp->GetNumOfImages()==6)
		return eTextureType_CubeMap;

	const cVector3l& vSize = apBmp->GetSize();
	if(vSize.z==1)
	{
		if(vSize.y==1)
			return eTextureType_1D;
		
		if(vSize.x%2!=0 || 
			vSize.y%2!=0)
			return eTextureType_Rect;

		return eTextureType_2D;
	}
	else
		return eTextureType_3D;
}

//------------------------------------------------------------------------------------

void cTextureWrapper::SetEnabled(bool abX)
{
	if(mbEnabled==abX)
		return;

	mbEnabled = abX;
	SetUpdated();
}

void cTextureWrapper::SetFile(const tWString& asX)
{
	if(msFile==asX)
		return;

	SetNeedsReload();
	SetUpdated();
	msFile = asX;

	mbEnabled = true;
}

//------------------------------------------------------------------------------------

void cTextureWrapper::SetWrap(const tWString& asX)
{
	tString sWrap = cString::To8Char(asX);
	if(msWrap==sWrap)
		return;

	SetUpdated();
	msWrap = sWrap;
}

//------------------------------------------------------------------------------------

void cTextureWrapper::SetAnimMode(const tWString& asX)
{
	tString sAnimMode = cString::To8Char(asX);
	if(msAnimMode==sAnimMode)
		return;

	SetUpdated();
	msAnimMode = sAnimMode;
}

//------------------------------------------------------------------------------------

void cTextureWrapper::SetCompressed(bool abX)
{
	if(mbCompressed==abX)
		return;

	SetNeedsReload();
	SetUpdated();
	mbCompressed = abX;
}

//------------------------------------------------------------------------------------

void cTextureWrapper::SetUseMipMaps(bool abX)
{
	if(mbMipMaps==abX)
		return;

	SetNeedsReload();
	SetUpdated();
	mbMipMaps = abX;
}

//------------------------------------------------------------------------------------

void cTextureWrapper::SetFrameTime(float afX)
{
	if(mfFrameTime==afX)
		return;

	SetUpdated();
	mfFrameTime = afX;
}
//------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------

//------------------------------------------------------------------------------------

cMaterialWrapper::cMaterialWrapper(cEditorWindowMaterialEditor* apEditor)
{
	mpMatEditor = apEditor;
	mpPreviewMat = NULL;

	cTextureManager* pTexMan = mpMatEditor->GetEditor()->GetEngine()->GetResources()->GetTextureManager();

	for(int i=0;i<eMaterialTexture_LastEnum;++i)
		mvTextures.push_back(hplNew(cTextureWrapper,(this, (eMaterialTexture)i)));

	mvDefaultTextures.resize(eMaterialTexture_LastEnum);
	mvDefaultTextures[eMaterialTexture_Diffuse] = pTexMan->Create2DImage("editor_default_diffuse.jpg", false).Release();
	mvDefaultTextures[eMaterialTexture_NMap] = pTexMan->Create2DImage("editor_rect_nrm.jpg", false).Release();
	for(int i=eMaterialTexture_NMap+1;i<eMaterialTexture_LastEnum;++i)
		mvDefaultTextures[i] = NULL;

	mpMatClass = NULL;
	mpClass = NULL;

	Reset();
}

//------------------------------------------------------------------------------------

cMaterialWrapper::~cMaterialWrapper()
{
	if(mpPreviewMat)
		mpMatEditor->GetEditor()->GetEngine()->GetResources()->GetMaterialManager()->Destroy(mpPreviewMat);

	for(int i=0;i<(int)mvTextures.size();++i)
	{
		hplDelete(mvTextures[i]);
	}
}

//------------------------------------------------------------------------------------

bool cMaterialWrapper::IsSaveable()
{
	bool bSaveable = false;
	for(int i=0;i<(int)mvTextures.size();++i)
	{
		cTextureWrapper* pTex = mvTextures[i];
		if(pTex->IsEnabled()==false || pTex->IsValid()==false)
			continue;

		bSaveable = true;
		break;
	}

	return bSaveable;
}

//------------------------------------------------------------------------------------

void cMaterialWrapper::Reset()
{
	mbPreviewUpdated = true;
	mbTypeChanged = true;

	msType = _W("");
	msPhysicsMat = _W("Default");
	mbDepthTest = true;
	mbUseAlpha = false;
	msBlendMode = _W("Add");

	for(int i=0;i<(int)mvTextures.size();++i)
		mvTextures[i]->Reset();

	mvUVAnimations.clear();



	GetPreviewMaterial();
}

//------------------------------------------------------------------------------------

void cMaterialWrapper::Load(const tWString& asFilename)
{
	cMaterialManager* pManager = mpMatEditor->GetEditor()->GetEngine()->GetResources()->GetMaterialManager();
	SharedResourceHandle<cMaterial> pMat = pManager->CreateMaterial(cString::To8Char(asFilename));

	if(!pMat)
		return;

	SetType(cString::To16Char(pMat->GetType()->GetName()));
	SetDepthTest(pMat->GetDepthTest());
	SetUseAlpha(pMat->GetUseAlphaDissolveFilter());
	SetPhysicsMaterial(cString::To16Char(pMat->GetPhysicsMaterial()));

	tWString sBlendMode = cString::To16Char(gsBlendModeStrings[pMat->GetBlendMode()]);
	if(sBlendMode==_W("None"))
		SetBlendMode(_W("Add"));
	else
		SetBlendMode(sBlendMode);

	for(int i=0;i<eMaterialTexture_LastEnum;++i)
	{
		mvTextures[i]->CreateFromImage(pMat->GetImage((eMaterialTexture)i));
		mvTextures[i]->Update();
	}

	//////////////////////////////////////////////////
	// Load UV Animations
	ClearUVAnims();
	for(int i=0;i<pMat->GetUvAnimationNum();++i)
	{
		cMaterialUvAnimation* pAnim = pMat->GetUvAnimation(i);
		AddUVAnim(pAnim->mType, pAnim->mfSpeed, pAnim->mfAmp, pAnim->mAxis);
	}

	//////////////////////////////////////////////////
	// Load variables
	if(mpClass)
	{
		cResourceVarsObject* pVars = pMat->GetVarsObject();
		mpClass->LoadFromResourceVarsObject(pVars);
	}
	// pMat (SharedResourceHandle) releases automatically.
}

//------------------------------------------------------------------------------------

void cMaterialWrapper::Save(const tWString& asFilename)
{
	cResources* pRes = mpMatEditor->GetEditor()->GetEngine()->GetResources();
	cMaterialManager* pManager = pRes->GetMaterialManager();
	tinyxml2::XMLDocument xmlDoc;
	tinyxml2::XMLElement* pDoc = xmlDoc.NewElement("Material");
	xmlDoc.InsertEndChild(pDoc);

	tinyxml2::XMLElement* pMain = pDoc->GetDocument()->NewElement("Main");
	pDoc->InsertEndChild(pMain);
	hpl::SetAttributeString(pMain, "Type", cString::To8Char(msType));
	hpl::SetAttributeBool(pMain, "DepthTest", mbDepthTest);
	hpl::SetAttributeBool(pMain, "UseAlpha", mbUseAlpha);
	hpl::SetAttributeString(pMain, "PhysicsMaterial", cString::To8Char(msPhysicsMat));
	hpl::SetAttributeString(pMain, "BlendMode", cString::To8Char(msBlendMode));

	bool bSave = true;
	tinyxml2::XMLElement* pUnits = pDoc->GetDocument()->NewElement("TextureUnits");
	pDoc->InsertEndChild(pUnits);

	iMaterialType* pType = GetTypePointer();
	for(int i=0;i<pType->GetUsedTextureNum();++i)
	{
		cTextureWrapper* pTex = mvTextures[pType->GetUsedTexture(i)->mType];
		if(pTex->IsEnabled()==false || pTex->IsValid()==false)
			continue;

		tinyxml2::XMLElement* pUnit = pUnits->GetDocument()->NewElement("");
		pUnits->InsertEndChild(pUnit);
		pTex->Save(pUnit);
	}

	/////////////////////////////////////////////////////
	// Save UV animations
	if(mvUVAnimations.empty()==false)
	{
		tinyxml2::XMLElement* pXmlAnims = pDoc->GetDocument()->NewElement("UvAnimations");
		pDoc->InsertEndChild(pXmlAnims);
		tString sTypeStrings[] =
		{
			"Translate",
			"Sin",
			"Rotate"
		};
		tString sAxisStrings[] =
		{
			"X",
			"Y",
			"Z"
		};

		for(int i=0;i<(int)mvUVAnimations.size();++i)
		{
			cMaterialUvAnimation* pAnim = &(mvUVAnimations[i]);
			tinyxml2::XMLElement* pXmlAnim = pXmlAnims->GetDocument()->NewElement("Animation");
			pXmlAnims->InsertEndChild(pXmlAnim);
			hpl::SetAttributeString(pXmlAnim, "Type", sTypeStrings[pAnim->mType]);
			hpl::SetAttributeString(pXmlAnim, "Axis", sAxisStrings[pAnim->mAxis]);
			hpl::SetAttributeFloat(pXmlAnim, "Speed", pAnim->mfSpeed);
			hpl::SetAttributeFloat(pXmlAnim, "Amplitude", pAnim->mfAmp);
		}
	}

	///////////////////////////////////////////
	// Save user variables
	if(mpClass)
	{
		tinyxml2::XMLElement* pXmlUserVars = pDoc->GetDocument()->NewElement("SpecificVariables");
		pDoc->InsertEndChild(pXmlUserVars);

		mpClass->Save(pXmlUserVars);
	}

	if(bSave)
		hpl::SaveXmlFile(xmlDoc, asFilename);

	UpdateMaterialInMemory(cString::To8Char(asFilename));
}

//------------------------------------------------------------------------------------

void cMaterialWrapper::UpdateMaterialInMemory(const tString& asName)
{
	cResources* pRes = mpMatEditor->GetEditor()->GetEngine()->GetResources();
	cMaterialManager* pMatMgr = pRes->GetMaterialManager();
	cTextureManager* pTexMgr = pRes->GetTextureManager();

	// Grab a temporary handle to the (cached) material. If nobody else references
	// it, it isn't live in the scene/preview, so there's nothing to update in place.
	SharedResourceHandle<cMaterial> pMat = pMatMgr->CreateMaterial(asName);
	if(!pMat)
		return;
	if(pMat->GetReferenceCount() <= 1)
		return;

	pMat->SetType(GetTypePointer());
	pMat->SetPhysicsMaterial(cString::To8Char(msPhysicsMat));
	pMat->SetBlendMode(pMatMgr->GetBlendMode(cString::To8Char(msBlendMode)));
	pMat->SetDepthTest(mbDepthTest);

	for(int i=0;i<eMaterialTexture_LastEnum;++i)
	{
		// SetImage's SharedResourceHandle releases the previous image through
		// the texture manager (manager-created materials auto-destroy their
		// textures) — no manual Destroy of the old texture needed.
		pMat->SetImage((eMaterialTexture)i, NULL);

		Image* pEditingTex = mvTextures[i]->GetImage();
		if(pEditingTex)
		{
			tString sName = cString::GetFileName(cString::To8Char(pEditingTex->GetFullPath()));
			bool bMipMaps = mvTextures[i]->GetUseMipMaps();
			Image* pNewTex = NULL;
			switch(mvTextures[i]->GetTextureType())
			{
			case eTextureType_1D:
				pNewTex = pTexMgr->Create1DImage(sName, bMipMaps).Release();
				break;
			case eTextureType_2D:
				pNewTex = pTexMgr->Create2DImage(sName, bMipMaps).Release();
				break;
			case eTextureType_3D:
				pNewTex = pTexMgr->Create3DImage(sName, bMipMaps).Release();
				break;
			case eTextureType_CubeMap:
				pNewTex = pTexMgr->CreateCubeMapImage(sName, bMipMaps).Release();
				break;
			case eTextureType_Rect:
				pNewTex = pTexMgr->Create2DImage(sName, bMipMaps, eTextureType_Rect).Release();
				break;
			default:
				break;
			}
			if(pNewTex)
				pMat->SetImage((eMaterialTexture)i, pNewTex);
		}
	}
	pMat->ClearUvAnimations();
	for(int i=0;i<pMat->GetUvAnimationNum();++i)
	{
		cMaterialUvAnimation* pAnim = &(mvUVAnimations[i]);
		pMat->AddUvAnimation(pAnim->mType,pAnim->mfSpeed,pAnim->mfSpeed,pAnim->mAxis);
	}

	// Push the edited user variables into the material. LoadVariables now builds
	// the material's data blob directly (there is no separate Compile step), and
	// runs after the textures above are set so its texture-dependent derivations
	// (alpha mode, water reflection) see the new images.
	if(mpClass)
	{
		cResourceVarsObject* pVarsObject = pMat->GetVarsObject();
		for(int i=0;i<mpClass->GetVarInstanceNum();++i)
		{
			cEditorVarInstance* pVar = mpClass->GetVarInstance(i);
			pVarsObject->SetUserVariable(cString::To8Char(pVar->GetName()), cString::To8Char(pVar->GetValue()));
		}
		pMat->LoadVariablesFromVarsObject(pVarsObject);
	}
}

//------------------------------------------------------------------------------------

bool cMaterialWrapper::ReloadTextures()
{
	for(int i=0;i<(int)mvTextures.size();++i)
	{
		cTextureWrapper* pTex = mvTextures[i];
		if(pTex->CheckFileIsUpdated())
		{
			Log("Texture %s was update outside editor, updating...\n", cString::To8Char(pTex->GetFile()).c_str());
			pTex->Update();
			SetUpdated();

			return true;
		}
	}

	return false;
}

//------------------------------------------------------------------------------------

void cMaterialWrapper::SetType(const tWString& asType)
{
	if(msType==asType)
		return;

	mbPreviewUpdated = true;
	mbTypeChanged = true;
	msType = asType;

	/////////////////////////////////////////////
	// Type has changed, update user variables
	iMaterialType* pType = GetTypePointer();

	if(mpMatClass)
		hplDelete(mpMatClass);
	if(mpClass)
		hplDelete(mpClass);

	mpMatClass = hplNew(cEditorClassMaterial,(mpMatEditor));
	if(mpMatClass->Create(pType))
		mpClass = mpMatClass->CreateInstance();
	else
	{
		hplDelete(mpMatClass);
		mpMatClass = NULL;
		mpClass = NULL;
	}
}

//------------------------------------------------------------------------------------

void cMaterialWrapper::SetPhysicsMaterial(const tWString& asMat)
{
	if(msPhysicsMat==asMat)
		return;

	mbPreviewUpdated = true;
	msPhysicsMat = asMat;
}

//------------------------------------------------------------------------------------

void cMaterialWrapper::SetDepthTest(bool abX)
{
	if(mbDepthTest==abX)
		return;

	mbPreviewUpdated = true;
	mbDepthTest = abX;
}

//------------------------------------------------------------------------------------

void cMaterialWrapper::SetUseAlpha(bool abX)
{
	if(mbUseAlpha==abX)
		return;

	mbPreviewUpdated = true;
	mbUseAlpha = abX;
}

//------------------------------------------------------------------------------------

void cMaterialWrapper::SetBlendMode(const tWString& asMode)
{
	if(msBlendMode==asMode)
		return;

	mbPreviewUpdated = true;
	msBlendMode = asMode;
}

//------------------------------------------------------------------------------------

void cMaterialWrapper::AddUVAnim(eMaterialUvAnimation aType, float afSpeed, float afAmp, eMaterialAnimationAxis aAxis)
{
	mvUVAnimations.push_back(cMaterialUvAnimation(aType, afSpeed, afAmp, aAxis));

	mbPreviewUpdated = true;
}

//------------------------------------------------------------------------------------

void cMaterialWrapper::RemoveUVAnim(int alIdx)
{
	if(alIdx<0 || alIdx>(int)mvUVAnimations.size())
		return;

	std::vector<cMaterialUvAnimation> vNewAnims;
	for(int i=0;i<(int)mvUVAnimations.size();++i)
	{
		if(i!=alIdx)
			vNewAnims.push_back(mvUVAnimations[i]);
	}
	mvUVAnimations = vNewAnims;
}

//------------------------------------------------------------------------------------

void cMaterialWrapper::ClearUVAnims()
{
	mvUVAnimations.clear();
}

//------------------------------------------------------------------------------------

cMaterialUvAnimation* cMaterialWrapper::GetUVAnim(int alIdx)
{
	if(alIdx<0 || alIdx>(int)mvUVAnimations.size())
		return NULL;

	return &(mvUVAnimations[alIdx]);
}
//------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------


//------------------------------------------------------------------------------------

//------------------------------------------------------------------------------------

cMaterial* cMaterialWrapper::GetPreviewMaterial()
{
	if(mbPreviewUpdated)
	{

		cEngine* pEng = mpMatEditor->GetEditor()->GetEngine();
		cGraphics* pGfx = pEng->GetGraphics();
		cMaterialManager* pManager = pEng->GetResources()->GetMaterialManager();

		//////////////////////////////////////////////
		// Destroy previous engine material
		if(mpPreviewMat)
		{
			pManager->Destroy(mpPreviewMat);
			mpPreviewMat=NULL;
		}

		iMaterialType* pType = GetTypePointer();
		if(pType==NULL)
			return NULL;

		if(mpPreviewMat==NULL)
		{
			mpPreviewMat = pManager->CreateCustomMaterial("PreviewMat", pType).Release();
			mpPreviewMat->SetAutoDestroyTextures(false);
		}

			/*
		{
			mpPreviewMat = pManager->CreateCustomMaterial("PreviewMat", pType);
		}
		else
			mpPreviewMat->SetType(pType);
			*/

		mpPreviewMat->SetDepthTest(mbDepthTest);
		mpPreviewMat->SetBlendMode(pManager->GetBlendMode(cString::To8Char(msBlendMode)));

		for(int i=0;i<eMaterialTexture_LastEnum;++i)
		{
			Image* pTex = NULL;
			cTextureWrapper* pTexWrapper = mvTextures[i];
			if(pTexWrapper->IsEnabled() && pTexWrapper->IsValid())
				pTex = pTexWrapper->GetImage();
			else
				pTex = mvDefaultTextures[i];

			// Preview material has SetAutoDestroyTextures(false): SetImage's
			// wrapper won't release wrapper-/default-owned images.
			mpPreviewMat->SetImage((eMaterialTexture)i, pTex);
		}

		mpPreviewMat->ClearUvAnimations();
		for(int i=0;i<(int)mvUVAnimations.size();++i)
		{
			cMaterialUvAnimation* pAnim = &(mvUVAnimations[i]);
			mpPreviewMat->AddUvAnimation(pAnim->mType,pAnim->mfSpeed, pAnim->mfAmp, pAnim->mAxis);
		}

		if(mpClass)
		{
			cResourceVarsObject* pVarsObject = mpPreviewMat->GetVarsObject();
			for(int i=0;i<mpClass->GetVarInstanceNum();++i)
			{
				cEditorVarInstance* pVar = mpClass->GetVarInstance(i);

				pVarsObject->SetUserVariable(cString::To8Char(pVar->GetName()), cString::To8Char(pVar->GetValue()));
			}
			mpPreviewMat->LoadVariablesFromVarsObject(pVarsObject);
		}
		// LoadVariablesFromVarsObject above builds the material data blob directly;
		// no separate Compile step is needed.

		mbPreviewUpdated=false;
	}

	return mpPreviewMat;
}

//------------------------------------------------------------------------------------

iMaterialType* cMaterialWrapper::GetTypePointer()
{
	cEngine* pEng = mpMatEditor->GetEditor()->GetEngine();
	cGraphics* pGfx = pEng->GetGraphics();

	return pGfx->GetMaterialType(cString::To8Char(msType));
}

//------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------

cTextureUnitPanel::cTextureUnitPanel(cEditorWindowMaterialEditor* apWin, eMaterialTexture aUnit)
{
	mpWindow = apWin;
	mUnit = aUnit;

	mpTextureWrapper = mpWindow->GetMaterial()->GetTextureWrapper(mUnit);

	cGraphics* pGfx = mpWindow->GetEditor()->GetEngine()->GetGraphics();
	iWidget* pParent = apWin->GetFrameUnits();
	tString sLabel = mpWindow->GetEditor()->GetEngine()->GetResources()->GetMaterialManager()->GetTextureString(mUnit);
	cGuiSet* pSet = mpWindow->GetSet();

	mpHandle = pSet->CreateWidgetDummy(0, pParent);
	mpLUnit = pSet->CreateWidgetLabel(0, 0, cString::To16Char(sLabel), mpHandle);
	mpLUnit->SetDefaultFontSize(cVector2f(12));

	mpInputs = pSet->CreateWidgetDummy(cVector3f(10,10,0) + cVector3f(0,mpLUnit->GetSize().y,0), mpHandle);

	cVector3f vPos = 0;

	mpInpFile = mpWindow->CreateInputFile(vPos, _W("File"), "", mpInputs);
	mpInpFile->SetBrowserType(eEditorResourceType_Texture);
	if(cString::ToLowerCase(sLabel)=="cubemap")
		mpInpFile->SetBrowserSubType(eEditorTextureResourceType_CubeMap);
	mpInpFile->SetUserData(this);

	vPos.y += mpInpFile->GetSize().y; 

	cWidgetFrame* pFThumb = pSet->CreateWidgetFrame(cVector3f(150,15,-0.01f), cVector2f(100), false, mpInputs);
	pFThumb->SetBackgroundZ(0);
	pFThumb->SetDrawBackground(true);
	pFThumb->SetBackGroundColor(cColor(0,1));
	
	mpImgThumb = pSet->CreateWidgetImage("", cVector3f(0,0,0.01f), cVector2f(100), eGuiMaterial_Diffuse, false, pFThumb);

	mpInpWrap = mpWindow->CreateInputEnum(vPos, _W("Wrap Mode"), "", tWStringList(), mpInputs);
	mpInpWrap->AddValue(_W("Repeat"));
	mpInpWrap->AddValue(_W("Clamp"));
	mpInpWrap->AddValue(_W("ClampToEdge"));
	mpInpWrap->AddValue(_W("ClampToBorder"));
	mpInpWrap->SetUserData(this);

	vPos.y += mpInpWrap->GetSize().y;

	mpInpMipMaps = mpWindow->CreateInputBool(vPos, _W("Use Mip Maps"), "", mpInputs);
	mpInpMipMaps->SetUserData(this);

	vPos.y += mpInpMipMaps->GetSize().y;

	mpInpAnimMode = mpWindow->CreateInputEnum(vPos, _W("Animation mode"), "", tWStringList(), mpInputs);
	mpInpAnimMode->AddValue(_W("None"));
	mpInpAnimMode->AddValue(_W("Loop"));
	mpInpAnimMode->AddValue(_W("Oscillate"));
	mpInpAnimMode->SetUserData(this);

	vPos.y += mpInpAnimMode->GetSize().y;

	mpInpFrameTime = mpWindow->CreateInputNumber(vPos, _W("Frame time"), "", mpInputs);
	mpInpFrameTime->SetUserData(this);

	Reset();
}

//------------------------------------------------------------------------------------

cTextureUnitPanel::~cTextureUnitPanel()
{
}

//------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------

void cTextureUnitPanel::Reset()
{
	cGuiGfxElement* pImg = mpImgThumb->GetImage();
	if(pImg)
		mpWindow->GetSet()->GetGui()->DestroyGfx(pImg);
	mpImgThumb->SetImage(NULL);

	Update();
}

//------------------------------------------------------------------------------------

void cTextureUnitPanel::Update()
{
	cGui* pGui = mpWindow->GetSet()->GetGui();
	cGuiGfxElement* pImg = mpImgThumb->GetImage();
	if(pImg)
		pGui->DestroyGfx(pImg);

	if(mpTextureWrapper->IsValid()==false)
	{
		mpInpFile->SetValue(_W(""), false);
		pImg = NULL;
	}
	else
	{
		const tWString& sTextureFile = mpTextureWrapper->GetFile();
		mpInpFile->SetValue(mpTextureWrapper->GetFile(), false);
		Image* pTex = mpTextureWrapper->GetImage();
		if(pTex)
		{
			// cGui only builds elements from Image* now (the iTexture* overload
			// is gone) — reload the thumbnail from the texture file through the
			// Image-based loader instead of wrapping the legacy texture.
			pImg = pGui->CreateGfxTexture(cString::To8Char(cString::GetFileNameW(sTextureFile)),
										  eGuiMaterial_Alpha, mpTextureWrapper->GetTextureType());
		}
	}

	mpInpFile->SetInitialPath(mpWindow->msLastTexturePath);

	mpInpMipMaps->SetValue(mpTextureWrapper->GetUseMipMaps(), false);
	mpInpWrap->SetValue(mpWindow->GetEditor()->GetEngine()->GetResources()->GetMaterialManager()->GetWrap(mpTextureWrapper->GetWrap()), false);
		

	mpImgThumb->SetImage(pImg);
	((iEditorInput*)mpInpAnimMode)->SetValue(cString::To16Char(mpTextureWrapper->GetAnimMode()), false);
	mpInpFrameTime->SetValue(mpTextureWrapper->GetFrameTime(), false);

}

//------------------------------------------------------------------------------------

void cTextureUnitPanel::UpdateTexture()
{
	mpTextureWrapper->Update();
}

//------------------------------------------------------------------------------------

bool cTextureUnitPanel::PanelSpecificInputCallback(iEditorInput* apInput)
{
	cMaterialWrapper* pMat = mpWindow->GetMaterial();
	cGraphics* pGfx = mpWindow->GetEditor()->GetEngine()->GetGraphics();
	if(apInput==mpInpFile)
	{
		mpTextureWrapper->SetFile(mpInpFile->GetValue());
		mpWindow->msLastTexturePath = mpInpFile->GetValue();
		mpWindow->GetEditor()->SetLayoutNeedsUpdate(true);
	}
	else if(apInput==mpInpMipMaps)
	{
		mpTextureWrapper->SetUseMipMaps(mpInpMipMaps->GetValue());
	}
	else if(apInput==mpInpWrap)
	{
		mpTextureWrapper->SetWrap(mpInpWrap->GetEnumValue(mpInpWrap->GetValue()));
	}
	else if(apInput==mpInpAnimMode)
	{
		mpTextureWrapper->SetAnimMode(mpInpAnimMode->GetEnumValue(mpInpAnimMode->GetValue()));
	}
	else if(apInput==mpInpFrameTime)
	{
		mpTextureWrapper->SetFrameTime(mpInpFrameTime->GetValue());
	}

	mpTextureWrapper->Update();
	mpWindow->SetPreviewUpdated();
	Update();

	return true;
}

//------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------

//------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------


cEditorWindowMaterialEditor::cEditorWindowMaterialEditor(iEditorBase* apEditor, 
														 const tWString& asMatFile, cEditorInputFile* apInput, 
														 bool abStandAlone) : iEditorWindowPopUp(apEditor,"Material Editor Window", 
																								 abStandAlone==false, abStandAlone==false, abStandAlone==false,
																								 cVector2f(900,725)),
																			  iEditorViewport(apEditor, NULL)
{
	///////////////////////////////////////////////////////////////
	// Set up world
	mpMatWorld = mpEditor->GetEngine()->GetScene()->CreateWorld("Mat");
	mmtxRotation = cMatrixf::Identity;

	mpMatWorld->SetSkyBoxColor(cColor(0,0,1,1));
	mpMatWorld->SetSkyBoxActive(true);

	// Load models
	cResources* pRes = mpEditor->GetEngine()->GetResources();
	cMeshManager* pManager = pRes->GetMeshManager();
	cMesh* pMesh = NULL;
	pMesh = pManager->CreateMesh("mateditor_cube.dae");
	if(pMesh)
		mvPreviewEntities.push_back(mpMatWorld->CreateMeshEntity("PrevBox", pMesh));

	pMesh = pManager->CreateMesh("mateditor_cylinder.dae");
	if(pMesh)
		mvPreviewEntities.push_back(mpMatWorld->CreateMeshEntity("PrevCylinder", pMesh));

	pMesh = pManager->CreateMesh("mateditor_sphere.dae");
	if(pMesh)
		mvPreviewEntities.push_back(mpMatWorld->CreateMeshEntity("PrevSphere", pMesh));

	pMesh = pManager->CreateMesh("mateditor_plane.dae");
	if(pMesh)
		mvPreviewEntities.push_back(mpMatWorld->CreateMeshEntity("PrevPlane", pMesh));

	for(int i=0;i<(int)mvPreviewEntities.size();++i)
	{
		mvPreviewEntities[i]->SetVisible(false);
		mvPreviewEntities[i]->SetActive(false);
	}
	mpCurrentPreviewEntity = NULL;

	mpFixedLight = mpMatWorld->CreateLightPoint();
	mpFixedLight->SetRadius(6);
	mpFixedLight->SetDiffuseColor(cColor(1,1));
	mpFixedLight->SetPosition(cVector3f(0,0,2.5f));

	mpMovableLight = mpMatWorld->CreateLightPoint();
	mpMovableLight->SetRadius(6);
	mpMovableLight->SetDiffuseColor(cColor(1,1));
	mpMovableLight->SetPosition(cVector3f(2.5f,0,0));

	mCamera.SetCameraType(eEditorCameraType_Orbit);
	mCamera.SetKeepOrientation(false);
	mCamera.SetTargetPosition(cVector3f(0));
	mCamera.SetCameraPosition(cVector3f(0,0,2.5f));
	mCamera.SetZoom(0,true);
	mCamera.UpdateCamera(0);

	mbMoveLight = false;
	mbMoveCamera = false;

	mbRotationEnabled = false;

	mpEngineViewport->SetWorld(mpMatWorld);
	mpEngineViewport->SetCamera(mCamera.GetEngineCamera());

	mpMaterial = NULL;

	mpInputPanel = NULL;

	mpDestInput = apInput;

	msInitMatFile = asMatFile;

	mbStandAlone = abStandAlone;
}

//------------------------------------------------------------------------------------

cEditorWindowMaterialEditor::~cEditorWindowMaterialEditor()
{
	UpdatePreview(NULL);
	hplDelete(mpMaterial);
	STLDeleteAll(mvUnitPanels);

	mpEditor->GetEngine()->GetScene()->DestroyWorld(mpMatWorld);
}

//------------------------------------------------------------------------------------

void cEditorWindowMaterialEditor::SetUpUnits()
{
	for(int i = 0; i< eMaterialTexture_LastEnum; ++i)
	{
		cEditorInputBool* pSwitch = mvUnitSwitches[i];
		cTextureUnitPanel* pPanel = mvUnitPanels[i];
		pSwitch->GetHandle()->SetEnabled(false);
		pSwitch->GetHandle()->SetVisible(false);		
		pPanel->GetHandle()->SetEnabled(false);
		pPanel->GetHandle()->SetVisible(false);
		pPanel->Update();
	}

	if(mpMaterial==NULL)
		return;
		
	iMaterialType* pType = mpMaterial->GetTypePointer();
	if(pType==NULL)
		return;

	cVector3f vSwitchPos = cVector3f(10,10,0.1f);
	for(int i=0;i<pType->GetUsedTextureNum();++i)
	{
		eMaterialTexture index = pType->GetUsedTexture(i)->mType;
		bool bIsEnabled = mpMaterial->GetTextureWrapper((eMaterialTexture)index)->IsEnabled();
		
		cEditorInputBool* pSwitch = mvUnitSwitches[index];
		pSwitch->GetHandle()->SetEnabled(true);
		pSwitch->GetHandle()->SetVisible(true);
		pSwitch->SetValue(bIsEnabled, false);
		pSwitch->GetHandle()->SetPosition(vSwitchPos);

		vSwitchPos.y += pSwitch->GetHandle()->GetSize().y + 5;
	}
}

//------------------------------------------------------------------------------------

void cEditorWindowMaterialEditor::SetUpPanels()
{
	cVector3f vPanelPos = cVector3f(0,0,0.1f);
	for(int i=0;i<eMaterialTexture_LastEnum;++i)
	{
		bool bIsTextureEnabled = mpMaterial->GetTextureWrapper((eMaterialTexture)i)->IsEnabled();
		bool bIsShown = bIsTextureEnabled && mvUnitSwitches[i]->GetHandle()->IsVisible();
	
		cTextureUnitPanel* pPanel = mvUnitPanels[i];
		pPanel->GetHandle()->SetEnabled(bIsShown);
		pPanel->GetHandle()->SetVisible(bIsShown);
		pPanel->UpdateTexture();
		pPanel->Update();
		if(bIsShown)
		{
			pPanel->GetHandle()->SetPosition(vPanelPos);
			vPanelPos.y += pPanel->GetHandle()->GetSize().y + 10;
		}
	}
}

//------------------------------------------------------------------------------------

void cEditorWindowMaterialEditor::SetUpVars()
{
	if(mpInputPanel)
		hplDelete(mpInputPanel);

	cEditorClassInstance* pInstance = mpMaterial->GetClass();
	mpInputPanel = pInstance->CreateInputPanel(this, mpFMaterialVars, true);
	
	if(mpInputPanel)
		mpInputPanel->Update();
}

//------------------------------------------------------------------------------------

bool cEditorWindowMaterialEditor::LoadMaterial(const tWString& asFilename)
{
	if(cString::ToLowerCaseW(cString::GetFileExtW(asFilename))!=_W("mat"))
	{
		mpEditor->ShowMessageBox(_W("Error"), asFilename + _W(" is not a material file"), _W("OK"), _W(""), NULL,NULL);
		return false;
	}

	tWString sFilename = asFilename;
	Reset();
	msMatFilename = sFilename;
	msLastTexturePath = msMatFilename;
	mpMaterial->Load(msMatFilename);
	mpEditor->SetLayoutNeedsUpdate(true);

	return true;
}

//------------------------------------------------------------------------------------

void cEditorWindowMaterialEditor::SaveMaterial(const tWString& asFile)
{
	mpMaterial->Save(asFile);
	msMatFilename = asFile;

	mpEditor->SetLayoutNeedsUpdate(true);
}

//------------------------------------------------------------------------------------

void cEditorWindowMaterialEditor::Command_New()
{
	Reset();
}

//------------------------------------------------------------------------------------

void cEditorWindowMaterialEditor::Command_Open()
{
	cGuiPopUpFilePicker* pPicker = mpSet->CreatePopUpLoadFilePicker(mvTempLoadedFiles, false, cString::GetFilePathW(msMatFilename), false, this, kGuiCallback(FilePickerLoad));
	pPicker->AddCategory(_W("Materials"), _W("*.mat"));
}

//------------------------------------------------------------------------------------

void cEditorWindowMaterialEditor::Command_Save()
{
	if(mpMaterial->IsSaveable()==false)
		return;
	if(msMatFilename==_W(""))
		Command_SaveAs();
	else
		SaveMaterial(msMatFilename);
}

//------------------------------------------------------------------------------------

void cEditorWindowMaterialEditor::Command_SaveAs()
{
	if(mpMaterial->IsSaveable()==false)
		return;
	mpSet->CreatePopUpSaveFilePicker(msMatFilename, _W("Materials"), _W("*.mat"), cString::GetFilePathW(msMatFilename), false, this, kGuiCallback(FilePickerSave));
}

//------------------------------------------------------------------------------------

void cEditorWindowMaterialEditor::Reset()
{
	mpWindow->SetText(_W("Unnamed material - Material Editor"));

	msMatFilename = _W("");
	mpMaterial->Reset();

	mpInpDepthTest->SetValue(true);
	mpInpMatType->SetValue(0);
	mpInpPhysicsMat->SetValue(0);
	mpInpBlendMode->SetValue(0);

	for(int i=0;i<(int)mvUnitPanels.size();++i)
		mvUnitPanels[i]->Reset();

	SetUpPanels();

	///////////////////////////////////////////////////////
	// Reset preview pane
	const tWString sCubeMapPath = mpEditor->GetEngine()->GetResources()->GetFileSearcher()->GetFilePath("editor_cubemap_default.dds");
	mpInpBGCubeMap->SetValue(sCubeMapPath);
	mpInpPreviewModel->SetValue(0);
	mpInpRotateModel->SetValue(false);
	mpInpBGType->SetValue(0);
	mpInpBGColor->SetValue(cColor(0,0,1,1));

	mpInpFixedLightColor->SetValue(cColor(1));
	mpInpMovableLightColor->SetValue(cColor(1));
	mpMovableLight->SetPosition(cVector3f(2.5f,0,0));

	mCamera.SetCameraPosition(cVector3f(0,0,2.5f));
	mCamera.SetZoom(0,true);
	mCamera.UpdateCamera(0);

	SetPreviewUpdated();
	mpEditor->SetLayoutNeedsUpdate(true);
}

//------------------------------------------------------------------------------------

bool cEditorWindowMaterialEditor::FilePickerLoad(iWidget* apWidget, const cGuiMessageData& aData)
{
	if(aData.mlVal==1)
	{
		msMatFilename = mvTempLoadedFiles.front();
		mvTempLoadedFiles.clear();
		LoadMaterial(msMatFilename);
	}
	return true;
}
kGuiCallbackDeclaredFuncEnd(cEditorWindowMaterialEditor,FilePickerLoad);

//------------------------------------------------------------------------------------

bool cEditorWindowMaterialEditor::FilePickerSave(iWidget* apWidget, const cGuiMessageData& aData)
{
	if(aData.mlVal==1)
		SaveMaterial(msMatFilename);
	return true;
}
kGuiCallbackDeclaredFuncEnd(cEditorWindowMaterialEditor,FilePickerSave);

//------------------------------------------------------------------------------------

bool cEditorWindowMaterialEditor::ButtonPressed(iWidget* apWidget, const cGuiMessageData& aData)
{
	if(apWidget==mpBAddAnim)
	{
		mpMaterial->AddUVAnim(eMaterialUvAnimation_Translate,0,0,eMaterialAnimationAxis_X);
	}
	else if(apWidget==mpBRemAnim)
	{
		mpMaterial->RemoveUVAnim(mpInpUVAnimation->GetValue());
	}

	mpEditor->SetLayoutNeedsUpdate(true);
	return true;
}
kGuiCallbackDeclaredFuncEnd(cEditorWindowMaterialEditor,ButtonPressed);

//------------------------------------------------------------------------------------

void cEditorWindowMaterialEditor::UpdateUVAnimInputs()
{
	int lSelectedAnim = mpInpUVAnimation->GetValue();
	cMaterialUvAnimation* pAnim = mpMaterial->GetUVAnim(lSelectedAnim);
	if(pAnim)
	{
		mpInpUVAnimType->SetValue(pAnim->mType,false);
		mpInpUVAnimAxis->SetValue(pAnim->mAxis,false);
		mpInpUVAnimSpeed->SetValue(pAnim->mfSpeed,false);
		mpInpUVAnimAmp->SetValue(pAnim->mfAmp,false);
	}
	else
	{
		mpInpUVAnimType->SetValue(-1,false);
		mpInpUVAnimAxis->SetValue(-1,false);
		mpInpUVAnimSpeed->SetValue(0,false);
		mpInpUVAnimAmp->SetValue(0,false);
	}

	mpInpUVAnimType->GetHandle()->SetEnabled(pAnim!=NULL);
	mpInpUVAnimAxis->GetHandle()->SetEnabled(pAnim!=NULL);
	mpInpUVAnimSpeed->GetHandle()->SetEnabled(pAnim!=NULL);
	mpInpUVAnimAmp->GetHandle()->SetEnabled(pAnim!=NULL);
}

//------------------------------------------------------------------------------------

void cEditorWindowMaterialEditor::SetWindowCaption(const tWString& asCaption)
{
	if(mbStandAlone)
		mpEditor->GetEngine()->GetGraphics()->GetWindow()->SetCaption(cString::To8Char(asCaption));
	else
		mpWindow->SetText(asCaption);
}

//------------------------------------------------------------------------------------

void cEditorWindowMaterialEditor::CreateMaterial(const tString& asMat)
{
	SetPreviewUpdated();
}

//------------------------------------------------------------------------------------

void cEditorWindowMaterialEditor::UpdatePreview(cMaterial* apMat)
{
	cMaterialManager* pMatMgr = GetEditor()->GetEngine()->GetResources()->GetMaterialManager();
	for(int i=0;i<(int)this->mvPreviewEntities.size();++i)
	{
		cMesh* pMesh = mvPreviewEntities[i]->GetMesh();
		for(int j=0;j<pMesh->GetSubMeshNum();++j)
		{
			cSubMesh* pSubMesh = pMesh->GetSubMesh(j);
			// apMat is borrowed; each submesh takes its own counted reference.
			pSubMesh->SetMaterial(RetainResource<cMaterial>(pMatMgr, apMat));
		}
	}
}

//------------------------------------------------------------------------------------

void cEditorWindowMaterialEditor::OnInit()
{
	mpMaterial = hplNew(cMaterialWrapper,(this));
}

//------------------------------------------------------------------------------------

void cEditorWindowMaterialEditor::OnInitLayout()
{
	iEditorWindowPopUp::OnInitLayout();
	iWidget* pParent = (iWidget*)mpWindow;

	if(mbStandAlone)
	{
		pParent = (iWidget*)mpBGFrame;
		mpWindow->SetSize(0);
		mpWindow->SetVisible(false);
		mpWindow->SetEnabled(false);
		mpBGFrame->SetPosition(cVector3f(0,0,0.05f));
		mpBGFrame->SetSize(mpSet->GetVirtualSize());
		mpBGFrame->SetVisible(true);
		mpBGFrame->SetEnabled(true);
	}

	//////////////////////////////////////////////////
	// Window Main menu setup
	cWidgetMainMenu* pMenu = mpSet->CreateWidgetMainMenu(pParent);
	cWidgetMenuItem* pItem = pMenu->AddMenuItem(_W("File"));
	//////////////////
	// File.New
	mpMenuNew = pItem->AddMenuItem(_W("New"));
	mpMenuNew->AddCallback(eGuiMessage_ButtonPressed, this, kGuiCallback(MenuCallback));
	pItem->AddSeparator();
	//////////////////
	// File.Open
	mpMenuOpen = pItem->AddMenuItem(_W("Open"));
	mpMenuOpen->AddCallback(eGuiMessage_ButtonPressed, this, kGuiCallback(MenuCallback));
	//////////////////
	// File.Save
	mpMenuSave = pItem->AddMenuItem(_W("Save"));
	mpMenuSave->AddCallback(eGuiMessage_ButtonPressed, this, kGuiCallback(MenuCallback));
	//////////////////
	// File.Save As
	mpMenuSaveAs = pItem->AddMenuItem(_W("Save As"));
	mpMenuSaveAs->AddCallback(eGuiMessage_ButtonPressed, this, kGuiCallback(MenuCallback));
	pItem->AddSeparator();
	//////////////////
	// File.Exit
	mpMenuExit = pItem->AddMenuItem(_W("Exit"));
	mpMenuExit->AddCallback(eGuiMessage_ButtonPressed, this, kGuiCallback(MenuCallback));


	float fStartY = pMenu->GetLocalPosition().y+pMenu->GetSize().y+5;

	cWidgetGroup* pGroup = NULL;

	////////////////////////////////////////////////
	// Preview
	{
		pGroup = mpSet->CreateWidgetGroup(cVector3f(440,fStartY,0.1f), cVector2f(450,565), _W("Preview"), pParent);

		////////////////////////////////////////////////
		// Set up preview viewport
		CreateGuiViewport(pGroup);
		SetGuiViewportPos(cVector3f(10,10,0.1f));
		SetGuiViewportSize(cVector2f(430));

		// Legacy iFrameBuffer chain is dead on the RI backend (CreateFrameBuffer
		// returns NULL) — the preview pane renders through the editor-owned pane
		// surface set as the engine viewport's TargetView instead (see
		// iEditorViewport::UpdateViewport).
		SetEngineViewportPositionAndSize(0, 512);
		UpdateViewport();
		SetViewportActive(true);

		////////////////////////////////
		// Preview Controls

		// Preview Model selector
		mpInpPreviewModel = CreateInputEnum(cVector3f(10,15,0.1f) + cVector3f(0,GetGuiViewportSize().y,0), _W("Model"), "", tWStringList(),pGroup);
		mpInpPreviewModel->AddValue(_W("Box"));
		mpInpPreviewModel->AddValue(_W("Cylinder"));
		mpInpPreviewModel->AddValue(_W("Sphere"));
		mpInpPreviewModel->AddValue(_W("Plane"));

		// Rotate Model
		mpInpRotateModel = CreateInputBool(mpInpPreviewModel->GetPosition() + cVector3f(mpInpPreviewModel->GetSize().x+10, 15, 0), _W("Rotate Model"), "", pGroup);

		// Background Type
		mpInpBGType = CreateInputEnum(mpInpPreviewModel->GetPosition()+ cVector3f(0,mpInpPreviewModel->GetSize().y + 10,0), _W("Background Type"), "", tWStringList(), pGroup);
		mpInpBGType->AddValue(_W("CubeMap"));
		mpInpBGType->AddValue(_W("Flat"));

		// Background CubeMap
		mpInpBGCubeMap = CreateInputFile(mpInpBGType->GetPosition() + cVector3f(mpInpBGType->GetSize().x + 10,0,0), _W("CubeMap file"), "", pGroup);
		mpInpBGCubeMap->SetBrowserSubType(eEditorTextureResourceType_CubeMap);
		// Background Color
		mpInpBGColor = CreateInputColorFrame(mpInpBGCubeMap->GetPosition() + cVector3f(mpInpBGCubeMap->GetSize().x +10,15,0), _W("Background color"),"", pGroup);

		// Lights color
		mpInpFixedLightColor = CreateInputColorFrame(mpInpBGType->GetPosition() + cVector3f(0,mpInpBGType->GetSize().y+10,0),_W("Fixed light color"),"", pGroup);
		mpInpMovableLightColor = CreateInputColorFrame(mpInpFixedLightColor->GetPosition() + cVector3f(mpInpFixedLightColor->GetSize().x+10,0,0),_W("Movable light color"),"", pGroup);
	}



	cVector3f vPos;
	////////////////////////////////////////////////////////
	// General inputs
	{
		pGroup = mpSet->CreateWidgetGroup(cVector3f(10,fStartY,0.1f), cVector2f(420, 85), _W("General"), pParent);

		vPos = cVector3f(10,10,0.1f);
		// Material Type
		mpInpMatType = CreateInputEnum(vPos, _W("Type:"), "", tWStringList(), pGroup);
		
		// Add type names
		tStringVec vTypeNames = mpEditor->GetEngine()->GetGraphics()->GetMaterialTypeNames();
		for(int i=0;i<(int)vTypeNames.size();++i)
			mpInpMatType->AddValue(cString::To16Char(vTypeNames[i]));

		vPos.y += mpInpMatType->GetSize().y;

		// Physisc material
		mpInpPhysicsMat = CreateInputEnum(vPos, _W("Physics Material"), "", tWStringList(), pGroup);
		// Populate physics material data
		cSurfaceDataIterator it = mpEditor->GetEngine()->GetPhysics()->GetSurfaceDataIterator();
		while(it.HasNext())
		{
			cSurfaceData* pData = it.Next();
			mpInpPhysicsMat->AddValue(cString::To16Char(pData->GetName()));
		}

		vPos = cVector3f(130, 22, 0.1f);
		// Depth Test
		mpInpDepthTest = CreateInputBool(vPos, _W("Depth Test"), "", pGroup);

		vPos.y += mpInpDepthTest->GetSize().y;

		// Value (TODO: I still dont know what this is for)
		mpInpValue = CreateInputNumber(vPos, _W("Value"), "", pGroup);

		vPos = cVector3f(230,10, 0.1f);

		// Blend Mode
		mpInpBlendMode = CreateInputEnum(vPos, _W("Blend Mode"), "", tWStringList(), pGroup);
		// Add blend modes
		for(int i=1;gsBlendModeStrings[i]!="";++i)
			mpInpBlendMode->AddValue(cString::To16Char(gsBlendModeStrings[i]));
	}

	vPos = cVector3f(10, mpInpPhysicsMat->GetPosition().y + mpInpPhysicsMat->GetSize().y+10,0.1f);

	////////////////////////////////////////
	// Animations
	{
		pGroup = mpSet->CreateWidgetGroup(pGroup->GetLocalPosition() + cVector3f(0, pGroup->GetSize().y+10,0), cVector2f(420,80), _W("UV Animations"), pParent);

		vPos = cVector3f(10,10,0.1f);

		// Animation selector
		mpInpUVAnimation = CreateInputEnum(vPos, _W("UV Animation"), "", tWStringList(), pGroup);

		// Add anim button
		mpBAddAnim = mpSet->CreateWidgetButton(vPos + cVector3f(0, mpInpUVAnimation->GetSize().y + 5,0), cVector2f(50,20), _W("Create"), pGroup);
		mpBAddAnim->AddCallback(eGuiMessage_ButtonPressed,this,kGuiCallback(ButtonPressed));

		// Remove anim button
		mpBRemAnim = mpSet->CreateWidgetButton(vPos + cVector3f(55,mpInpUVAnimation->GetSize().y + 5,0), cVector2f(50,20), _W("Delete"), pGroup);
		mpBRemAnim->AddCallback(eGuiMessage_ButtonPressed,this,kGuiCallback(ButtonPressed));

		vPos.x += mpInpUVAnimation->GetSize().x + 30;

		// Anim type
		mpInpUVAnimType = CreateInputEnum(vPos, _W("Anim Type"), "", tWStringList(), pGroup, 80);
		mpInpUVAnimType->AddValue(_W("Translate"));
		mpInpUVAnimType->AddValue(_W("Sin"));
		mpInpUVAnimType->AddValue(_W("Rotate"));

		vPos.x += mpInpUVAnimType->GetSize().x +5;

		// Axis
		mpInpUVAnimAxis = CreateInputEnum(vPos, _W("Axis"), "", tWStringList(), pGroup, 40);
		mpInpUVAnimAxis->AddValue(_W("X"));
		mpInpUVAnimAxis->AddValue(_W("Y"));
		mpInpUVAnimAxis->AddValue(_W("Z"));

		vPos.x += mpInpUVAnimAxis->GetSize().x +5;

		// Speed
		mpInpUVAnimSpeed = CreateInputNumber(vPos, _W("Speed"), "", pGroup, 40, 0.1f);

		vPos.x += mpInpUVAnimSpeed->GetSize().x +5;

		// Sin Amplitude
		mpInpUVAnimAmp = CreateInputNumber(vPos, _W("Amplitude"), "", pGroup, 40, 0.1f);

		vPos.x += mpInpUVAnimAmp->GetSize().x +5;
	}


	pGroup = mpSet->CreateWidgetGroup(pGroup->GetLocalPosition() + cVector3f(0,pGroup->GetSize().y+10,0), cVector2f(420,380), _W("Texture Units"), pParent);

	////////////////////////////////////////////////////
	// Texture Unit Panels
	{
		mpFUnits = mpSet->CreateWidgetFrame(cVector3f(110,10,0.1f), cVector2f(300,340),false, pGroup, false, true);

		mvUnitSwitches.resize(eMaterialTexture_LastEnum);
		mvUnitPanels.resize(eMaterialTexture_LastEnum);
		for(int i=0;i<eMaterialTexture_LastEnum;++i)
		{
			// Texture Unit Panel itself
			cTextureUnitPanel* pPanel = hplNew( cTextureUnitPanel,(this, (eMaterialTexture)i));
			pPanel->GetHandle()->SetVisible(false);
			pPanel->GetHandle()->SetEnabled(false);

			// Texture Unit switch
			cEditorInputBool* pSwitch = CreateInputBool(0,pPanel->mpLUnit->GetText(), "", pGroup);
			pSwitch->GetHandle()->SetVisible(false);
			pSwitch->GetHandle()->SetEnabled(false);

			mvUnitPanels[i] = pPanel;
			mvUnitSwitches[i] = pSwitch;
		}
	}

	//////////////////////////////////////////////////////////
	// Material specific vars
	{
		pGroup = mpSet->CreateWidgetGroup(pGroup->GetLocalPosition() + cVector3f(0,pGroup->GetSize().y+10,0), cVector2f(880, 90), _W("Material Specific Variables"), pParent);

		mpFMaterialVars = mpSet->CreateWidgetFrame(cVector3f(15,10,0.1f), cVector2f(860,75), false, pGroup, false, true);
	}
}

//------------------------------------------------------------------------------------

void cEditorWindowMaterialEditor::PostInitLayout()
{
	mpEditor->GetEngine()->GetResources()->AddResourceDir(cString::GetFilePathW(msInitMatFile),false);
	if(msInitMatFile==_W("") || LoadMaterial(msInitMatFile)==false) 
		Reset();
}

//------------------------------------------------------------------------------------

void cEditorWindowMaterialEditor::OnSetActive(bool abX)
{
	cVector3f vPos = GetPosition();
	iEditorWindowPopUp::OnSetActive(abX);
	if(mbStandAlone)
	{
		SetPosition(vPos);
	}
}

//------------------------------------------------------------------------------------

void cEditorWindowMaterialEditor::OnUpdate(float afTimeStep)
{
	/////////////////////////////////////////////////////
	// Update window caption
	tWString sFile = _W("Unnamed material");
	if(msMatFilename!=_W(""))
		sFile = cString::GetFileNameW(msMatFilename);
	SetWindowCaption(sFile + _W(" - HPL Material Editor"));

	/////////////////////////////////////////////////////
	// Update type, physics material and blend mode
	const tWString& sType = mpMaterial->GetType();
	const tWString& sMat = mpMaterial->GetPhysicsMat();
	const tWString& sBlendMode = mpMaterial->GetBlendMode();
	((iEditorInput*)mpInpMatType)->SetValue(sType, false);
	mpInpDepthTest->SetValue(mpMaterial->GetDepthTest(), false);
	((iEditorInput*)mpInpPhysicsMat)->SetValue(sMat, false);
	((iEditorInput*)mpInpBlendMode)->SetValue(sBlendMode,false);

	// Update panels
	SetUpUnits();
	SetUpPanels();
	SetUpVars();
	
	/////////////////////////////////////////////////////
	// Update animation stuff
	int lSelectedAnim = mpInpUVAnimation->GetValue();
	mpInpUVAnimation->ClearValues();
	for(int i=0;i<mpMaterial->GetUVAnimNum();++i)
	{
		cMaterialUvAnimation* pAnim = mpMaterial->GetUVAnim(i);
		mpInpUVAnimation->AddValue(_W("Anim") + cString::ToStringW((float)i,0));
	}
	if(lSelectedAnim==-1 || lSelectedAnim>=mpInpUVAnimation->GetNumValues())
		lSelectedAnim = mpInpUVAnimation->GetNumValues()-1;
	
	mpInpUVAnimation->SetValue(lSelectedAnim,false);
	UpdateUVAnimInputs();
	
	//////////////////////////////////////////////////////
	// Update preview controls
	mpInpBGColor->SetValue(mpMatWorld->GetSkyBoxColor(), false);
	mpInpFixedLightColor->SetValue(mpFixedLight->GetDiffuseColor(), false);
	mpInpMovableLightColor->SetValue(mpMovableLight->GetDiffuseColor(), false);
}

//------------------------------------------------------------------------------------

bool cEditorWindowMaterialEditor::OnViewportUpdate(const cGuiMessageData& aData)
{
	if(mbPreviewNeedsUpdate)
	{
		mbPreviewNeedsUpdate=false;
		UpdatePreview(mpMaterial->GetPreviewMaterial());
	}

	if(mbRotationEnabled)
	{
		mmtxRotation = cMath::MatrixMul(cMath::MatrixRotate(cVector3f(0,0.005f,0), eEulerRotationOrder_XYZ), mmtxRotation);
		mpCurrentPreviewEntity->SetMatrix(mmtxRotation);
	}
	return true;
}

//------------------------------------------------------------------------------------

bool cEditorWindowMaterialEditor::OnViewportMouseMove(const cGuiMessageData& aData)
{
	cVector2f vRel = aData.mvRel*0.01f;
	if(mbMoveCamera)
	{
		mCamera.UpdateCamera(vRel);
	}
	if(mbMoveLight)
	{
		cVector2f vSphCoords = cMath::GetSphericalCoordsFromPoint3D(0, mpMovableLight->GetWorldPosition());
		vSphCoords += cVector2f(vRel.y, -vRel.x);
		vSphCoords.x = cMath::Clamp(vSphCoords.x, 0.01f, kPif-0.01f);
		mpMovableLight->SetPosition(cMath::GetPoint3DFromSphericalCoords(0, 2.5f, vSphCoords));
	}
	return true;
}

//------------------------------------------------------------------------------------

bool cEditorWindowMaterialEditor::OnViewportMouseDown(const cGuiMessageData& aData)
{
	if(aData.mlVal&eGuiMouseButton_Left)
	{
		mCamera.SetTumbleActive(true);
		mbMoveCamera = true;
	}
	if(aData.mlVal&eGuiMouseButton_Right)
	{
		mbMoveLight = true;
	}
	if((aData.mlVal&eGuiMouseButton_WheelUp) || (aData.mlVal&eGuiMouseButton_WheelDown))
	{
		cVector2f vZoom = 0;
		if(aData.mlVal & eGuiMouseButton_WheelUp)
			vZoom=-1;
		if(aData.mlVal & eGuiMouseButton_WheelDown)
			vZoom=1;

		if(vZoom!=0)
		{
			mCamera.Zoom(vZoom,true);
			mCamera.UpdateCamera(0);
		}
	}

	return true;
}

//------------------------------------------------------------------------------------

bool cEditorWindowMaterialEditor::OnViewportMouseUp(const cGuiMessageData& aData)
{
	if(aData.mlVal&eGuiMouseButton_Left)
	{
		mCamera.SetTumbleActive(false);
		mbMoveCamera = false;
	}
	if(aData.mlVal&eGuiMouseButton_Right)
	{
		mbMoveLight = false;
	}
	return true;
}

//------------------------------------------------------------------------------------

bool cEditorWindowMaterialEditor::MenuCallback(iWidget* apWidget, const cGuiMessageData& aData)
{
	if(apWidget==mpMenuNew)
	{
		Command_New();
	}
	if(apWidget==mpMenuOpen)
	{
		Command_Open();
	}
	else if(apWidget==mpMenuSave)
	{
		Command_Save();
	}
	else if(apWidget==mpMenuSaveAs)
	{
		Command_SaveAs();
	}
	else if(apWidget==mpMenuExit)
	{
		if(mbStandAlone)
			mpEditor->Command_Exit();
		else
			SetActive(false);
	}

	return true;
}
kGuiCallbackDeclaredFuncEnd(cEditorWindowMaterialEditor, MenuCallback);

//------------------------------------------------------------------------------------

bool cEditorWindowMaterialEditor::WindowSpecificInputCallback(iEditorInput* apInput)
{
	cTextureUnitPanel* pPanel = (cTextureUnitPanel*)apInput->GetUserData();
	if(pPanel)
		return pPanel->PanelSpecificInputCallback(apInput);

	/////////////////////////////////////
	// Material type
	if(apInput==mpInpMatType)
	{
		mpMaterial->SetType(mpInpMatType->GetEnumValue(mpInpMatType->GetValue()));
		SetUpUnits();
		SetUpPanels();
		SetUpVars();
	}
	/////////////////////////////////////
	// Depth Test
	else if(apInput==mpInpDepthTest)
	{
		mpMaterial->SetDepthTest(mpInpDepthTest->GetValue());
	}
	/////////////////////////////////////
	// Physics material
	else if(apInput==mpInpPhysicsMat)
	{
		int lItem = mpInpPhysicsMat->GetValue();
		mpMaterial->SetPhysicsMaterial(mpInpPhysicsMat->GetEnumValue(lItem));
	}
	/////////////////////////////////////
	// Blend mode
	else if(apInput==mpInpBlendMode)
	{
		mpMaterial->SetBlendMode(mpInpBlendMode->GetEnumValue(mpInpBlendMode->GetValue()));
	}
	/////////////////////////////////////
	// UV Animation
	else if(apInput==mpInpUVAnimation)
	{
		UpdateUVAnimInputs();
	}
	/////////////////////////////////////
	// UV Animation Type
	else if(apInput==mpInpUVAnimType)
	{
		cMaterialUvAnimation* pAnim = mpMaterial->GetUVAnim(mpInpUVAnimation->GetValue());
		if(pAnim)
			pAnim->mType = (eMaterialUvAnimation)mpInpUVAnimType->GetValue();
		mpMaterial->SetUpdated();
	}
	/////////////////////////////////////
	// UV Animation Axis
	else if(apInput==mpInpUVAnimAxis)
	{
		cMaterialUvAnimation* pAnim = mpMaterial->GetUVAnim(mpInpUVAnimation->GetValue());
		if(pAnim)
			pAnim->mAxis = (eMaterialAnimationAxis)mpInpUVAnimAxis->GetValue();
		mpMaterial->SetUpdated();
	}
	/////////////////////////////////////
	// UV Animation Speed
	else if(apInput==mpInpUVAnimSpeed)
	{
		cMaterialUvAnimation* pAnim = mpMaterial->GetUVAnim(mpInpUVAnimation->GetValue());
		if(pAnim)
			pAnim->mfSpeed = mpInpUVAnimSpeed->GetValue();
		mpMaterial->SetUpdated();
	}
	/////////////////////////////////////
	// UV Animation Amplitude
	else if(apInput==mpInpUVAnimAmp)
	{
		cMaterialUvAnimation* pAnim = mpMaterial->GetUVAnim(mpInpUVAnimation->GetValue());
		if(pAnim)
			pAnim->mfAmp = mpInpUVAnimAmp->GetValue();
		mpMaterial->SetUpdated();
	}
	/////////////////////////////////////
	// Preview Model
	else if(apInput==mpInpPreviewModel)
	{
		if(mpCurrentPreviewEntity)
		{
			mpCurrentPreviewEntity->SetVisible(false);
			mpCurrentPreviewEntity->SetActive(false);
		}

		mpCurrentPreviewEntity = mvPreviewEntities[mpInpPreviewModel->GetValue()];
		mpCurrentPreviewEntity->SetVisible(true);
		mpCurrentPreviewEntity->SetActive(true);
	}
	/////////////////////////////////////
	// Background Type
	else if(apInput==mpInpBGType)
	{
		// CubeMap
		if(mpInpBGType->GetValue()==0)
		{
			Image* pTexture = NULL;
			if(cEditorHelper::LoadTextureResource( eEditorTextureResourceType_CubeMap, cString::To8Char(mpInpBGCubeMap->GetValue()), &pTexture))
			{
				mpMatWorld->SetSkyBox(pTexture,true);
				mpMatWorld->SetSkyBoxColor(cColor(1));
			}
			else
				mpInpBGType->SetValue(1);
		}
		// Flat
		else
		{
			mpMatWorld->SetSkyBox((Image*)NULL, true);
			mpMatWorld->SetSkyBoxColor(mpInpBGColor->GetValue());
		}
	}
	/////////////////////////////////////
	// Background Color
	else if(apInput==mpInpBGColor)
	{
		if(mpInpBGType->GetValue()==1)
			mpMatWorld->SetSkyBoxColor(mpInpBGColor->GetValue());
	}
	/////////////////////////////////////
	// Background CubeMap
	else if(apInput==mpInpBGCubeMap)
	{
		mpMatWorld->SetSkyBoxColor(mpInpBGColor->GetValue());
	}
	/////////////////////////////////////
	// Preview Model Rotate
	else if(apInput==mpInpRotateModel)
	{
		mbRotationEnabled = mpInpRotateModel->GetValue();
	}
	/////////////////////////////////////
	// Fixed Light Color
	else if(apInput==mpInpFixedLightColor)
	{
		mpFixedLight->SetDiffuseColor(mpInpFixedLightColor->GetValue());
	}
	/////////////////////////////////////
	// Movable Light Color
	else if(apInput==mpInpMovableLightColor)
	{
		mpMovableLight->SetDiffuseColor(mpInpMovableLightColor->GetValue());
	}
	else
	{
		/////////////////////////////////////
		// Texture Panel Inputs
		bool bInputInTexturePanels=false;
		for(int i=0;i<(int)mvUnitSwitches.size();++i)
		{
			if(apInput==mvUnitSwitches[i])
			{
				cTextureWrapper* pTexWrapper = mpMaterial->GetTextureWrapper((eMaterialTexture)i);
				pTexWrapper->SetEnabled(mvUnitSwitches[i]->GetValue());
				SetUpPanels();
				bInputInTexturePanels=true;
				break;
			}
		}
		if(bInputInTexturePanels==false)
			mpMaterial->SetUpdated();
	}

	SetPreviewUpdated();

	return true;
}

//------------------------------------------------------------------------------------
