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

#include "LuxHandObject.h"

#include "LuxMap.h"

#include <tinyxml2.h>
#include "resources/XmlHelper.h"

//////////////////////////////////////////////////////////////////////////
// CONSTRUCTORS
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

iLuxHandObject::iLuxHandObject(const tString& asName, cLuxPlayerHands *apHands)
{
	msName = asName;

	mpHands = apHands;

	m_mtxOffset = cMatrixf::Identity;
	mpMeshEntity = NULL;
	mpMesh = NULL;
}

iLuxHandObject::~iLuxHandObject()
{
	if(mpMesh) 
	{
		gpBase->mpEngine->GetResources()->GetMeshManager()->Destroy(mpMesh);
	}
}

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// LOADER
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

cLuxHandObjectLoader::cLuxHandObjectLoader(const tString& asName) : cEntityLoader_Object(asName)
{
	mpHandObject = NULL;
}

//-----------------------------------------------------------------------

void cLuxHandObjectLoader::BeforeLoad(tinyxml2::XMLElement *apRootElem, const cMatrixf &a_mtxTransform,cWorld *apWorld, cResourceVarsObject *apInstanceVars)
{

}

void cLuxHandObjectLoader::AfterLoad(tinyxml2::XMLElement *apRootElem, const cMatrixf &a_mtxTransform,cWorld *apWorld, cResourceVarsObject *apInstanceVars)
{
	///////////////////////////////
	// Load mesh

	//If hand object has not saved mesh, save it and increase user count!
	if(mpHandObject->mpMesh==NULL)
	{
		mpMesh->AddReference();
		mpHandObject->mpMesh = mpMesh;
	}

	mpHandObject->mpMeshEntity = mpEntity;


	///////////////////////////////
	// Load light, billboard and particles
	mpHandObject->mvBillboards = mvBillboards;
	mpHandObject->mvParticleSystems = mvParticleSystems;
	mpHandObject->mvLights = mvLights;

	if(mpEntity)
	{
		cMatrixf mtxInvModel = cMath::MatrixInverse(mpEntity->GetLocalMatrix());

		for(size_t i=0; i<mvBillboards.size(); ++i)
		{
			cBillboard *pBB = mvBillboards[i];
			cMatrixf mtxLocal = cMath::MatrixMul(mtxInvModel, pBB->GetLocalMatrix());
			pBB->SetMatrix(mtxLocal);

			mpEntity->AddChild(pBB);
		}

		for(size_t i=0; i<mvLights.size(); ++i)
		{
			iLight *pLight = mvLights[i];
			cMatrixf mtxLocal = cMath::MatrixMul(mtxInvModel, pLight->GetLocalMatrix());
			pLight->SetMatrix(mtxLocal);

			mpEntity->AddChild(pLight);
		}

		mpEntity->SetUpdateBonesWhenCulled(true);
	}
}

//-----------------------------------------------------------------------


//////////////////////////////////////////////////////////////////////////
// PUBLIC METHODS
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

bool iLuxHandObject::LoadMainData(tinyxml2::XMLElement *apMainElem)
{
	msModelFile = hpl::GetAttributeString(apMainElem, "Model");

	return true;
}

//-----------------------------------------------------------------------

void iLuxHandObject::LoadSettings(tinyxml2::XMLElement *apVarsElem)
{
	/////////////////////////////
	//Load base settings 
	
	//Offset matrix
	cVector3f vOffsetScale = hpl::GetAttributeVector3f(apVarsElem, "OffsetScale",1);
	cVector3f vOffsetRotation =cMath::Vector3ToRad(hpl::GetAttributeVector3f(apVarsElem, "OffsetRotation",0));
	cVector3f vOffsetPos = hpl::GetAttributeVector3f(apVarsElem, "OffsetPosition",0);

	m_mtxOffset = cMath::MatrixMul(cMath::MatrixRotate(vOffsetRotation,eEulerRotationOrder_XYZ), cMath::MatrixScale(vOffsetScale));
	m_mtxOffset.SetTranslation(vOffsetPos);

	//Animations
	msHandsAnim_Idle =		hpl::GetAttributeString(apVarsElem, "HandsAnim_Idle", "");
	msHandsAnim_Draw =		hpl::GetAttributeString(apVarsElem, "HandsAnim_Draw", "");
	msHandsAnim_Holster =	hpl::GetAttributeString(apVarsElem, "HandsAnim_Holster", "");

	//Bone that object attaches to
	msAttachBoneName =	hpl::GetAttributeString(apVarsElem, "AttachBoneName", "attachpoint");
	

	/////////////////////////////
	//Load implemented vars
	LoadImplementedVars(apVarsElem);
}

//-----------------------------------------------------------------------

void iLuxHandObject::CreateEntity(cLuxMap *apMap)
{
	if(mpMeshEntity) return;

	///////////////////////
	// Load the entity
	tString sFile = "models/player/"+msName+"/"+ msModelFile;
	apMap->GetWorld()->CreateEntity("PlayerHands", cMatrixf::Identity, sFile);
	
	///////////////////////
	// Set up the mesh
	if(mpMeshEntity)
	{
		mpMeshEntity->SetMatrix(m_mtxOffset);
		mpMeshEntity->SetRenderFlagBit(eRenderableFlag_ShadowCaster,false);
		mpMeshEntity->SetRenderFlagBit(eRenderableFlag_VisibleInReflection,false);
		mpMeshEntity->SetUpdateBonesWhenCulled(true);
	}

	ImplementedCreateEntity(apMap);
}

//-----------------------------------------------------------------------

void iLuxHandObject::DestroyEntity(cLuxMap *apMap)
{
	cWorld *pWorld = apMap->GetWorld();
	if(mpMeshEntity)
	{
		pWorld->DestroyMeshEntity(mpMeshEntity);
		mpMeshEntity = NULL;
	}

	for(size_t i=0; i<mvBillboards.size(); ++i)
	{
		pWorld->DestroyBillboard(mvBillboards[i]);
	}
	mvBillboards.clear();

	for(size_t i=0; i<mvParticleSystems.size(); ++i)
	{
		pWorld->DestroyParticleSystem(mvParticleSystems[i]);
	}
	mvParticleSystems.clear();

	for(size_t i=0; i<mvLights.size(); ++i)
	{
		pWorld->DestroyLight(mvLights[i]);
	}
	mvLights.clear();

	ImplementedDestroyEntity(apMap);
}

//-----------------------------------------------------------------------

void iLuxHandObject::Reset()
{
	ResetEntityContainers();

	if(mpMesh)
	{
		gpBase->mpEngine->GetResources()->GetMeshManager()->Destroy(mpMesh);
		mpMesh = NULL;
	}

	ImplementedReset();
}

//-----------------------------------------------------------------------

void iLuxHandObject::ResetEntityContainers()
{
	mpMeshEntity = NULL;
	mvBillboards.clear();
	mvParticleSystems.clear();
	mvLights.clear();
}

void iLuxHandObject::SetSetEntitiesVisible(bool abVisible)
{
	if(mpMeshEntity)
	{
		mpMeshEntity->SetVisible(abVisible);
		mpMeshEntity->SetActive(abVisible);
	}
	
	for(size_t i=0; i<mvBillboards.size(); ++i)
	{
		mvBillboards[i]->SetVisible(abVisible);
	}
	
	for(size_t i=0; i<mvParticleSystems.size(); ++i)
	{
		mvParticleSystems[i]->SetVisible(abVisible);
		mvParticleSystems[i]->SetActive(abVisible);
	}
	
	for(size_t i=0; i<mvLights.size(); ++i)
	{
		mvLights[i]->SetVisible(abVisible);
		mvLights[i]->SetActive(abVisible);
	}
}

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// PRIVATE METHODS
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------


//-----------------------------------------------------------------------
