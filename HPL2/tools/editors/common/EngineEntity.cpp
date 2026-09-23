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

#include "EngineEntity.h"

#include "EntityIcon.h"
#include "EntityWrapper.h"
#include "EditorWorld.h"

#include "EditorWindowViewport.h"
#include "EditorHelper.h"

#include "scene/Decal.h"

//-----------------------------------------------------------------------

void iEngineEntity::SetMatrix(const cMatrixf& amtxX)
{
	if(mpEntity)
		mpEntity->SetWorldMatrix(amtxX);
}

const cMatrixf& iEngineEntity::GetWorldMatrix()
{
	return mpEntity->GetWorldMatrix();
}

//-----------------------------------------------------------------------

bool iEngineEntity::IsCulledByFrustum(cCamera* apCamera)
{
	cBoundingVolume* pBV = GetRenderBV();
	cFrustum* pFrustum = apCamera->GetFrustum();
	if(pBV)
		return (pFrustum->CollideBoundingVolume(pBV)!=eCollision_Outside)==false;
	
	return pFrustum->CollidePoint(mpParent->GetPosition())==false;
}

//-----------------------------------------------------------------------

bool iEngineEntity::IsInsideBoundingVolume(cBoundingVolume* apBV, cEditorWindowViewport* apViewport)
{
	cBoundingVolume* pBV = GetPickBV(apViewport);

	return cMath::CheckAABBIntersection(apBV->GetMin(), apBV->GetMax(), pBV->GetMin(), pBV->GetMax());
}

//-----------------------------------------------------------------------

bool iEngineEntity::CheckRayBoundingVolumeIntersect(cEditorWindowViewport* apViewport, cVector3f* apPos, float* apT)
{
	cBoundingVolume* pBV = GetPickBV(apViewport);

	return cMath::CheckAABBLineIntersection(pBV->GetMin(), pBV->GetMax(), apViewport->GetUnprojectedStart(), apViewport->GetUnprojectedEnd(), apPos, apT);
}

//-----------------------------------------------------------------------

bool iEngineEntity::Check2DBoxIntersect(cEditorWindowViewport* apViewport, const cRect2l& aBox)
{
	cRect2l rect;
	cFrustum* pFrustum = apViewport->GetCamera()->GetFrustum();
	cBoundingVolume* pBV = GetPickBV(apViewport);
	cMath::GetClipRectFromBV(rect,*pBV,pFrustum,apViewport->GetGuiViewportSizeInt(), pFrustum->GetFOV()*0.5f);

	return cMath::CheckRectIntersection(aBox, rect);
}

bool iEngineEntity::CheckRayIntersect(cEditorWindowViewport* apViewport, cVector3f* apPos, tVector3fVec* apTriangle, float* apT)
{
	return IsInsideBoundingVolume(apViewport->GetRayBV(), apViewport) &&
			CheckRayBoundingVolumeIntersect(apViewport, apPos, apT);
}


cBoundingVolume* iEngineEntity::GetRenderBV()
{
	if(mpEntity)
		return mpEntity->GetBoundingVolume(); 
	
	return NULL;
}
//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////
// MESH ENTITY - CONSTRUCTORS
//////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

float iEngineEntityMesh::mfDisabledCoverage = 0.5f;

//-----------------------------------------------------------------------

iEngineEntityMesh::iEngineEntityMesh(iEntityWrapper* apParent) : iEngineEntity(apParent), mpMesh(NULL)
{
	mbShowMesh = true;
}

iEngineEntityMesh::~iEngineEntityMesh()
{
	if(mpEntity)
	{
		cWorld* pWorld = mpParent->GetEditorWorld()->GetWorld();
		pWorld->DestroyMeshEntity((cMeshEntity*)mpEntity);
	}	
}

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////
// MESH ENTITY - PUBLIC METHODS
//////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

void iEngineEntityMesh::SetUpMesh()
{
	cMeshEntity* pMeshEntity = GetMeshEntity();
	if(pMeshEntity==NULL){
		Error("Engine entity had a NULL MeshEntity during SetUpMesh\n");
		return;
	}

	for(int i=0;i<pMeshEntity->GetSubMeshEntityNum();++i)
	{
		cSubMeshEntity* pSubMeshEntity = pMeshEntity->GetSubMeshEntity(i);
        pSubMeshEntity->SetUserData(mpParent);
	}
}

bool iEngineEntityMesh::Create(const tString& asName)
{
	if(mpMesh==NULL) return false;

	cWorld* pWorld = mpParent->GetEditorWorld()->GetWorld();
	mpEntity = pWorld->CreateMeshEntity(asName, mpMesh);
	SetUpMesh();

	return IsCreated();
}

//-----------------------------------------------------------------------

bool iEngineEntityMesh::CheckRayIntersect(cEditorWindowViewport* apViewport, cVector3f* apPos, tVector3fVec* apTriangle, float* apT)
{
	if(iEngineEntity::CheckRayIntersect(apViewport, apPos, apTriangle, apT)==false)
		return false;

	return cEditorHelper::CheckRayMeshEntityIntersect(apViewport->GetUnprojectedStart(), apViewport->GetUnprojectedEnd(), 
														(cMeshEntity*)mpEntity, apPos, apTriangle, apT);
}

//-----------------------------------------------------------------------

void iEngineEntityMesh::Update()
{
	float fCoverage = mpParent->IsActive()? 1.0f : mfDisabledCoverage ;
	SetCoverage(fCoverage);
}

//-----------------------------------------------------------------------

void iEngineEntityMesh::UpdateVisibility()
{
	Update();
	// Blocker meshes (ShowMesh=false in the loader) follow the Blockers
	// visibility toggle; normal meshes are always allowed.
	bool bBlockerVis = true;
	if(!mbShowMesh)
		bBlockerVis = cEditorHelper::GetVisibilityTypeState(eEditorVisibilityType_Blockers);
	((cMeshEntity*)mpEntity)->SetVisible(mpParent->IsVisible() && mpParent->IsCulledByClipPlanes()==false && bBlockerVis);
}

//-----------------------------------------------------------------------

void iEngineEntityMesh::Draw(cEditorWindowViewport* apViewport, DebugDraw* apFunctions, bool abIsSelected,	bool abIsActive, const cColor& aHighlightCol)
{
	if(abIsSelected==false)
		return;

	cMeshEntity* pMeshEntity = GetMeshEntity();
	for(int i=0;i<pMeshEntity->GetSubMeshEntityNum();++i)
	{
		cSubMeshEntity* pSubMeshEntity = pMeshEntity->GetSubMeshEntity(i);

		DebugDraw::DebugDrawOptions options;
		cMatrixf* pModelMtx = pSubMeshEntity->GetModelMatrix(NULL);
		if(pModelMtx) options.m_transform = *pModelMtx;

		apFunctions->DebugWireFrameFromVertexBuffer(pSubMeshEntity->GetVertexBuffer(), aHighlightCol, options);
	}
}

//-----------------------------------------------------------------------

void iEngineEntityMesh::DrawProgram(cEditorWindowViewport* apViewport, DebugDraw* apFunctions, iGpuProgram* apProg, const cColor& aCol)
{
	// Legacy drew the submeshes through a flat-color GPU program; the batcher
	// equivalent is a solid tinted triangle walk over the vertex buffers.
	cMeshEntity* pMeshEntity = GetMeshEntity();
	for(int i=0;i<pMeshEntity->GetSubMeshEntityNum();++i)
	{
		cSubMeshEntity* pSubMeshEntity = pMeshEntity->GetSubMeshEntity(i);

		DebugDraw::DebugDrawOptions options;
		cMatrixf* pModelMtx = pSubMeshEntity->GetModelMatrix(NULL);
		if(pModelMtx) options.m_transform = *pModelMtx;

		apFunctions->DebugSolidFromVertexBuffer(pSubMeshEntity->GetVertexBuffer(), aCol, options);
	}
}

//-----------------------------------------------------------------------

void iEngineEntityMesh::SetCastShadows(bool abX)
{
	((cMeshEntity*)mpEntity)->SetRenderFlagBit(eRenderableFlag_ShadowCaster,abX);
}

//-----------------------------------------------------------------------

void iEngineEntityMesh::SetCoverage(float afX)
{
	((cMeshEntity*)mpEntity)->SetCoverageAmount(afX);
}

//-----------------------------------------------------------------------

bool iEngineEntityMesh::SetMaterial(const tString& asX)
{
	SharedResourceHandle<cMaterial> hMat = cEditorHelper::LoadResourceFile<cMaterial>(asX);
	if(hMat)
	{
		cMesh* pMesh = ((cMeshEntity*)mpEntity)->GetMesh();
		if(pMesh)
		{
			for(int i=0;i<pMesh->GetSubMeshNum();++i)
			{
				cSubMesh* pSubMesh = pMesh->GetSubMesh(i);
				pSubMesh->SetMaterial(hMat); // copy: each submesh takes its own reference
			}
		}

		return true;
	}

	return false;
}

//-----------------------------------------------------------------------

bool iEngineEntityMesh::SetCustomMaterial(const tString& asX, bool abDelete)
{
	SharedResourceHandle<cMaterial> hMat = cEditorHelper::LoadResourceFile<cMaterial>(asX);
	if(hMat)
	{
		cMeshEntity* pMeshEnt = ((cMeshEntity*)mpEntity);
		if(pMeshEnt)
		{
			for(int i=0;i<pMeshEnt->GetSubMeshEntityNum();++i)
			{
				cSubMeshEntity* pSubMeshEnt = pMeshEnt->GetSubMeshEntity(i);
				pSubMeshEnt->SetCustomMaterial(hMat, abDelete); // copy: each takes its own reference
			}
		}

		return true;
	}

	return false;
}

//-----------------------------------------------------------------------

bool iEngineEntityMesh::SetCustomMaterial(cMaterial* apMat, bool abDelete)
{
	cMeshEntity* pMeshEnt = GetMeshEntity();
	if(pMeshEnt==NULL)
		return false;

	{
		cSubMeshEntity* pSubMeshEntity = pMeshEnt->GetSubMeshEntity(0);
		if(pSubMeshEntity->GetCustomMaterial()==apMat)
			return false;
	}
	cMaterialManager* pMatMgr = mpParent->GetEditorWorld()->GetEditor()->GetEngine()->GetResources()->GetMaterialManager();
	for(int i=0;i<pMeshEnt->GetSubMeshEntityNum();++i)
	{
		cSubMeshEntity* pSubMeshEnt = pMeshEnt->GetSubMeshEntity(i);
		// apMat is borrowed (caller-owned); each sub-entity takes its own reference.
		pSubMeshEnt->SetCustomMaterial(RetainResource<cMaterial>(pMatMgr, apMat), abDelete);
	}
	return true;
}

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////
// LOADED MESH ENTITY - CONSTRUCTORS
//////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

cEngineEntityLoadedMesh::cEngineEntityLoadedMesh(iEntityWrapper* apParent, const tString& asFilename) : iEngineEntityMesh(apParent)
{
	iEditorWorld* pWorld = mpParent->GetEditorWorld();

	cMesh *pMesh = pWorld->GetEditor()->GetEngine()->GetResources()->GetMeshManager()->CreateMesh(asFilename);
	SetMesh(pMesh);
}

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////
// LOADED MESH AGGREGATE ENTITY - CONSTRUCTORS
//////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

cEngineEntityLoadedMeshAggregate::cEngineEntityLoadedMeshAggregate(iEntityWrapper* apParent, const tString& asFilename) : iEngineEntityMesh(apParent)
{
	msFilename = asFilename;
	mbLightsActive = true;
	mbParticleSystemsActive = true;
}

cEngineEntityLoadedMeshAggregate::~cEngineEntityLoadedMeshAggregate()
{
	cWorld* pWorld = mpParent->GetEditorWorld()->GetWorld();
	for(int i=0;i<(int)mvLights.size();++i)
		pWorld->DestroyLight(mvLights[i]);
	for(int i=0;i<(int)mvBillboards.size();++i)
		pWorld->DestroyBillboard(mvBillboards[i]);
	for(int i=0;i<(int)mvParticleSystems.size();++i)
		pWorld->DestroyParticleSystem(mvParticleSystems[i]);
	for(int i=0;i<(int)mvSounds.size();++i)
		pWorld->DestroySoundEntity(mvSounds[i]);	
}

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////
// MESH AGGREGATE ENTITY - PUBLIC METHODS
//////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

bool cEngineEntityLoadedMeshAggregate::Create(const tString& asName)
{
	iEditorWorld* pWorld = mpParent->GetEditorWorld();
	cEditorEntityLoader* pLoader = pWorld->GetEditor()->GetEngineEntityLoader();

	mpEntity = pLoader->LoadEntFile(mpParent->GetID(), asName, msFilename, pWorld->GetWorld(), false, true, true, false, true);
	// On a failed load the loader's child lists still hold the previous entity's
	// objects; bail before taking them (the destructor would destroy them).
	if(mpEntity==NULL)
		return false;

	mvLights = pLoader->GetLights();
	mvBillboards = pLoader->GetBillboards();
	mvParticleSystems = pLoader->GetParticleSystems();
	mvSounds = pLoader->GetSounds();

	// "ShowMesh"=false marks a blocker entity — its mesh follows the Blockers
	// visibility toggle (iEngineEntityMesh::UpdateVisibility).
	mbShowMesh = pLoader->GetVarBool("ShowMesh", true);

	for(int i=0;i<(int)mvLights.size();++i)
		mpEntity->AddChild(mvLights[i]);
	for(int i=0;i<(int)mvBillboards.size();++i)
		mpEntity->AddChild(mvBillboards[i]);
	for(int i=0;i<(int)mvParticleSystems.size();++i)
		mpEntity->AddChild(mvParticleSystems[i]);
	for(int i=0;i<(int)mvSounds.size();++i)
		mpEntity->AddChild(mvSounds[i]);

	SetUpMesh();

	return IsCreated();
}

//-----------------------------------------------------------------------

void cEngineEntityLoadedMeshAggregate::Update()
{
	iEngineEntityMesh::Update();

	bool bActive = mpParent->IsActive();
	bool bVisible = mpParent->IsVisible();
	iEditorWorld* pWorld = mpParent->GetEditorWorld();
	bool bLightsVisible = pWorld->GetTypeVisibility(eEditorEntityType_Light);
	bool bPSVisible = pWorld->GetTypeVisibility(eEditorEntityType_ParticleSystem);

	// Children tagged for the other renderer (the retail half of a split lamp
	// light) stay hidden unless the world shows other-renderer objects.
	const unsigned lEditorRendererBit = pWorld->GetWorld()->GetRendererMaskBit();
	const bool bShowOtherRenderer = pWorld->GetShowOtherRendererObjects();
	auto InEditorRenderer = [&](iRenderable* apChild)
	{
		return bShowOtherRenderer || (apChild->GetRendererMask() & lEditorRendererBit) != 0;
	};

	bool bLit = mbLightsActive && bLightsVisible && bActive && bVisible;
	for(int i=0;i<(int)mvLights.size();++i)
		mvLights[i]->SetVisible(bLit && InEditorRenderer(mvLights[i]));

	// Preview-only illumination toggle: when the lamp's own lights are hidden,
	// drop the mesh's illumination maps too so the geometry shows unlit (HPL3
	// editor behavior). Mirrors the per-submesh glow scalar the renderer reads.
	cMeshEntity* pMeshEntity = GetMeshEntity();
	for(int i=0;i<pMeshEntity->GetSubMeshEntityNum();++i)
		pMeshEntity->GetSubMeshEntity(i)->SetIlluminationAmount(bLit ? 1.0f : 0.0f);

	for(int i=0;i<(int)mvParticleSystems.size();++i)
		mvParticleSystems[i]->SetVisible(mbParticleSystemsActive && bPSVisible && bActive && bVisible);

	// Connected billboards follow the Billboard type-visibility toggle (HPL3
	// editor; the billboards were previously left always-visible). (7dc1fc5)
	bool bBillboardsVisible = pWorld->GetTypeVisibility(eEditorEntityType_Billboard);
	for(int i=0;i<(int)mvBillboards.size();++i)
		mvBillboards[i]->SetVisible(mbBillboardsActive && bBillboardsVisible && bActive && bVisible && InEditorRenderer(mvBillboards[i]));
}

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////
// GENERATED MESH ENTITY - CONSTRUCTORS
//////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

cEngineEntityGeneratedMesh::cEngineEntityGeneratedMesh(iEntityWrapper* apParent, cMesh* apMesh) : iEngineEntityMesh(apParent)
{
	SetMesh(apMesh);
}

bool cEngineEntityGeneratedMesh::ReCreate(cMesh* apMesh)
{
	cWorld* pWorld = mpParent->GetEditorWorld()->GetWorld();
	tString sName; 
	if(mpEntity)
	{
		sName = mpEntity->GetName();
		pWorld->DestroyMeshEntity((cMeshEntity*)mpEntity);
		mpEntity = NULL;
	}

	SetMesh(apMesh);

	return Create(sName);
}

cVertexBuffer* cEngineEntityGeneratedMesh::GetVertexBuffer()
{
	return mpMesh->GetSubMesh(0)->GetVertexBuffer();
}

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////
// DECAL ENTITY - CONSTRUCTORS
//////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

cEngineEntityDecal::cEngineEntityDecal(iEntityWrapper* apParent, const tString& asMaterial, const cColor& aColor,
									   const cVector2l& avSubDiv, int alCurrentSubDiv, int alReceiverMask)
	: iEngineEntity(apParent), msMaterial(asMaterial), mColor(aColor), mvSubDiv(avSubDiv),
	  mlCurrentSubDiv(alCurrentSubDiv), mlReceiverMask(alReceiverMask)
{
}

cEngineEntityDecal::~cEngineEntityDecal()
{
	if(mpEntity)
	{
		cWorld* pWorld = mpParent->GetEditorWorld()->GetWorld();
		pWorld->DestroyDecal((cDecal*)mpEntity);
		// Removing a decal invalidates every receiver's baked decal list; request
		// a rebuild (debounced in PrepareFrame) so the GPU projection stays
		// consistent.
		pWorld->MarkDecalAssociationsDirty();
	}
}

//-----------------------------------------------------------------------

bool cEngineEntityDecal::Create(const tString& asName)
{
	// A decal with no material can't project — leave mpEntity NULL so the wrapper
	// shows its icon (matches the legacy "no geometry" behavior).
	if(msMaterial=="")
		return false;

	cWorld* pWorld = mpParent->GetEditorWorld()->GetWorld();
	cDecal* pDecal = pWorld->CreateDecal(asName, msMaterial, mColor, mvSubDiv);
	if(pDecal==NULL)
		return false;

	pDecal->SetCurrentSubDiv(mlCurrentSubDiv);
	pDecal->SetReceiverMask(mlReceiverMask);
	mpEntity = pDecal;

	// The wrapper applies the world matrix (SetMatrix) and calls CompileDecals()
	// after creation, so we don't rebuild the association here.
	return IsCreated();
}

//-----------------------------------------------------------------------

void cEngineEntityDecal::UpdateVisibility()
{
	// cDecal is static set-dressing. NOTE: the GPU projection is baked from
	// cWorld::mvDecals regardless of the renderable's visible flag, so toggling
	// editor visibility does not currently hide the projected decal; the flag is
	// still propagated for any container-side culling.
	((cDecal*)mpEntity)->SetVisible(mpParent->IsVisible() && mpParent->IsCulledByClipPlanes()==false);
}

//-----------------------------------------------------------------------

void cEngineEntityDecal::Draw(cEditorWindowViewport* apViewport, DebugDraw* apFunctions, bool abIsSelected, bool abIsActive, const cColor& aHighlightCol)
{
	if(abIsSelected==false)
		return;

	// The decal volume is the unit cube [-0.5,0.5]^3 mapped by the decal world
	// matrix (translate*rotate*scale) — draw it as the selection box.
	DebugDraw::DebugDrawOptions options;
	options.m_transform = mpEntity->GetWorldMatrix();
	apFunctions->DebugDrawBoxMinMax(cVector3f(-0.5f), cVector3f(0.5f), aHighlightCol, options);
}

//-----------------------------------------------------------------------

void cEngineEntityDecal::SetCurrentSubDiv(int alX)
{
	mlCurrentSubDiv = alX;
	if(mpEntity)
		((cDecal*)mpEntity)->SetCurrentSubDiv(alX);
}

void cEngineEntityDecal::SetReceiverMask(int alMask)
{
	mlReceiverMask = alMask;
	if(mpEntity)
		((cDecal*)mpEntity)->SetReceiverMask(alMask);
}

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////
// ICON ENTITY BASE - CONSTRUCTORS
//////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------


iIconEntity::iIconEntity(iEntityWrapper* apParent, const tString& asIconFile) : iEngineEntity(apParent)
{
	mpIcon = hplNew(cEntityIcon,(apParent, asIconFile));
}

iIconEntity::~iIconEntity()
{
	if(mpIcon)
		hplDelete(mpIcon);
}

bool iIconEntity::Create(const tString& asName)
{
	return true;
}

cBoundingVolume* iIconEntity::GetPickBV(cEditorWindowViewport* apViewport)
{
	if(mpIcon)
		return mpIcon->GetPickBV(apViewport);
	
	return NULL;
}

bool iIconEntity::Check2DBoxIntersect(cEditorWindowViewport* apViewport, const cRect2l& aBox)
{
	return mpIcon && mpIcon->Check2DBoxIntersect(apViewport, aBox);
}

bool iIconEntity::CheckRayIntersect(cEditorWindowViewport* apViewport, cVector3f* apPos, tVector3fVec* apTriangle, float* apT)
{
	return mpIcon && mpIcon->CheckRayIntersect(apViewport, apPos, apTriangle, apT);
}

void iIconEntity::Draw(cEditorWindowViewport* apViewport, 
					  DebugDraw* apFunctions, 
					  bool abIsSelected,
					  bool abIsActive,
					  const cColor& aHighlightCol)
{
	if(mpIcon) mpIcon->DrawIcon(apViewport, apFunctions, NULL, abIsSelected, mpParent->GetPosition(), mpParent->IsActive());
}

