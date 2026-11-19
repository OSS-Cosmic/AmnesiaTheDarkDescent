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

#include "LuxPlayerHands.h"

#include "LuxPlayer.h"
#include "LuxMapHandler.h"
#include "LuxMap.h"

#include "LuxHandObject_Melee.h"
#include "LuxHandObject_LightSource.h"

#include <tinyxml2.h>
#include "resources/XmlHelper.h"


/////////////////////////////////////////////////////////////////////////
// CONSTRUCTORS
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

cLuxPlayerHands::cLuxPlayerHands(cLuxPlayer *apPlayer) : iLuxPlayerHelper(apPlayer, "LuxPlayerHands")
{
	////////////////////
	//Init data

	//Add special entity type for loading the hands.
	gpBase->mpEngine->GetResources()->AddEntityLoader( hplNew( cLuxPlayerHandsLoader, ("PlayerHands", this) ) );
	
	//Add entity loader for hand objects
	mpHandObjectLoader = hplNew( cLuxHandObjectLoader, ("HandObject") );
	gpBase->mpEngine->GetResources()->AddEntityLoader(mpHandObjectLoader);

	mlMaxCamRotations = gpBase->mpGameCfg->GetInt("Player_General", "HandsRotationSmoothNum", 0);
	mfPosAddMul = gpBase->mpGameCfg->GetFloat("Player_General", "HandsPosAddMul", 0);

	// Time constant equal to the per tick rotation smoothing (the weighted
	// average of the last HandsRotationSmoothNum tick rotations).
	mfSwayTimeConst = 0;
	if(mlMaxCamRotations > 1)
	{
		float fN = (float)mlMaxCamRotations;
		float fWeightSum = 0;
		float fAgeSum = 0;
		for(size_t i = 1; i <= mlMaxCamRotations; ++i)
		{
			float fWeight = (float)i / fN;
			fWeight *= fWeight;
			fWeightSum += fWeight;
			fAgeSum += fWeight * (fN - (float)i);
		}
		mfSwayTimeConst = (fAgeSum / fWeightSum) * gpBase->mpEngine->GetStepSize();
	}
	mvSwayRotation = 0;
	mbSwayValid = false;

	mpHandsMesh = NULL;

}

cLuxPlayerHands::~cLuxPlayerHands()
{
	if(mpHandsMesh) 
	{
		gpBase->mpEngine->GetResources()->GetMeshManager()->Destroy(mpHandsMesh);
	}

	for(size_t i=0; i<mvHandAnimations.size(); ++i)
	{
		gpBase->mpEngine->GetResources()->GetAnimationManager()->Destroy(mvHandAnimations[i]);
	}
	
	STLDeleteAll(mvHandObjects);

	//Resources delete hand object loader by itself!
}

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// ENTITY LOADER
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

cLuxPlayerHandsLoader::cLuxPlayerHandsLoader(const tString& asName, cLuxPlayerHands *apPlayerHands) : cEntityLoader_Object(asName)
{
	mpPlayerHands = apPlayerHands;
}

//-----------------------------------------------------------------------

void cLuxPlayerHandsLoader::BeforeLoad(tinyxml2::XMLElement *apRootElem, const cMatrixf &a_mtxTransform,cWorld *apWorld, cResourceVarsObject *apInstanceVars)
{
	
}

//-----------------------------------------------------------------------

void cLuxPlayerHandsLoader::AfterLoad(tinyxml2::XMLElement *apRootElem, const cMatrixf &a_mtxTransform,cWorld *apWorld, cResourceVarsObject *apInstanceVars)
{
	mpPlayerHands->mpHandsEntity = mpEntity;
	if(mpEntity) mpEntity->SetRenderFlagBit(eRenderableFlag_ShadowCaster,false);
	
	if(mpMesh && mpPlayerHands->mpHandsMesh==NULL)
	{
		mpMesh->AddReference();
		mpPlayerHands->mpHandsMesh = mpMesh;
	}

	if(mpPlayerHands->mvHandAnimations.empty() && mpEntity)
	{
		for(int i=0; i<mpEntity->GetAnimationStateNum(); ++i)
		{
			cAnimationState *pAnimState = mpEntity->GetAnimationState(i);
            cAnimation *pAnim = pAnimState->GetAnimation();
			pAnim->AddReference();
			mpPlayerHands->mvHandAnimations.push_back(pAnim);
		}

	}
}

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// PUBLIC METHODS
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

void cLuxPlayerHands::OnStart()
{
	
}

void cLuxPlayerHands::Reset()
{
	////////////////////
	//Init variables
	mpCurrentHandObject = NULL;
	mpHandsEntity = NULL;

	mHandState = eLuxHandsState_Disabled;
	mfHandObjectAlpha =0;
 
	msCurrentAnim = "";

	mlstCamRotations.clear();

	for(size_t i=0; i<mvHandObjects.size(); ++i)
	{
		iLuxHandObject *pHandObject = mvHandObjects[i];
		pHandObject->Reset();
	}

	////////////////////
	//Init hand object variables
	ResetHandObjectVars();
}

//-----------------------------------------------------------------------

void cLuxPlayerHands::Update(float afTimeStep)
{
	for(int i=0; i<mpHandsEntity->GetAnimationStateNum(); ++i)
	{
		cAnimationState *pAnim = mpHandsEntity->GetAnimationState(i);
	}
	
	////////////////////////////
	// Disabled
	if(mHandState == eLuxHandsState_Disabled)
	{
		return;
	}
	////////////////////////////
	// Holster
	else if(mHandState == eLuxHandsState_Holster)
	{
		if(AnimOver())
		{
			////////////////////
			//Draw up new object
			if(mpCurrentHandObject)
			{
				cLuxMap *pMap = gpBase->mpMapHandler->GetCurrentMap();

				CreateAndAttachHandObject(pMap, mpCurrentHandObject);
				PlayAnim(mpCurrentHandObject->GetAnimDraw(),false);
				
				mHandState = eLuxHandsState_Draw;
			}
			////////////////////
			//Disable hands
			else
			{
				HideAllHandObjects();
				mpHandsEntity->SetVisible(false);
				mHandState = eLuxHandsState_Disabled;
			}
		}
	}	
	////////////////////////////
	// Draw
	else if(mHandState == eLuxHandsState_Draw)
	{
		if(AnimOver())
		{
			PlayAnim(mpCurrentHandObject->GetAnimIdle(),true);
			mHandState = eLuxHandsState_Idle;
		}

	}		
	////////////////////////////
	// Idle
	else if(mHandState == eLuxHandsState_Idle)
	{

	}
	////////////////////////////
	// Hand Object Specific
	else if(mHandState == eLuxHandsState_HandObject)
	{
		if(mpCurrentHandObject)
		{
			//////////////////////////////
			//Animation is over check
			if(AnimOver())
			{
				if(mpCurrentHandObject->AnimationIsOver())
				{
					PlayAnim(mpCurrentHandObject->GetAnimIdle(),true);
					mHandState = eLuxHandsState_Idle;
					return;
				}
			}
		}
	}

	////////////////////
	// Update Hand Object (need to do like this, since current might not be drawn yet!)
	for(size_t i=0; i<mvHandObjects.size(); ++i)
	{
		iLuxHandObject *pHandObject = mvHandObjects[i];
		if(pHandObject->GetMeshEntity() && pHandObject->GetMeshEntity()->IsActive())
		{
			pHandObject->Update(afTimeStep);
		}
	}
}


//-----------------------------------------------------------------------

void cLuxPlayerHands::PostUpdate(float afTimeStep)
{
	UpdatePlayerHandsPos(afTimeStep);
}

//-----------------------------------------------------------------------

static float WrapAngleDelta(float afX)
{
	while(afX > kPif) afX -= k2Pif;
	while(afX < -kPif) afX += k2Pif;
	return afX;
}

void cLuxPlayerHands::OnDraw(float afFrameTime)
{
	if(mpHandsEntity==NULL || cRenderInterpolation::IsEnabled()==false) return;

	cCamera *pCam = mpPlayer->GetCamera();
	cVector3f vTarget = pCam->GetRenderPitchYawRoll();

	float fDelta = afFrameTime;
	if(fDelta < 0) fDelta = 0;
	if(fDelta > 0.25f) fDelta = 0.25f;

	if(mbSwayValid==false || mfSwayTimeConst <= 0)
	{
		mvSwayRotation = vTarget;
		mbSwayValid = true;
	}
	else
	{
		float fFollow = 1.0f - expf(-fDelta / mfSwayTimeConst);
		cVector3f vDiff(WrapAngleDelta(vTarget.x - mvSwayRotation.x),
						WrapAngleDelta(vTarget.y - mvSwayRotation.y),
						WrapAngleDelta(vTarget.z - mvSwayRotation.z));

		if(cMath::Abs(vDiff.x) > kPi2f || cMath::Abs(vDiff.y) > kPi2f || cMath::Abs(vDiff.z) > kPi2f)
			mvSwayRotation = vTarget;
		else
			mvSwayRotation = vTarget - vDiff * (1.0f - fFollow);
	}

	cMatrixf mtxCam = cMath::MatrixRotate(vTarget, eEulerRotationOrder_ZXY);
	cMatrixf mtxSway = cMath::MatrixRotate(mvSwayRotation, eEulerRotationOrder_ZXY);
	pCam->SetViewModelOffset(cMath::MatrixMul(mtxSway, mtxCam.GetTranspose()));
}

//-----------------------------------------------------------------------

void cLuxPlayerHands::OnMapEnter(cLuxMap *apMap)
{
	//////////////////////////
	// If a hand object is loaded, create it.
	if(mpCurrentHandObject)
	{
		CreateAndAttachHandObject(apMap, mpCurrentHandObject);
		
		mpHandsEntity->SetVisible(true);
		mpHandsEntity->PlayName(mpCurrentHandObject->GetAnimIdle(),true, true);
	}

	mlstCamRotations.clear();
	mbSwayValid = false;
}

//-----------------------------------------------------------------------

void cLuxPlayerHands::OnMapLeave(cLuxMap *apMap)
{
	mpHandsEntity = NULL;
	for(size_t i=0; i<mvHandObjects.size(); ++i)
	{
		iLuxHandObject *pHandObject = mvHandObjects[i];
		pHandObject->ResetEntityContainers();
	}
}

//-----------------------------------------------------------------------

void cLuxPlayerHands::CreateWorldEntities(cLuxMap *apMap)
{
	//////////////////////////
	// Create the hand entity
	CreateHandEntity(apMap);
}

void cLuxPlayerHands::DestroyWorldEntities(cLuxMap *apMap)
{
	/////////////////////////
	// Destroy the hand entity
	DestroyHandEntity(apMap);

	///////////////////////////////////
	// Destroy all hand object entities
	for(size_t i=0; i<mvHandObjects.size(); ++i)
	{
		iLuxHandObject *pHandObject = mvHandObjects[i];
		pHandObject->DestroyEntity(apMap);
	}
}

//-----------------------------------------------------------------------

void cLuxPlayerHands::RenderSolid(DebugDraw* apDebugDraw)
{
	if(mpCurrentHandObject) mpCurrentHandObject->RenderSolid(apDebugDraw);
}

void cLuxPlayerHands::RenderTrans(cRendererCallbackFunctions* apFunctions)
{
	if(mpCurrentHandObject) mpCurrentHandObject->RenderTrans(apFunctions);
}

//-----------------------------------------------------------------------

void cLuxPlayerHands::PlayAnim(const tString& asAnim, bool abLoop)
{
	mpHandsEntity->PlayFadeToName(asAnim, abLoop, 0.2f);
	msCurrentAnim = asAnim;
}

bool cLuxPlayerHands::AnimOver()
{
	return mpHandsEntity->AnimationIsOver(msCurrentAnim);
}

//-----------------------------------------------------------------------

bool cLuxPlayerHands::CheckAnimationEvent(float afRelTime)
{
	cAnimationState *pAnim = mpHandsEntity->GetAnimationStateFromName(msCurrentAnim);
	if(pAnim == NULL) return false;

	float fPrevPos = pAnim->GetPreviousTimePosition() / pAnim->GetLength();
	float fPos = pAnim->GetRelativeTimePosition();
    
	return afRelTime > fPrevPos && afRelTime <= fPos;
}

//-----------------------------------------------------------------------

void cLuxPlayerHands::DoAction(eLuxPlayerAction aAction, bool abPressed)
{
	if(mpCurrentHandObject==NULL) return;
	if(mHandState != eLuxHandsState_Idle && mHandState != eLuxHandsState_HandObject) return;

	if(mpCurrentHandObject->DoAction(aAction, abPressed))
	{
		mHandState = eLuxHandsState_HandObject;
	}
}

//-----------------------------------------------------------------------

void cLuxPlayerHands::SetActiveHandObject(const tString& asName)
{
	if(mpHandsEntity==NULL) return; //Make sure the 

	if(asName != "")
	{
		iLuxHandObject* pObject = GetHandObject(asName);
		if(pObject) SetCurrentHandObject(pObject);
	}
	else
	{
		SetCurrentHandObject(NULL);
	}
}

//-----------------------------------------------------------------------

iLuxHandObject* cLuxPlayerHands::GetHandObject(const tString& asName)
{
	//////////////////////////////////////
	// Search in already loaded objects
	for(size_t i=0; i<mvHandObjects.size(); ++i)
	{
		iLuxHandObject *apObject = mvHandObjects[i];
        if(asName == apObject->GetName()) return apObject;
	}

	//////////////////////////////////////
	// If not found try loading
	return LoadHandObject(asName);
}


//-----------------------------------------------------------------------

void cLuxPlayerHands::SetCurrentHandObject(iLuxHandObject *apObject)
{
	if(mpCurrentHandObject == apObject) return;

	cLuxMap *pMap = gpBase->mpMapHandler->GetCurrentMap();

	ResetHandObjectVars();
	
	////////////////////////
	// Previous hand set
    if(mpCurrentHandObject)
	{
		PlayAnim(mpCurrentHandObject->GetAnimHolster(),false);
		
		mHandState = eLuxHandsState_Holster;
	}
	////////////////////////
	// Draw directly
	else if(apObject)
	{
		CreateAndAttachHandObject(pMap, apObject);

		// Start the draw animation at full weight (no fade from the previous pose) and pose
		// the hands right away, otherwise they are shown in the old pose for a frame and jerk.
		mpHandsEntity->PlayName(apObject->GetAnimDraw(), false, true);
		msCurrentAnim = apObject->GetAnimDraw();
		mpHandsEntity->SetVisible(true);
		mpHandsEntity->UpdateLogic(0);
		mpHandsEntity->SnapRenderInterpolation();

		mHandState = eLuxHandsState_Draw;
	}
	mpCurrentHandObject = apObject;
}


//-----------------------------------------------------------------------

void cLuxPlayerHands::SetState(eLuxHandsState aState)
{ 
	mHandState = aState; 
}


//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// PRIVATE METHODS
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

void cLuxPlayerHands::ResetHandObjectVars()
{
	mfHandObjectChargeCount =0;
	mlHandObjectState =0;
	mbHandObjectAttackDown = false;
	mbHandObjectInteractDown = false;

	//NOTE: Do note reset the alpha here!
}

//-----------------------------------------------------------------------

void cLuxPlayerHands::CreateHandEntity(cLuxMap *apMap)
{
	if(mpHandsEntity) return;

	apMap->GetWorld()->CreateEntity("PlayerHands", cMatrixf::Identity, "models/player/hands/hands.ent");
	mpHandsEntity->SetVisible(false);
	mpHandsEntity->Stop();
	mpHandsEntity->SetRenderFlagBit(eRenderableFlag_ShadowCaster,false);
	mpHandsEntity->SetRenderFlagBit(eRenderableFlag_VisibleInReflection,false);

	//View model: the hands (and the lantern and lights attached to them) are drawn glued to the
	//camera as it is rendered, so they follow per frame mouse look without lagging behind.
	mpHandsEntity->SetRenderInterpCamera(mpPlayer->GetCamera());
}

void cLuxPlayerHands::DestroyHandEntity(cLuxMap *apMap)
{
	if(mpHandsEntity)
	{
		apMap->GetWorld()->DestroyMeshEntity(mpHandsEntity);
		mpHandsEntity = NULL;
	}
}

//-----------------------------------------------------------------------

void cLuxPlayerHands::CreateAndAttachHandObject(cLuxMap *apMap, iLuxHandObject *apHandObject)
{
	HideAllHandObjects();

	///////////////////////
	//Load object
	mpHandObjectLoader->SetHandObject(apHandObject);
	apHandObject->CreateEntity(apMap);

	///////////////////////
	//Attach object
	cMeshEntity *pMeshEntity = apHandObject->GetMeshEntity();
	if(pMeshEntity)
	{
		cBoneState *pBone = mpHandsEntity->GetBoneStateFromName(apHandObject->GetAttachBoneName());
		if(pBone==NULL)
		{
			Error("Could not find bone '%s' in player hands model!\n", apHandObject->GetAttachBoneName().c_str());
			return;
		}
				
		pBone->AddEntity(pMeshEntity);
	}
	apHandObject->SetSetEntitiesVisible(true);

	mfHandObjectAlpha =0;
}

//-----------------------------------------------------------------------

void cLuxPlayerHands::HideAllHandObjects()
{
	for(size_t i=0; i<mvHandObjects.size(); ++i)
	{
		iLuxHandObject *pHandObject = mvHandObjects[i];
		pHandObject->SetSetEntitiesVisible(false);
	}
}

//-----------------------------------------------------------------------

void cLuxPlayerHands::UpdatePlayerHandsPos(float afTimeStep)
{
	if(mpHandsEntity==NULL) return;

	cCamera *pCam = mpPlayer->GetCamera();

	cVector3f vRotation(pCam->GetPitch(), pCam->GetYaw(), pCam->GetRoll());
	cVector3f vFinalRot = vRotation;

	if(cRenderInterpolation::IsEnabled())
	{
		////////////////////////////////////////
		// Sway is added every frame (OnDraw), the hands follow the camera exactly here.
		mlstCamRotations.clear();
	}
	else
	{
		pCam->SetViewModelOffset(cMatrixf::Identity);
		mbSwayValid = false;

		mlstCamRotations.push_back(vRotation);
		if(mlstCamRotations.size() > mlMaxCamRotations) mlstCamRotations.pop_front();

		////////////////////////////////////////
		// Iterate all of the saved rotations and calculate a new
		vFinalRot = 0;
		float fWeightTotal =0;
		float fCount = 1.0f;

		tVector3fListIt it = mlstCamRotations.begin();
		for(; it != mlstCamRotations.end(); ++it)
		{
			float fWeight = fCount / ((float)mlstCamRotations.size());
			fWeight *= fWeight; //Exp increase...
			++fCount;

			vFinalRot += *it * fWeight;
			fWeightTotal += fWeight;
		}

		if(fWeightTotal > 0)	vFinalRot = vFinalRot / fWeightTotal;
		else					vFinalRot = vRotation;
	}
	
	////////////////////////////////////////
	// Set hand matrix
	cMatrixf mtxHands = cMath::MatrixRotate(vFinalRot, eEulerRotationOrder_ZXY);

	cVector3f vHandPosAdd =0;
	vHandPosAdd -= mpPlayer->GetHeadPosAdd(eLuxHeadPosAdd_Bob);
	vHandPosAdd -= mpPlayer->GetHeadPosAdd(eLuxHeadPosAdd_ScreenShake);
	vHandPosAdd -= mpPlayer->GetHeadPosAdd(eLuxHeadPosAdd_Hurt);
	///vHandPosAdd -= mpPlayer->GetHeadPosAdd(eLuxHeadPosAdd_Lean);

	cVector3f vRight = pCam->GetRight();
	cVector3f vUp = pCam->GetUp();
	cVector3f vFwd = pCam->GetForward();

	vHandPosAdd = vRight * vHandPosAdd.x + vUp * vHandPosAdd.y + vFwd * vHandPosAdd.z;

	mtxHands.SetTranslation(pCam->GetPosition() - vHandPosAdd *mfPosAddMul);

    mpHandsEntity->SetMatrix(mtxHands);
}

//-----------------------------------------------------------------------

iLuxHandObject* cLuxPlayerHands::LoadHandObject(const tString& asName)
{
	/////////////////////
	// Init variables
	cResources *pResources = gpBase->mpEngine->GetResources();

	/////////////////////
	// Get file name
	tString sFileName = cString::SetFileExt(asName, "ho");
	tString sFile = "/models/hand_objects/"+asName+"/"+sFileName;

	// Some mods put hand objects in models/hand_object (no 's'). Files are found by name and
	// closest folder match, so the base game's hand_objects version would win; prefer the mod's.
	{
		tString sAltFile = "/models/hand_object/"+asName+"/"+sFileName;
		const tWString& sAltPath = pResources->GetFileSearcher()->GetFilePath(sAltFile);
		if(sAltPath != _W(""))
		{
			tString sLowAltPath = cString::ToLowerCase(cString::To8Char(cString::ReplaceCharToW(sAltPath, _W("\\"), _W("/"))));
			tString sLowWanted = cString::ToLowerCase(sAltFile);
			if(	sLowAltPath.size() >= sLowWanted.size() &&
				sLowAltPath.compare(sLowAltPath.size() - sLowWanted.size(), sLowWanted.size(), sLowWanted) == 0)
			{
				Log("Hand object '%s' found in 'models/hand_object' instead of 'models/hand_objects', using that one.\n", asName.c_str());
				sFile = sAltFile;
			}
		}
	}

	/////////////////////
	// Load XML document
	tinyxml2::XMLElement *pDoc = pResources->LoadXmlDocument(sFile);
	if(pDoc==NULL)
	{
		Error("Could not load hand object file: '%s'\n", sFile.c_str());
		return NULL;
	}

	// Main element
	tinyxml2::XMLElement *pMainElem = pDoc->FirstChildElement("Main");
	if(pMainElem == NULL){
		Error("No main element found in hand object file '%s'\n", sFile.c_str());
		pResources->DestroyXmlDocument(pDoc);
		return NULL;
	}

	// Settings element
	tinyxml2::XMLElement *pSettingsElem = pDoc->FirstChildElement("Settings");
	if(pSettingsElem == NULL){
		Error("No settings element found in hand object file '%s'\n", sFile.c_str());
		pResources->DestroyXmlDocument(pDoc);
		return NULL;
	}


	/////////////////////
	// Create the object

	//Get the type
	eLuxHandObjectType aType = ToHandObjectType(hpl::GetAttributeString(pMainElem, "Type",""));
	if(aType == eLuxHandObjectType_LastEnum)
	{
		pResources->DestroyXmlDocument(pDoc);
		return NULL;
	}

	//Create object and load data
	iLuxHandObject *pObject = CreateObjectFromType(asName,aType);
	if(aType == eLuxHandObjectType_LastEnum)
	{
		pResources->DestroyXmlDocument(pDoc);
		return NULL;
	}

	//Load the main data (model file name, etc)
	if(pObject->LoadMainData(pMainElem)==false)
	{
		pResources->DestroyXmlDocument(pDoc);
		return NULL;	
	}

	//Load the settings
	pObject->LoadSettings(pSettingsElem);


	/////////////////////
	// Add object

	mvHandObjects.push_back(pObject);

	return pObject;
}

//-----------------------------------------------------------------------

iLuxHandObject* cLuxPlayerHands::CreateObjectFromType(const tString& asName, eLuxHandObjectType aType)
{
	switch(aType)
	{
	case eLuxHandObjectType_Melee:			return hplNew(cLuxHandObject_Melee, (asName, this));
	//case eLuxHandObjectType_Ranged:			return NULL;
	case eLuxHandObjectType_LightSource:	return hplNew(cLuxHandObject_LightSource, (asName, this));
	}

	Error("HandObject '%s' of type %d could not be created! Type is not implemented in code!",asName.c_str(), aType);

	return NULL;
}

//-----------------------------------------------------------------------

eLuxHandObjectType cLuxPlayerHands::ToHandObjectType(const tString& asType)
{
	tString sLowType = cString::ToLowerCase(asType);

	if(sLowType == "melee") return eLuxHandObjectType_Melee;
	if(sLowType == "ranged") return eLuxHandObjectType_Ranged;
	if(sLowType == "lightsource") return eLuxHandObjectType_LightSource;

	Error("Hand object of type '%s' does not exist!", asType.c_str());
	return eLuxHandObjectType_LastEnum;
}

//-----------------------------------------------------------------------

