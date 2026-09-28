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

#include "LuxHandObject_Melee.h"

#include "LuxMap.h"
#include "LuxPlayer.h"
#include "LuxPlayerHands.h"
#include "LuxMapHelper.h"
#include "LuxHelpFuncs.h"

#include <tinyxml2.h>
#include "resources/XmlHelper.h"

//////////////////////////////////////////////////////////////////////////
// CONSTRUCTORS
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

cLuxHandObject_Melee::cLuxHandObject_Melee(const tString& asName, cLuxPlayerHands *apHands) : iLuxHandObject(asName, apHands)
{
	mpCollideShape = NULL;
}

cLuxHandObject_Melee::~cLuxHandObject_Melee()
{
}

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// PUBLIC METHODS
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

void cLuxHandObject_Melee::RenderSolid(DebugDraw* apDebugDraw)
{
	/*cCamera *pCam = gpBase->mpPlayer->GetCamera();

	cVector3f vRotation(pCam->GetPitch(), pCam->GetYaw(), pCam->GetRoll());
	cMatrixf mtxHands = cMath::MatrixRotate(vRotation, eEulerRotationOrder_XYZ);
	mtxHands.SetTranslation(pCam->GetPosition());
	
	cBoundingVolume shapeBV =  mpCollideShape->GetBoundingVolume();
	shapeBV.SetTransform(cMath::MatrixMul(mtxHands, shapeBV.GetTransform()));
	
	apDebugDraw->DebugDrawBoxMinMax(shapeBV.GetMin(), shapeBV.GetMax(), cColor(1,1));*/
	(void)apDebugDraw;
}

//-----------------------------------------------------------------------

void cLuxHandObject_Melee::RenderTrans(cRendererCallbackFunctions* apFunctions)
{

}

//-----------------------------------------------------------------------

void cLuxHandObject_Melee::LoadImplementedVars(tinyxml2::XMLElement *apVarsElem)
{
	msHandsAnim_Swing = hpl::GetAttributeString(apVarsElem, "HandsAnim_Swing", "");
	msHandsAnim_SwingCharge = hpl::GetAttributeString(apVarsElem, "HandsAnim_SwingCharge", "");
	msHandsAnim_Charge = hpl::GetAttributeString(apVarsElem, "HandsAnim_Charge", "");
	msSwingSound = hpl::GetAttributeString(apVarsElem, "SwingSound", "");
	msSwingChargeSound = hpl::GetAttributeString(apVarsElem, "SwingChargeSound", "");
	msChargeSound = hpl::GetAttributeString(apVarsElem, "ChargeSound", "");
	msChargeDoneSound = hpl::GetAttributeString(apVarsElem, "ChargeDoneSound", "");
	msHitSound = hpl::GetAttributeString(apVarsElem, "HitSound", "");

	mvCollideShapeSize = hpl::GetAttributeVector3f(apVarsElem, "ColliderSize", cVector3f(1));
	mvCollideShapeOffset = hpl::GetAttributeVector3f(apVarsElem, "ColliderOffset", cVector3f(0));

	mfChargePlayerSpeedMul = hpl::GetAttributeFloat(apVarsElem, "ChargePlayerSpeedMul", 1.0f);
	mfChargeTime = hpl::GetAttributeFloat(apVarsElem, "ChargeTime", 2.0f);
	mfRelativeDamageTime = hpl::GetAttributeFloat(apVarsElem, "RelativeDamageTime", 0.5f);

	mfAttackForce = hpl::GetAttributeFloat(apVarsElem, "AttackForce", 0);
	mfMaxAttackForce = hpl::GetAttributeFloat(apVarsElem, "MaxAttackForce", 0);

	mfAttackMinDamage = hpl::GetAttributeFloat(apVarsElem, "AttackMinDamage", 0);
	mfAttackMaxDamage = hpl::GetAttributeFloat(apVarsElem, "AttackMaxDamage", 0);
	mfAttackHitSpeed = hpl::GetAttributeFloat(apVarsElem, "AttackHitSpeed", 0);

	mlAttackStrength = hpl::GetAttributeInt(apVarsElem, "AttackHitSpeed", 0);
}

//-----------------------------------------------------------------------

void cLuxHandObject_Melee::ImplementedCreateEntity(cLuxMap *apMap)
{
	if(mpCollideShape) return;

	cMatrixf mtxOffset = cMath::MatrixTranslate(mvCollideShapeOffset);
	mpCollideShape = apMap->GetPhysicsWorld()->CreateBoxShape(mvCollideShapeSize, &mtxOffset);
}

//-----------------------------------------------------------------------

void cLuxHandObject_Melee::ImplementedDestroyEntity(cLuxMap *apMap)
{
	if(mpCollideShape==NULL) return;

	apMap->GetPhysicsWorld()->DestroyShape(mpCollideShape);
	mpCollideShape = NULL;
}

//-----------------------------------------------------------------------

void cLuxHandObject_Melee::ImplementedReset()
{
	mpCollideShape = NULL;

}

//-----------------------------------------------------------------------

void cLuxHandObject_Melee::Update(float afTimeStep)
{
	if(mpHands->mlHandObjectState ==1)
	{
		UpdateCharge(afTimeStep);
	}
	else if(mpHands->mlHandObjectState ==2)
	{
		UpdateCheckDamageEvent(afTimeStep);
	}
}

//-----------------------------------------------------------------------

bool cLuxHandObject_Melee::DoAction(eLuxPlayerAction aAction, bool abPressed)
{
	////////////////////////////
	// Attack
	if(aAction == eLuxPlayerAction_Attack)
	{
		if(mpHands->GetState()== eLuxHandsState_Idle)
		{
			if(abPressed)
			{
				Charge();
				return true;
			}
		}
		else if(mpHands->GetState()== eLuxHandsState_HandObject)
		{
			if(abPressed ==false && mpHands->mlHandObjectState != 2)
			{
				mpHands->mbHandObjectAttackDown = false;
				if(mpHands->mlHandObjectState !=0) Swing();
			}
		}
	}
	return false;
}

//-----------------------------------------------------------------------

bool cLuxHandObject_Melee::AnimationIsOver()
{
	///////////////////////
	//Going to charge
	if(mpHands->mlHandObjectState == 0)
	{
		if(mpHands->mbHandObjectAttackDown==false)
		{
			Swing();
		}
		else
		{
			gpBase->mpPlayer->SetInteractionMoveSpeedMul(mfChargePlayerSpeedMul);
			mpHands->mlHandObjectState = 1;
		}

		return false;
	}
	///////////////////////
	//Charging
	else if(mpHands->mlHandObjectState == 1)
	{
		return false;	
	}
	///////////////////////
	//Swing
	else if(mpHands->mlHandObjectState == 2)
	{
		mpHands->mlHandObjectState =0;
		return true;
		
	}
	return false;
}

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// PRIVATE METHODS
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

void cLuxHandObject_Melee::UpdateCharge(float afTimeStep)
{
	if(mpHands->mbHandObjectAttackDown==false) return;

	if(mpHands->mfHandObjectChargeCount < 1)
	{
		mpHands->mfHandObjectChargeCount += afTimeStep * (1.0f/mfChargeTime);
		if(mpHands->mfHandObjectChargeCount >= 1.0f)
		{
			mpHands->mfHandObjectChargeCount = 1;
			gpBase->mpPlayer->FadeAspectMulTo(0.9f, 4);
			gpBase->mpPlayer->FadeFOVMulTo(0.95f, 2);
			gpBase->mpHelpFuncs->PlayGuiSoundData(msChargeDoneSound,eSoundEntryType_World);
		}
	}
}

//-----------------------------------------------------------------------

void cLuxHandObject_Melee::UpdateCheckDamageEvent(float afTimeStep)
{
	if(mpHands->CheckAnimationEvent(mfRelativeDamageTime))
	{
		cCamera *pCam = gpBase->mpPlayer->GetCamera();

		cVector3f vRotation(pCam->GetPitch(), pCam->GetYaw(), pCam->GetRoll());
		cMatrixf mtxHands = cMath::MatrixRotate(vRotation, eEulerRotationOrder_XYZ);
		mtxHands.SetTranslation(pCam->GetPosition());

		float fDamageMul = mpHands->mfHandObjectChargeCount>=1 ? 2.0f : 1.0f;

		bool bHit = gpBase->mpMapHelper->ShapeDamage(	mpCollideShape, mtxHands, pCam->GetPosition(),
											mfAttackMinDamage*fDamageMul, mfAttackMaxDamage*fDamageMul,
											mfAttackForce, mfMaxAttackForce,
											mlAttackStrength, mfAttackHitSpeed,
											eLuxDamageType_BloodSplat, eLuxWeaponHitType_Sword,
											true,false, true, false, NULL);

		if(bHit)
		{
			gpBase->mpHelpFuncs->PlayGuiSoundData(msHitSound,eSoundEntryType_World);
		}
	}
}

//-----------------------------------------------------------------------

void cLuxHandObject_Melee::Swing()
{
	//Reset any charing stuff
	gpBase->mpPlayer->SetInteractionMoveSpeedMul(1.0f);
	gpBase->mpPlayer->FadeAspectMulTo(1.0f, 7.0f);
	gpBase->mpPlayer->FadeFOVMulTo(1.0f, 3.5f);

	mpHands->mlHandObjectState=2;
	if(mpHands->mfHandObjectChargeCount >= 1)
	{
		mpHands->PlayAnim(msHandsAnim_SwingCharge, false);
		gpBase->mpHelpFuncs->PlayGuiSoundData(msSwingChargeSound,eSoundEntryType_World);
	}
	else
	{
		mpHands->PlayAnim(msHandsAnim_Swing, false);
		gpBase->mpHelpFuncs->PlayGuiSoundData(msSwingSound,eSoundEntryType_World);
	}
	
}

//-----------------------------------------------------------------------

void cLuxHandObject_Melee::Charge()
{
	mpHands->mbHandObjectAttackDown = true;
	mpHands->mfHandObjectChargeCount =0;
	mpHands->mlHandObjectState =0;
	
	mpHands->PlayAnim(msHandsAnim_Charge, false);
	gpBase->mpHelpFuncs->PlayGuiSoundData(msChargeSound,eSoundEntryType_World);
}

//-----------------------------------------------------------------------
