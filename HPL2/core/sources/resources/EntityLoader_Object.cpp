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

#include "resources/EntityLoader_Object.h"

#include "system/String.h"
#include "scene/World.h"
#include "system/LowLevelSystem.h"


#include "physics/PhysicsWorld.h"
#include "physics/PhysicsBody.h"
#include "physics/PhysicsJoint.h"
#include "physics/PhysicsController.h"
#include "physics/CollideShape.h"
#include "physics/PhysicsJointBall.h"
#include "physics/PhysicsJointHinge.h"
#include "physics/PhysicsJointScrew.h"
#include "physics/PhysicsJointSlider.h"

#include "math/Math.h"

#include "resources/MeshManager.h"
#include "resources/MaterialManager.h"
#include "resources/AnimationManager.h"
#include "resources/MeshLoaderHandler.h"
#include "resources/FileSearcher.h"
#include "resources/EngineFileLoading.h"

#include <map>
#include <tinyxml2.h>
#include "resources/XmlHelper.h"

#include "graphics/Mesh.h"
#include "graphics/SubMesh.h"
#include "graphics/Animation.h"
#include "graphics/BoneState.h"
#include "scene/AnimationState.h"
#include "scene/MeshEntity.h"
#include "scene/Node3D.h"
#include "scene/SoundEntity.h"
#include "scene/BillBoard.h"
#include "scene/ParticleSystem.h"

#include "scene/Light.h"

#include "haptic/Haptic.h"
#include "haptic/LowLevelHaptic.h"
#include "haptic/HapticShape.h"

namespace hpl {

	//////////////////////////////////////////////////////////////////////////
	// CONSTRUCTORS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------
	
	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PUBLIC METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	typedef std::multimap<int,iCollideShape*> tLoaderCollideShapeMap;
	typedef tLoaderCollideShapeMap::iterator tLoaderCollideShapeMapIt;

	typedef std::multimap<int,iPhysicsBody*> tLoaderPhysicsBodyMap;
	typedef tLoaderPhysicsBodyMap::iterator tLoaderPhysicsBodyMapIt;

	//-----------------------------------------------------------------------

	static iCollideShape* GetBodyShape(tinyxml2::XMLElement *apBodyElem,iPhysicsWorld *apPhysicsWorld, tLoaderCollideShapeMap &a_setShapes)
	{
		////////////////////////////////////////
		// Get shapes for body
		tCollideShapeVec vShapes;
		for(tinyxml2::XMLElement *pShapeElem = apBodyElem->FirstChildElement(); pShapeElem != NULL; pShapeElem = pShapeElem->NextSiblingElement())
		{
			int lShapeID = GetAttributeInt(pShapeElem, "ID");

			tLoaderCollideShapeMapIt it = a_setShapes.find(lShapeID);
			if(it != a_setShapes.end())
			{
				vShapes.push_back(it->second);
				a_setShapes.erase(it);
			}
		}
		
		////////////////////////////////////////
		// Create final shape
		if(vShapes.empty()) return NULL;
        if(vShapes.size()==1) return vShapes[0];

		iCollideShape *pCompound = apPhysicsWorld->CreateCompundShape(vShapes);
		return pCompound;
	}

	//-----------------------------------------------------------------------

	eCollideShapeType ToCollideShape(const tString& asType)
	{
		tString sLowType = cString::ToLowerCase(asType);

		if(sLowType == "box") return eCollideShapeType_Box;
		if(sLowType == "cylinder") return eCollideShapeType_Cylinder;
		if(sLowType == "sphere") return eCollideShapeType_Sphere;
		if(sLowType == "capsule") return eCollideShapeType_Capsule;

		Error("CollideShape '%s' does not exist!\n", asType.c_str());

		return eCollideShapeType_Null;
	}

	static iCollideShape* CreateCollideShape(tinyxml2::XMLElement *apShapeElem, iPhysicsWorld *apPhysicsWorld, const cVector3f &avScale)
	{
		eCollideShapeType type = ToCollideShape(GetAttributeString(apShapeElem, "ShapeType"));
		cVector3f vSize = GetAttributeVector3f(apShapeElem, "Scale") * avScale;
		cVector3f vPos = GetAttributeVector3f(apShapeElem, "RelativeTranslation") * avScale;
		cVector3f vRot = GetAttributeVector3f(apShapeElem, "RelativeRotation");

		cMatrixf mtxOffset = cMath::MatrixRotate(vRot,eEulerRotationOrder_XYZ);
		mtxOffset.SetTranslation(vPos);

		switch(type)
		{
		case eCollideShapeType_Box: 
			return apPhysicsWorld->CreateBoxShape(vSize,&mtxOffset);
		case eCollideShapeType_Sphere: 
			return apPhysicsWorld->CreateSphereShape(vSize,&mtxOffset);
		case eCollideShapeType_Cylinder: 
			mtxOffset = cMath::MatrixMul(mtxOffset, cMath::MatrixRotateZ(kPi2f));
			return apPhysicsWorld->CreateCylinderShape(vSize.x,vSize.y,&mtxOffset);
		case eCollideShapeType_Capsule: 
			mtxOffset = cMath::MatrixMul(mtxOffset, cMath::MatrixRotateZ(kPi2f));
			return apPhysicsWorld->CreateCapsuleShape(vSize.x,vSize.y,&mtxOffset);
		}

		return NULL;
	}

	//-----------------------------------------------------------------------
	
	static iPhysicsBody * FindBody(int alID, tLoaderPhysicsBodyMap &a_setBodies)
	{
		tLoaderPhysicsBodyMapIt it = a_setBodies.find(alID);
		if(it == a_setBodies.end()) return NULL;
        
		return it->second;
	}

	static ePhysicsJointType ToJointType(const tString& asType)
	{
		tString sLowType = cString::ToLowerCase(asType);

        if(sLowType == "jointhinge")	return ePhysicsJointType_Hinge;
		if(sLowType == "jointball")		return ePhysicsJointType_Ball;
		if(sLowType == "jointslider")	return ePhysicsJointType_Slider;
		if(sLowType == "joinscrew")		return ePhysicsJointType_Screw;

		Error("Joint type '%s' does not exist!\n", asType.c_str());

		return ePhysicsJointType_Ball;
	}

	static iPhysicsJoint* CreateJoint(	const tString& asEntityName,
										tinyxml2::XMLElement *apJointElem, iPhysicsWorld *apPhysicsWorld,
										tLoaderPhysicsBodyMap &a_setBodies,
										const cMatrixf& a_mtxTransform,
										const cVector3f& avScale)
	{

		/////////////////////////////
		//Get pin direction and pivot and transform according to entity
		cVector3f vPivot = GetAttributeVector3f(apJointElem, "WorldPos") * avScale;
		cVector3f vPinDir = GetAttributeVector3f(apJointElem, "PinDir");

		vPivot = cMath::MatrixMul(a_mtxTransform, vPivot);
		vPinDir = cMath::MatrixMul3x3(a_mtxTransform, vPinDir);
		
		/////////////////////////////
		//Name and type
		tString sJointName = asEntityName + "_" + GetAttributeString(apJointElem, "Name");

		ePhysicsJointType jointType = ToJointType(apJointElem->Value());


		/////////////////////////////
		//Get the bodies
		int lParentID = GetAttributeInt(apJointElem, "ConnectedParentBodyID");
		int lChildID = GetAttributeInt(apJointElem, "ConnectedChildBodyID");

		iPhysicsBody *pParentBody = lParentID > 0 ? FindBody(lParentID,a_setBodies) : NULL;
		iPhysicsBody *pChildBody = FindBody(lChildID,a_setBodies);
		
		if(pChildBody==NULL)
		{
			Error("Could not find child body with ID %d for joint '%s'\n", lChildID, sJointName.c_str());
			return NULL;
		}
		
		///////////////////////////
		// Hinge
		if(jointType == ePhysicsJointType_Hinge)
		{
			iPhysicsJointHinge *pJoint = apPhysicsWorld->CreateJointHinge(sJointName,vPivot,vPinDir,pParentBody,pChildBody);

			pJoint->SetMinAngle(cMath::ToRad(GetAttributeFloat(apJointElem, "MinAngle")));
			pJoint->SetMaxAngle(cMath::ToRad(GetAttributeFloat(apJointElem, "MaxAngle")));

			return pJoint;
		}
		///////////////////////////
		// Ball
		else if(jointType == ePhysicsJointType_Ball)
		{
			iPhysicsJointBall *pJoint = apPhysicsWorld->CreateJointBall(sJointName,vPivot,vPinDir,pParentBody,pChildBody);

			pJoint->SetConeLimits(	cMath::ToRad(GetAttributeFloat(apJointElem, "MaxConeAngle")),
									cMath::ToRad(GetAttributeFloat(apJointElem, "MaxTwistAngle")));

			return pJoint;
		}
		///////////////////////////
		// Slider
		else if(jointType == ePhysicsJointType_Slider)
		{
			iPhysicsJointSlider *pJoint = apPhysicsWorld->CreateJointSlider(sJointName, vPivot,vPinDir,pParentBody,pChildBody);

			pJoint->SetMinDistance(GetAttributeFloat(apJointElem, "MinDistance"));
			pJoint->SetMaxDistance(GetAttributeFloat(apJointElem, "MaxDistance"));

			return pJoint;
		}
		///////////////////////////
		// Screw
		else if(jointType == ePhysicsJointType_Screw)
		{
			iPhysicsJointScrew *pJoint = apPhysicsWorld->CreateJointScrew(sJointName,vPivot,vPinDir,pParentBody,pChildBody);

			pJoint->SetMinDistance(GetAttributeFloat(apJointElem, "MinDistance"));
			pJoint->SetMaxDistance(GetAttributeFloat(apJointElem, "MaxDistance"));

			return pJoint;
		}


		return NULL;	
	}

	
	//-----------------------------------------------------------------------

	static cMatrixf GetMatrixFromVectors(const cVector3f& avPos, const cVector3f& avRot, const cVector3f& avScale)
	{
		cMatrixf mtxOut = cMath::MatrixScale(avScale);
		mtxOut = cMath::MatrixMul(cMath::MatrixRotate(avRot, eEulerRotationOrder_XYZ), mtxOut);
		mtxOut.SetTranslation(avPos);

		return mtxOut;
	}


	//-----------------------------------------------------------------------

	eAnimationEventType ToAnimEventType(const tString& asType)
	{
		tString sLowType = cString::ToLowerCase(asType);

        if(sLowType == "playsound")	return eAnimationEventType_PlaySound;
		if(sLowType == "step")		return eAnimationEventType_Step;

		Error("No animation event named '%s'\n", asType.c_str());
		return eAnimationEventType_LastEnum;
	}
	
	//-----------------------------------------------------------------------


	iEntity3D* cEntityLoader_Object::Load(	const tString &asName, int alID, bool abActive, tinyxml2::XMLElement *apRootElem,
											const cMatrixf &a_mtxTransform, const cVector3f &avScale, 
											cWorld *apWorld, const tString &asFileName, const tWString &asFullPath, cResourceVarsObject *apInstanceVars)
	{
		////////////////////////////////////////	
		// Init
		mvBodies.clear();
		mvJoints.clear();

		mvBodyExtraData.clear();

		mvHapticShapes.clear();
		
		mvParticleSystems.clear();
		mvBillboards.clear();
		mvSoundEntities.clear();
		mvLights.clear();
		mvBeams.clear();

		tEntity3DList lstEntities;
		
		mpEntity = NULL;
		mpMesh = NULL;

		msFileName = asFileName; 
		mlID = alID;
		mbActive = abActive;
		mvScale = avScale;

		mbNodeAnimation = false;

		iPhysicsWorld *pPhysicsWorld = apWorld->GetPhysicsWorld();

		////////////////////////////////////////	
		// Load ModelData
		tinyxml2::XMLElement* pModelDataElem = apRootElem->FirstChildElement("ModelData");
		if(pModelDataElem==NULL){
			Error("Couldn't load element ModelData"); return NULL;
		}

		//msName = cString::ToString(pModelDataElem->Attribute("Name"),"");
		//msSubType = cString::ToString(pModelDataElem->Attribute("Subtype"),"");

		//////////////////////////////
		// Before load virtual call.
		BeforeLoad(apRootElem,a_mtxTransform,apWorld,apInstanceVars);


		////////////////////////////////////////	
		// Load Mesh and create entity
		tinyxml2::XMLElement *pMeshElem =NULL;
		{
			pMeshElem  = pModelDataElem->FirstChildElement("Mesh");
			if(pMeshElem==NULL){
				Error("Couldn't load element Mesh"); return NULL;
			}

			// Create mesh
			tString sMeshFile = GetAttributeString(pMeshElem, "Filename");
			if(cString::GetFilePath(sMeshFile).size() < 1)
			{
				sMeshFile = cString::SetFilePath(sMeshFile, cString::To8Char(cString::GetFilePathW(asFullPath) ) );
			}
			//Log("Mesh: '%s'\n",sMeshFile.c_str());
			mpMesh = apWorld->GetResources()->GetMeshManager()->CreateMesh(sMeshFile);
			if(mpMesh==NULL) return NULL;

			//Create entity
			mpEntity = apWorld->CreateMeshEntity(asName, mpMesh, mbLoadAsStatic);
			mpEntity->SetRendererMask(mlInstanceRendererMask);
			
			if(mpMesh->GetSkeleton()!=NULL)
				mpEntity->SetMatrix(cMath::MatrixScale(mvScale));

			//Set entity properties
			//TODO...
			mpEntity->SetRenderFlagBit(eRenderableFlag_ShadowCaster,true); //<- Temp
		}

		////////////////////////////////////////	
		// Load sub meshes
		{
			bool bHasSkeleton = mpMesh->GetSkeleton()!=NULL;
			for(tinyxml2::XMLElement *pSubMeshElem = pMeshElem->FirstChildElement(); pSubMeshElem != NULL; pSubMeshElem = pSubMeshElem->NextSiblingElement())
			{
				//////////////////////////
				// Load the sub entity
				tString sName = GetAttributeString(pSubMeshElem, "Name");
				//tString sMaterialFile = cString::ToString(pSubMeshElem->Attribute("MaterialFile"),"");

				cSubMeshEntity *pSubEntity = mpEntity->GetSubMeshEntityName(sName);
				if(pSubEntity==NULL)
				{
					Warning("Sub mesh '%s' does not exist in mesh '%s'!\n",sName.c_str(), mpMesh->GetName().c_str());
					continue;
				}
				if(bHasSkeleton==false)
					lstEntities.push_back(pSubEntity);

				
				//////////////////////////
				// Get transform matrix
				if(bHasSkeleton==false)
				{
					cMatrixf mtxLocalTransform = GetMatrixFromVectors(	GetAttributeVector3f(pSubMeshElem, "WorldPos")*mvScale,
																		GetAttributeVector3f(pSubMeshElem, "Rotation"),
																		GetAttributeVector3f(pSubMeshElem, "Scale")*mvScale);
					
					//mtxLocalTransform = cMath::MatrixMul(mtxLocalTransform, cMath::MatrixScale(mvScale));

					pSubEntity->SetWorldMatrix(mtxLocalTransform);
				}
				
				//////////////////////////
				// Set the variables
				int lID = GetAttributeInt(pSubMeshElem, "ID",-1);
				if(lID < 0) lID = GetAttributeInt(pSubMeshElem, "SubMeshID"); //To support older files!
				
				pSubEntity->SetUniqueID(lID);


				//////////////////////////
				// Set material
				//TODO:
				/*if(sMaterialFile != "")
				{
				cMaterial *pMaterial = apWorld->GetResources()->GetMaterialManager()->CreateMaterial(sMaterialFile);
				if(pMaterial)
				{
				pSubEntity->SetCustomMaterial(pMaterial);
				}
				}*/
			}
		}

		////////////////////////////////////////	
		// Animations
		tinyxml2::XMLElement *pAnimationsElem  = pModelDataElem->FirstChildElement("Animations");
		if(mbLoadAnimations && mpEntity && pAnimationsElem)
		{
			for(tinyxml2::XMLElement *pAnimElem = pAnimationsElem->FirstChildElement(); pAnimElem != NULL; pAnimElem = pAnimElem->NextSiblingElement())
			{
				tString sFile = GetAttributeString(pAnimElem, "File");
				tString sName = GetAttributeString(pAnimElem, "Name");
				float fSpeed = GetAttributeFloat(pAnimElem, "Speed",1.0f);
				float fSpecialEventTime = GetAttributeFloat(pAnimElem, "SpecialEventTime",0.0f);
				
				if(cString::GetFilePath(sFile).length() <= 1)
					sFile = cString::SetFilePath(sFile, cString::To8Char(cString::GetFilePathW(asFullPath) ) );

				// cAnimationState (created by AddAnimation) becomes the owner and Destroys
				// this animation via its manager back-pointer; hand it the raw reference.
				cAnimation *pAnim = apWorld->GetResources()->GetAnimationManager()->CreateAnimation(sFile).Release();

				if(pAnim)
				{
					cAnimationState *pState = mpEntity->AddAnimation(pAnim, sName,fSpeed);
					pState->SetSpecialEventTime(fSpecialEventTime);

					///////////////////////////////
					// Load events
					for(tinyxml2::XMLElement *pEventElem = pAnimElem->FirstChildElement(); pEventElem != NULL; pEventElem = pEventElem->NextSiblingElement())
					{
                        cAnimationEvent *pEvent = pState->CreateEvent();
						pEvent->mfTime = GetAttributeFloat(pEventElem, "Time");
						pEvent->mType = ToAnimEventType(GetAttributeString(pEventElem, "Type"));
						pEvent->msValue = GetAttributeString(pEventElem, "Value");
					}

				}
			}
		}


		////////////////////////////////////////
		// Load World entities
		{
			//List that contain light and billboard connections
			tEFL_LightBillboardConnectionList lstLightBillboardListConnections;

            tinyxml2::XMLElement *pEntitiesElem  = pModelDataElem->FirstChildElement("Entities");
			if(pEntitiesElem)
			{
				////////////////////////
				//Iterate entities
				for(tinyxml2::XMLElement *pEntityElem = pEntitiesElem->FirstChildElement(); pEntityElem != NULL; pEntityElem = pEntityElem->NextSiblingElement())
				{
					const tString sEntityType = pEntityElem->Value();
					iEntity3D *pEntity = NULL;

					// Child objects tagged for the other renderer backend are not created.
					// Lights are exempt from this filter: they carry both backends'
					// tuning and their mask gates rendering, not creation.
					if(cEngineFileLoading::IsElementEnabledForWorld(pEntityElem, apWorld)==false)
						continue;

					/////////////////////////
					// Particle System
					if(sEntityType == "ParticleSystem")
					{
						if(mbLoadParticleSystems)
						{
							cParticleSystem *pPS = cEngineFileLoading::LoadParticleSystem(pEntityElem,asName +"_", apWorld);
							if(pPS)	mvParticleSystems.push_back(pPS);
							pEntity = pPS;
						}
					}
					/////////////////////////
					// Billboard
					else if(sEntityType == "Billboard")
					{
						if(mbLoadBillboards)
						{
							cBillboard *pBillboard = cEngineFileLoading::LoadBillboard(pEntityElem,asName +"_", apWorld, apWorld->GetResources(), mbLoadAsStatic,
																						&lstLightBillboardListConnections);
							if(pBillboard)	mvBillboards.push_back(pBillboard);
							pEntity = pBillboard;
						}
					}
					/////////////////////////
					// Sound
					else if(sEntityType == "Sound")
					{
						if(mbLoadSounds)
						{
							cSoundEntity *pSound = cEngineFileLoading::LoadSound(pEntityElem,asName +"_", apWorld);
							if(pSound)	mvSoundEntities.push_back(pSound);
							pEntity = pSound;
						}
					}
                    /////////////////////////
					// Light
					else if(cString::GetLastStringPos(sEntityType,"Light")>0)
					{
						if(mbLoadLights)
						{
							iLight *pLight = cEngineFileLoading::LoadLight(pEntityElem,asName +"_", apWorld, apWorld->GetResources(),mbLoadAsStatic);
							if(pLight)	mvLights.push_back(pLight);
							pEntity = pLight;
						}
					}
					/////////////////////////
					// Unknown
					else
					{
						Error("Entity world entity type '%s' is unknown!\n", sEntityType.c_str());
					}

					/////////////////////////
					// Add to list and scale!
					if(pEntity)
					{
						// Children belong only to the renderers their instance does.
						iRenderable *pRenderable = dynamic_cast<iRenderable*>(pEntity);
						if(pRenderable) pRenderable->SetRendererMask(pRenderable->GetRendererMask() & mlInstanceRendererMask);

						//Scale the local position accoringly!
						cVector3f vPos = pEntity->GetLocalPosition();
						pEntity->SetPosition(vPos * mvScale);
						
						lstEntities.push_back(pEntity);
					}
				}
			}

			/////////////////////////////////////
			//Set up light and billboard connections
			for(tEFL_LightBillboardConnectionListIt connIt = lstLightBillboardListConnections.begin(); connIt != lstLightBillboardListConnections.end(); ++connIt)
			{
				cEFL_LightBillboardConnection& lightConnect = *connIt;

				cBillboard *pBB = NULL;
				for(size_t i=0; i<mvBillboards.size() && pBB==NULL; ++i)
					if(mvBillboards[i]->GetUniqueID() == lightConnect.msBillboardID) pBB = mvBillboards[i];
				iLight *pLight = NULL;
				for(size_t i=0; i<mvLights.size() && pLight==NULL; ++i)
					if(mvLights[i]->GetName() == lightConnect.msLightName) pLight = mvLights[i];

				if(pBB==NULL) continue;

				// Every light is created regardless of its renderer mask, so a
				// missing one is a genuinely broken reference.
				if(pLight==NULL)
				{
					Warning("Light '%s' connected to billboard with id '%d' does not exist!\n",
							lightConnect.msLightName.c_str(), lightConnect.msBillboardID);
					continue;
				}

				pLight->AttachBillboard(pBB, pBB->GetColor());
			}

		}

		////////////////////////////////////////	
		// Copy entity list to other list
		tEntity3DList lstTempEntities = lstEntities;


		////////////////////////////////////////	
		// Load Bones
		tNodeStateMap mapBoneStates;
		if(mpMesh->GetSkeleton())
		{
			tinyxml2::XMLElement *pBonesElem  = pModelDataElem->FirstChildElement("Bones");
			if(pBonesElem)
			{
				//////////////////////////
				// Iterate bones
				for(tinyxml2::XMLElement *pBoneElem = pBonesElem->FirstChildElement(); pBoneElem != NULL; pBoneElem = pBoneElem->NextSiblingElement())
				{
					int lID = GetAttributeInt(pBoneElem, "ID");
					tString sName = GetAttributeString(pBoneElem, "Name");

					cBoneState *pBoneState = mpEntity->GetBoneStateFromName(sName);
					if(pBoneState==NULL){
						Error("Could not find bone '%s' in model '%s'\n", sName.c_str(), cString::To8Char(asFullPath).c_str());
						continue;
					}

					/////////////////////////////
					//Add bones to list
                    mapBoneStates.insert(tNodeStateMap::value_type(lID, pBoneState));
					
					/////////////////////////////
					//Add children to bone
					LoadAndAttachChildren(pBoneElem, NULL, pBoneState, lstTempEntities, mapBoneStates, true, false);
				}
			}
		}
		


		////////////////////////////////////////	
		// Load Shapes
		tLoaderCollideShapeMap setShapes;

		if(pPhysicsWorld)
		{
			tinyxml2::XMLElement *pShapesElem  = pModelDataElem->FirstChildElement("Shapes");
			if(pShapesElem)
			{
				for(tinyxml2::XMLElement *pBodyShapeElem = pShapesElem->FirstChildElement(); pBodyShapeElem != NULL; pBodyShapeElem = pBodyShapeElem->NextSiblingElement())
				{
					iCollideShape *pCollideShape = CreateCollideShape(pBodyShapeElem, pPhysicsWorld, mvScale);
					if(pCollideShape == NULL) continue;
					int lID = GetAttributeInt(pBodyShapeElem, "ID");

					setShapes.insert(tLoaderCollideShapeMap::value_type(lID, pCollideShape));
				}
			}
		}
			

		////////////////////////////////////////	
		// Load Bodies
		tLoaderPhysicsBodyMap setBodies;

		if(pPhysicsWorld)
		{
			////////////////////////
			//Iterate the bodies elements
			tinyxml2::XMLElement *pBodiesElem  = pModelDataElem->FirstChildElement("Bodies");
			if(pBodiesElem)
			{
				for(tinyxml2::XMLElement *pBodyElem = pBodiesElem->FirstChildElement(); pBodyElem != NULL; pBodyElem = pBodyElem->NextSiblingElement())
				{
					/////////////////////
					// Init
					int lID = GetAttributeInt(pBodyElem, "ID");
					tString sBodyName = GetAttributeString(pBodyElem, "Name");

					/////////////////////
					// Get shape
					iCollideShape *pShape = GetBodyShape(pBodyElem,pPhysicsWorld,setShapes);
					if(pShape==NULL){
						Error("No shapes found for body '%s'\n", sBodyName.c_str());
						continue;
					}

					/////////////////////
					// Create body and set up properties
					iPhysicsBody *pBody = pPhysicsWorld->CreateBody(asName +"_"+ sBodyName,pShape);
					SetBodyProperties(pBody, pBodyElem);

					//Material
					iPhysicsMaterial *pPhysicsMat = pPhysicsWorld->GetMaterialFromName(GetAttributeString(pBodyElem, "Material"));
					if(pPhysicsMat) pBody->SetMaterial(pPhysicsMat);

					setBodies.insert(tLoaderPhysicsBodyMap::value_type(lID, pBody));
					mvBodies.push_back(pBody);

					/////////////////////
					// Add extra properties
					size_t lIdx = mvBodies.size() -1;
					mvBodyExtraData.push_back(cEntityBodyExtraData());

					mvBodyExtraData[lIdx].m_mtxLocalTransform = pBody->GetLocalMatrix();

					/////////////////////
					// Attach children
					LoadAndAttachChildren(pBodyElem, pBody, NULL, lstTempEntities, mapBoneStates, true, true);
				}
			}

			////////////////////////
			//If no bones got attached to bodies, then add the entire entity to be attached. (Luis: this comment does not make much sense given the code below, not sure what the point is)
			if(mpMesh->GetSkeleton())
			{
				if(mapBoneStates.size() != mpEntity->GetBoneStateNum())
				{
					Error("Loading entity %s: Skeletons in mesh file (%ls) and .ent file (%ls) differ! Probably caused by .ent not being up to date with mesh\n",
						asName.c_str(),
						mpMesh->GetFullPath().c_str(),
						asFullPath.c_str());
				}
				else
					lstTempEntities.push_back(mpEntity);
			}

			////////////////////////
			//Destroy any left over shapes
			tLoaderCollideShapeMapIt shapeSetIt = setShapes.begin();
			for(; shapeSetIt != setShapes.end(); ++shapeSetIt)
			{
				pPhysicsWorld->DestroyShape(shapeSetIt->second);
			}
		}
		
		////////////////////////////////////////	
		// Add all remaining entities directly to first body
		if(mvBodies.empty()==false)
		{
			iPhysicsBody *pMainBody = mvBodies[0];
			cMatrixf mtxInvParent = cMath::MatrixInverse(pMainBody->GetLocalMatrix());

			for(tEntity3DListIt it = lstTempEntities.begin(); it != lstTempEntities.end(); ++it)
			{
				iEntity3D *pEntity = *it;

				if(pEntity->GetEntityType() == "SubMesh" && mpMesh->GetSkeleton() == NULL && mpMesh->GetAnimationNum() > 0)
				{
					mbNodeAnimation = true;
					continue;
				}
				else
				{
					AttachEntityChild(pMainBody, mtxInvParent, pEntity);
				}
			}

			if(mbNodeAnimation)
			{
				/////////////
				// Attach the whole entity to the body
				AttachEntityChild(pMainBody, mtxInvParent, mpEntity);
			}
		}


		////////////////////////////////////////	
		// Set matrix on entity if there are no bodies.
		if(mvBodies.size()<=0)
		{
			if(mpMesh->GetSkeleton())
				mpEntity->SetMatrix(cMath::MatrixMul(a_mtxTransform, cMath::MatrixScale(avScale)) );
			else
				mpEntity->SetMatrix(a_mtxTransform);
			
			//to make sure everything is in place.
			mpEntity->UpdateLogic(0);
		}
		else
		{
			for(size_t i=0;i<mvBodies.size(); ++i)
			{
				iPhysicsBody *pBody = mvBodies[i];
				
				pBody->SetMatrix(cMath::MatrixMul(a_mtxTransform,pBody->GetLocalMatrix()));
			}
		}

		if(pPhysicsWorld)
		////////////////////////////////////////	
		// Load Joints
		{
			tinyxml2::XMLElement *JointsElem  = pModelDataElem->FirstChildElement("Joints");
			if(JointsElem)
			{
				for(tinyxml2::XMLElement *pJointElem = JointsElem->FirstChildElement(); pJointElem != NULL; pJointElem = pJointElem->NextSiblingElement())
				{
					iPhysicsJoint *pJoint = CreateJoint(asName,pJointElem,pPhysicsWorld,setBodies, a_mtxTransform, mvScale);
					if(pJoint)
					{
						SetJointProperties(pJoint,pJointElem, apWorld);

						mvJoints.push_back(pJoint);
					}
				}
			}
		}

		////////////////////////////////////////	
		// Reset the unique ID (will mess with other entities otherwise)
		// Do not want to do this with bodies!
		for(tEntity3DListIt it = lstEntities.begin(); it != lstEntities.end(); ++it)
		{
			iEntity3D *pEntity = *it;
			pEntity->SetUniqueID(-1);
		}


		////////////////////////////////////////	
		// Final setup, start animation or go to ragdoll mode is possible
		if(mpEntity->GetAnimationStateNum() >0)
		{
			mpEntity->Play(0, true, true);
		}
		else if(mpEntity->GetMesh()->GetSkeleton() && mvBodies.size() >0)
		{
			mpEntity->SetSkeletonPhysicsActive(true);
		}
		
		////////////////////////////////////////	
		// Load user variables();
		LoadUserVariables(apRootElem);
		
		// After load virtual call.
		// This is where the user adds extra stuff.
		AfterLoad(apRootElem,a_mtxTransform,apWorld,apInstanceVars);

		return mpEntity;
	}
	
	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PRIVATE METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------
	
	void cEntityLoader_Object::AttachEntityChild(iEntity3D *apParent, const cMatrixf& a_mtxInvParent, iEntity3D *apChild)
	{
		//Calculate local matrix
		cMatrixf mtxLocalTransform = cMath::MatrixMul(a_mtxInvParent, apChild->GetWorldMatrix());
		apChild->SetMatrix(mtxLocalTransform);

		//Add the entity to parent and remove it from any other hierarchy it might be in.
		if(apChild->GetEntityParent()) 
			apChild->GetEntityParent()->RemoveChild(apChild);
		apParent->AddChild(apChild);
	}

	//-----------------------------------------------------------------------

	void cEntityLoader_Object::AttachBoneChild(cBoneState *apBoneState, const cMatrixf& a_mtxInvParent, iEntity3D *apChild)
	{
		//Calculate local matrix
		cMatrixf mtxLocalTransform = cMath::MatrixMul(a_mtxInvParent, apChild->GetWorldMatrix());

		apChild->SetMatrix(mtxLocalTransform);

		//Add the entity to parent and remove it from any other hierarchy it might be in.
		if(apChild->GetParent()) 
			apChild->GetParent()->RemoveEntity(apChild);
		apBoneState->AddEntity(apChild);	
	}

	//-----------------------------------------------------------------------

	void cEntityLoader_Object::AttachBoneToBody(iPhysicsBody *apParentBody, const cMatrixf& a_mtxInvParent, cBoneState *apBoneState)
	{
		cMatrixf mtxInvBone = cMath::MatrixInverse(apBoneState->GetWorldMatrix());
		cMatrixf mtxBoneToBody = cMath::MatrixMul(mtxInvBone, apParentBody->GetWorldMatrix());

		//Attaching work differently for bones since they rely on base matrix and animations.
		//This sets up the basic properties and then the skeletal physics needs to be turned on the mesh entity.
		apBoneState->SetBody(apParentBody);
		apBoneState->SetBodyMatrix(mtxBoneToBody);

		apParentBody->SetIsRagDoll(true);
		//apParentBody->SetCollideRagDoll(false);

		/////////////////////////////////////
		//Create collider body
		/*
		iPhysicsBody *pColliderBody = mpPhysicsWorld->CreateBody(apEntity->GetName()+"_collider_"+pBoneState->GetName(),pShape);
		pColliderBody->SetMass(0);
		pColliderBody->SetActive(false);
		pColliderBody->SetCollideCharacter(false);

		pBoneState->SetColliderBody(pColliderBody);
		*/
	}
	
	//-----------------------------------------------------------------------
	
	void cEntityLoader_Object::LoadAndAttachChildren(	tinyxml2::XMLElement *apMainElem, iEntity3D *apEntityParent, cBoneState *apBoneStateParent,
														tEntity3DList& a_lstChildList, tNodeStateMap &a_mapBoneStates,
														bool abRemoveAttachedChild, bool abIsBody)
	{
		tinyxml2::XMLElement *pChildrenElem  = apMainElem->FirstChildElement("Children");
		if(pChildrenElem==NULL) return;

		cMatrixf mtxInvParent;
		if(apEntityParent)
			mtxInvParent = cMath::MatrixInverse(apEntityParent->GetLocalMatrix());
		else
			mtxInvParent = cMath::MatrixInverse(apBoneStateParent->GetWorldMatrix());

		///////////////////////////////
		//Iterate the children elements
		for(tinyxml2::XMLElement *pChildElem = pChildrenElem->FirstChildElement(); pChildElem != NULL; pChildElem = pChildElem->NextSiblingElement())
		{
			int lID = GetAttributeInt(pChildElem, "ID");
			
			//////////////////////////////////
			// Search for child entity
			bool bFound = false;
			for(tEntity3DListIt it = a_lstChildList.begin(); it != a_lstChildList.end(); ++it)
			{
				//////////////////////////
				//If found attach and remove from list if set
				iEntity3D *pEntity = *it;
				if(pEntity->GetUniqueID() != lID) continue;

				//Attach
				if(apEntityParent)
				{
					if(pEntity->GetEntityType() == "SubMesh" && mpMesh->GetSkeleton() == NULL && mpMesh->GetAnimationNum() > 0)
					{
						mbNodeAnimation = true;
						continue;
					}
					else
					{
						AttachEntityChild(apEntityParent, mtxInvParent, pEntity);
					}
				}
				else
					AttachBoneChild(apBoneStateParent, mtxInvParent, pEntity);
								
				//Final stuff				
				if(abRemoveAttachedChild) a_lstChildList.erase(it);
				bFound = true;
				break;
			}

			//////////////////////////////////
			// Search for child bone
			if(abIsBody && bFound==false)
			{
				tNodeStateMapIt it = a_mapBoneStates.find(lID);
				if(it != a_mapBoneStates.end())
				{
					iPhysicsBody *pParentBody = static_cast<iPhysicsBody*>(apEntityParent);

					cBoneState *pBoneState = it->second;
					AttachBoneToBody(pParentBody, mtxInvParent, pBoneState);
					bFound = true;

					a_mapBoneStates.erase(it);
				}
			}


			if(bFound == false)
			{
				if(apEntityParent)
					Warning("Could not find child with unique ID %d to attach to entity '%s' in '%s'\n", lID, apEntityParent->GetName().c_str(), msFileName.c_str());
				else
					Warning("Could not find child with unique ID %d to attach to bone '%s' in '%s'\n", lID, apBoneStateParent->GetName().c_str(), msFileName.c_str());
			}
		}
	}

	//-----------------------------------------------------------------------

	void cEntityLoader_Object::SetBodyProperties(iPhysicsBody *apBody, tinyxml2::XMLElement *apElem)
	{
		apBody->SetMatrix(GetMatrixFromVectors(	GetAttributeVector3f(apElem, "WorldPos") * mvScale,
												GetAttributeVector3f(apElem, "Rotation"),
												1.0f)
												);

		apBody->SetMass(GetAttributeFloat(apElem, "Mass",1.0f));

		apBody->SetAngularDamping(GetAttributeFloat(apElem, "AngularDamping"));
		apBody->SetLinearDamping(GetAttributeFloat(apElem, "LinearDamping"));

		apBody->SetBlocksSound(GetAttributeBool(apElem, "BlocksSound",false));
		apBody->SetCollideCharacter(GetAttributeBool(apElem, "CollideCharacter",true));
		apBody->SetCollide(GetAttributeBool(apElem, "CollideNonCharacter",true));

		apBody->SetGravity(GetAttributeBool(apElem, "HasGravity",true));
		apBody->SetBuoyancyDensityMul(GetAttributeFloat(apElem, "BuoyancyDensityMul",1.0));

		apBody->SetMaxAngularSpeed(GetAttributeFloat(apElem, "MaxAngularSpeed",0));
		apBody->SetMaxLinearSpeed(GetAttributeFloat(apElem, "MaxLinearSpeed",0));

		apBody->SetContinuousCollision(GetAttributeBool(apElem, "ContinuousCollision",true));

		apBody->SetPushedByCharacterGravity(GetAttributeBool(apElem, "PushedByCharacterGravity",false));

		apBody->SetVolatile(GetAttributeBool(apElem, "Volatile",false));

		apBody->SetUseSurfaceEffects(GetAttributeBool(apElem, "UseSurfaceEffects",true));

		apBody->SetGravityCanAttachCharacter(GetAttributeBool(apElem, "CanAttachCharacter",false));

		apBody->SetUniqueID(GetAttributeInt(apElem, "ID",-1));
	}

	//-----------------------------------------------------------------------

	void cEntityLoader_Object::SetJointProperties(iPhysicsJoint *pJoint, tinyxml2::XMLElement *apJointElem, cWorld *apWorld)
	{
		tString t = GetAttributeString(apJointElem, "MoveSound","");
		pJoint->SetMoveSound(t);
		pJoint->SetMinMoveSpeed(GetAttributeFloat(apJointElem, "MinMoveSpeed",0.5f));
		pJoint->SetMinMoveFreq(GetAttributeFloat(apJointElem, "MinMoveFreq",0.9f));
		pJoint->SetMinMoveVolume(GetAttributeFloat(apJointElem, "MinMoveVolume",0.3f));
		pJoint->SetMinMoveFreqSpeed(GetAttributeFloat(apJointElem, "MinMoveFreqSpeed",0.9f));
		pJoint->SetMaxMoveFreq(GetAttributeFloat(apJointElem, "MaxMoveFreq",1.1f));
		pJoint->SetMaxMoveVolume(GetAttributeFloat(apJointElem, "MaxMoveVolume",1.0f));
		pJoint->SetMaxMoveFreqSpeed(GetAttributeFloat(apJointElem, "MaxMoveFreqSpeed",1.1f));
		pJoint->SetMiddleMoveSpeed(GetAttributeFloat(apJointElem, "MiddleMoveSpeed",1.0f));
		pJoint->SetMiddleMoveVolume(GetAttributeFloat(apJointElem, "MiddleMoveVolume",1.0f));
		pJoint->SetMoveSpeedType(cString::ToLowerCase(GetAttributeString(apJointElem, "MoveType","Linear")) == "angular" ?
									ePhysicsJointSpeed_Angular : 	ePhysicsJointSpeed_Linear);

		pJoint->SetStickyMinLimit(GetAttributeBool(apJointElem, "StickyMinLimit",false));
		pJoint->SetStickyMaxLimit(GetAttributeBool(apJointElem, "StickyMaxLimit",false));

		pJoint->SetBreakable(GetAttributeBool(apJointElem, "Breakable",false));
		pJoint->SetBreakForce(GetAttributeFloat(apJointElem, "BreakForce",1000));
		pJoint->SetBreakSound(GetAttributeString(apJointElem, "BreakSound",""));

		pJoint->SetLimitAutoSleep(GetAttributeBool(apJointElem, "LimitAutoSleep",false));
		pJoint->SetLimitAutoSleepDist(GetAttributeFloat(apJointElem, "LimitAutoSleepDist",0.02f));
		pJoint->SetLimitAutoSleepNumSteps(GetAttributeInt(apJointElem, "LimitAutoSleepNumSteps",10));

		pJoint->SetCollideBodies(GetAttributeBool(apJointElem, "CollideBodies",true));

		pJoint->GetMaxLimit()->msSound = GetAttributeString(apJointElem, "MaxLimitSound","");
		pJoint->GetMaxLimit()->mfMaxSpeed = GetAttributeFloat(apJointElem, "MaxLimitMaxSpeed",10.0f);
		pJoint->GetMaxLimit()->mfMinSpeed = GetAttributeFloat(apJointElem, "MaxLimit_MinSpeed",20.0f);
		if(pJoint->GetMaxLimit()->mfMaxSpeed <=0) pJoint->GetMaxLimit()->mfMaxSpeed = 0.01f;

		pJoint->GetMinLimit()->msSound = GetAttributeString(apJointElem, "MinLimitSound","");
		pJoint->GetMinLimit()->mfMaxSpeed = GetAttributeFloat(apJointElem, "MinLimitMaxSpeed",10.0f);
		pJoint->GetMinLimit()->mfMinSpeed = GetAttributeFloat(apJointElem, "MinLimitMinSpeed",20.0f);
		if(pJoint->GetMinLimit()->mfMaxSpeed <=0) pJoint->GetMaxLimit()->mfMaxSpeed = 0.01f;

		pJoint->SetUniqueID(GetAttributeInt(apJointElem, "ID",-1));


		/////////////////////////////
		//Load all controllers
		//TODO!
		/*TiXmlElement *pControllerElem = pJointElem->FirstChildElement("Controller");
		for(; pControllerElem != NULL; pControllerElem = pControllerElem->NextSiblingElement("Controller"))
		{
			LoadController(pJoint,apWorld->GetPhysicsWorld(),pControllerElem);
		}*/
	}

	//-----------------------------------------------------------------------
	
	ePhysicsControllerType GetControllerType(const char* apString)
	{
		if(apString == NULL) return ePhysicsControllerType_LastEnum;

		tString sName = apString;

		if(sName == "Pid") return ePhysicsControllerType_Pid;
		else if(sName == "Spring") return ePhysicsControllerType_Spring;
		
		return ePhysicsControllerType_LastEnum;
	}

	/////////////////////////

	static ePhysicsControllerInput GetControllerInput(const char* apString)
	{
		if(apString == NULL) return ePhysicsControllerInput_LastEnum;

		tString sName = apString;

		if(sName == "JointAngle") return ePhysicsControllerInput_JointAngle;
		else if(sName == "JointDist") return ePhysicsControllerInput_JointDist; 
		else if(sName == "LinearSpeed") return ePhysicsControllerInput_LinearSpeed;
		else if(sName == "AngularSpeed")  return ePhysicsControllerInput_AngularSpeed;
		
		return ePhysicsControllerInput_LastEnum;
	}

	/////////////////////////

	static ePhysicsControllerOutput GetControllerOutput(const char* apString)
	{
		if(apString == NULL) return ePhysicsControllerOutput_LastEnum;

		tString sName = apString;

		if(sName == "Force") return ePhysicsControllerOutput_Force;
		else if(sName == "Torque") return ePhysicsControllerOutput_Torque; 
		
		return ePhysicsControllerOutput_LastEnum;
	}

	/////////////////////////

	static ePhysicsControllerAxis GetControllerAxis(const char* apString)
	{
		if(apString == NULL) return ePhysicsControllerAxis_LastEnum;

		tString sName = apString;

		if(sName == "X") return ePhysicsControllerAxis_X;
		else if(sName == "Y") return ePhysicsControllerAxis_Y; 
		else if(sName == "Z") return ePhysicsControllerAxis_Z; 

		return ePhysicsControllerAxis_LastEnum;
	}

	/////////////////////////

	static ePhysicsControllerEnd GetControllerEnd(const char* apString)
	{
		if(apString == NULL) return ePhysicsControllerEnd_Null;

		tString sName = apString;

		if(sName == "OnMax") return ePhysicsControllerEnd_OnMax;
		else if(sName == "OnMin") return ePhysicsControllerEnd_OnMin; 
		else if(sName == "OnDest") return ePhysicsControllerEnd_OnDest; 

		return ePhysicsControllerEnd_Null;
	}

	/////////////////////////


	void cEntityLoader_Object::LoadController(iPhysicsJoint* apJoint,iPhysicsWorld *apPhysicsWorld,
												tinyxml2::XMLElement *apElem)
	{
		//////////////////////////////
		// Get the properties
		/*tString sName = cString::ToString(apElem->Attribute("Name"),"");
		bool bActive = cString::ToBool(apElem->Attribute("Active"),false);

		ePhysicsControllerType CtrlType = GetControllerType(apElem->Attribute("Type"));
		float fA= cString::ToFloat(apElem->Attribute("A"),0);
		float fB= cString::ToFloat(apElem->Attribute("B"),0);
		float fC= cString::ToFloat(apElem->Attribute("C"),0);
		int lIntegralSize = cString::ToInt(apElem->Attribute("IntegralSize"),1);

		ePhysicsControllerInput CtrlInput = GetControllerInput(apElem->Attribute("Input"));
		ePhysicsControllerAxis CtrlInputAxis = GetControllerAxis(apElem->Attribute("InputAxis"));
		float fDestValue = cString::ToFloat(apElem->Attribute("DestValue"),0);
		float fMaxOutput = cString::ToFloat(apElem->Attribute("MaxOutput"),0);

		ePhysicsControllerOutput CtrlOutput = GetControllerOutput(apElem->Attribute("Output"));
		ePhysicsControllerAxis CtrlOutputAxis = GetControllerAxis(apElem->Attribute("OutputAxis"));
		bool bMulMassWithOutput = cString::ToBool(apElem->Attribute("MulMassWithOutput"),false);

		ePhysicsControllerEnd CtrlEnd = GetControllerEnd(apElem->Attribute("EndType"));
		tString sNextCtrl = cString::ToString(apElem->Attribute("NextController"),"");
		
		bool bLogInfo = cString::ToBool(apElem->Attribute("LogInfo"),false);

		//Convert degrees to radians.
		if(CtrlInput == ePhysicsControllerInput_JointAngle) {
			fDestValue = cMath::ToRad(fDestValue);
		}

		//////////////////////////////
		// Create the controller
		iPhysicsController *pController =apPhysicsWorld->CreateController(sName);

		pController->SetType(CtrlType);

		pController->SetA(fA);
		pController->SetB(fB);
		pController->SetC(fC);

		pController->SetPidIntegralSize(lIntegralSize);

		pController->SetActive(bActive);
		pController->SetInputType(CtrlInput,CtrlInputAxis);
		pController->SetDestValue(fDestValue);
        
		pController->SetOutputType(CtrlOutput,CtrlOutputAxis);
		pController->SetMaxOutput(fMaxOutput);
		pController->SetMulMassWithOutput(bMulMassWithOutput);

		pController->SetEndType(CtrlEnd);
		pController->SetNextController(sNextCtrl);

		pController->SetLogInfo(bLogInfo);

		apJoint->AddController(pController);

		//Log("Controller: %s active: %d val: %f %f %f input: %d %d output: %d %d\n",
		//	sName.c_str(),bActive, fA,fB,fC, (int)CtrlInput, (int)CtrlInputAxis, (int)CtrlOutput,(int)CtrlOutputAxis);*/
	}

	//-----------------------------------------------------------------------

	eAnimationEventType cEntityLoader_Object::GetAnimationEventType(const char* apString)
	{
		if(apString == NULL) return eAnimationEventType_LastEnum;

        tString sName = apString;
		sName = cString::ToLowerCase(sName);

		if(sName == "playsound")
		{
			return eAnimationEventType_PlaySound;
		}

		Warning("Animation event type '%s' does not exist!\n",apString);
		return eAnimationEventType_LastEnum;
	}
	
	//-----------------------------------------------------------------------

	void cEntityLoader_Object::LoadUserVariables(tinyxml2::XMLElement *apRootElem)
	{
		tinyxml2::XMLElement *pVarRootElem = apRootElem->FirstChildElement("UserDefinedVariables");
		if(pVarRootElem==NULL){
			Warning("Can not find a use variable root element!\n");
			return;
		}

		msEntityType = GetAttributeString(pVarRootElem, "EntityType");
		msEntitySubType = GetAttributeString(pVarRootElem, "EntitySubType");

		LoadVariables(pVarRootElem);
	}
	
	//-----------------------------------------------------------------------
}
