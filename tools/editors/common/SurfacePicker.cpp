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

#include "SurfacePicker.h"
#include "EditorTypes.h"
#include "EditorWorld.h"
#include "EditorBaseClasses.h"
#include "EditorWindowViewport.h"
#include "EntityWrapper.h"
#include "EditorHelper.h"
#include "graphics/DebugDraw.h"

cSurfacePicker::cSurfacePicker(iEditorWorld* apWorld) : cEntityPicker(apWorld)
{
	mvAffectedSurface.insert(mvAffectedSurface.end(), eSurfaceType_LastEnum, true);
	mfRayLength = 15;

	mpClosestMeshEntity = NULL;
	mpClosestSurfaceEntity = NULL;

	mbAverageNormal = true;
	mAvgVolume.SetSize(0.1f);
	mfBackfacingAngleThreshold = -0.1f;
}

void cSurfacePicker::SetAffectType(int alTypeID, bool abX, bool abTranslate)
{
	int lTypeID = abTranslate? ToSurfaceTypeID(alTypeID) : alTypeID;
	if(lTypeID==-1) return;

	mvAffectedSurface[lTypeID] = abX;
}

//----------------------------------------------------------------------

cMatrixf cSurfacePicker::GetOrientationMatrix()
{
	return cMath::MatrixUnitVectors(GetRightVector(), GetUpVector(), GetForwardVector(), 0);
}

//----------------------------------------------------------------------

int cSurfacePicker::ToSurfaceTypeID(int alTypeID)
{
	switch(alTypeID)
	{
	case eEditorEntityType_StaticObject:
		return eSurfaceType_StaticObject;
	case eEditorEntityType_Primitive:
		return eSurfaceType_Primitive;
	case eEditorEntityType_Entity:
		return eSurfaceType_Entity;
	}

	return -1;
}

//----------------------------------------------------------------------

void cSurfacePicker::OnDraw(DebugDraw* apFunctions)
{
	// apFunctions->SetMatrix(NULL);
	// apFunctions->SetDepthTest(false);


	if(HasPickedSurface())
	{
		apFunctions->DebugDrawSphere(cMath::ToForgeVec3(mvIntersection), 0.005f, Vector4(0,0,1,1));
		//apFunctions->GetLowLevelGfx()->DrawLine(mvIntersection, mvIntersection+mvTriangleNormal, cColor(0,1,0,1));
		//apFunctions->GetLowLevelGfx()->DrawLine(mvIntersection, mvIntersection+mvAverageUp, cColor(0,1,1,1));
		//apFunctions->GetLowLevelGfx()->DrawLine(mvIntersection, mvIntersection+mvRight, cColor(1,0,0,1));
		//apFunctions->GetLowLevelGfx()->DrawLine(mvIntersection, mvIntersection+mvForward, cColor(0,0,1,1));


		/*
		for(int i=0;i<(int)mvTrianglesToAverage.size();i+=3)
		{
			const cVector3f& v1 = mvTrianglesToAverage[i];
			const cVector3f& v2 = mvTrianglesToAverage[i+1];
			const cVector3f& v3 = mvTrianglesToAverage[i+2];

			apFunctions->GetLowLevelGfx()->DrawLine(v1, v2, cColor(0,1,1,1));
			apFunctions->GetLowLevelGfx()->DrawLine(v2, v3, cColor(0,1,1,1));
			apFunctions->GetLowLevelGfx()->DrawLine(v3, v1, cColor(0,1,1,1));
		}
		*/

		/*cMatrixf mtxTransform = GetOrientationMatrix();
		mtxTransform.SetTranslation(mvIntersection);
		apFunctions->SetMatrix(&mtxTransform);
		apFunctions->GetLowLevelGfx()->DrawLine(0, cVector3f(1,0,0), cColor(1,0,0,1));
		apFunctions->GetLowLevelGfx()->DrawLine(0, cVector3f(0,1,0), cColor(0,1,0,1));
		apFunctions->GetLowLevelGfx()->DrawLine(0, cVector3f(0,0,1), cColor(0,0,1,1));
		*/
	}
}

void cSurfacePicker::Iterate()
{
	/////////////////////////////
	// Set up line
	cEditorWindowViewport* pViewport = mpWorld->GetEditor()->GetFocusedViewport();
	const cVector3f& vStart = pViewport->GetUnprojectedStart();
	cVector3f vEnd = vStart + pViewport->GetUnprojectedDir()*mfRayLength;

    cBoundingVolume lineBV;
	lineBV.SetLocalMinMax(cMath::Vector3Min(vStart, vEnd), cMath::Vector3Max(vStart, vEnd));

	/////////////////////////////
	// Set up variables
	mfMinT = 9999.0f;
	mpClosestMeshEntity = NULL;
	mpClosestSurfaceEntity = NULL;
	mvFilteredEntities.clear();
	mvFilteredSubMeshes.clear();
	mvTriangle.clear();
	int lTriangleIndex;

	/////////////////////////////
	// Get Containers
    cWorld *pWorld = mpWorld->GetWorld();
	iRenderableContainer *pContainers[2] ={
		pWorld->GetRenderableContainer(eWorldContainerType_Dynamic),
		pWorld->GetRenderableContainer(eWorldContainerType_Static),
	};

	/////////////////////////////
	// Search nodes in containers
	for(int i=0; i<2; ++i)
	{
		pContainers[i]->UpdateBeforeRendering();
		IterateRenderableNode(pContainers[i]->GetRoot(), vStart, vEnd, &lineBV, &lTriangleIndex);
	}

	if(mpClosestMeshEntity==NULL)
		return;

	iVertexBuffer* pVB = mpClosestMeshEntity->GetSubMesh()->GetVertexBuffer();
	float* pVertices = pVB->GetFloatArray(eVertexBufferElement_Position);
	int lStride = pVB->GetElementNum(eVertexBufferElement_Position);
	unsigned int* pIndices = pVB->GetIndices();
	for(int i=0;i<3;++i)
	{
		int lBaseIndex = pIndices[lTriangleIndex+i]*lStride;
		mvTriangle.push_back(cMath::MatrixMul(mpClosestMeshEntity->GetWorldMatrix(),
												cVector3f(pVertices[lBaseIndex],pVertices[lBaseIndex+1], pVertices[lBaseIndex+2])));
	}

	cVector3f vEdge1 = mvTriangle[2]-mvTriangle[0];
	cVector3f vEdge2 = mvTriangle[1]-mvTriangle[0];
	mvTriangleNormal = cMath::Vector3Cross(vEdge1, vEdge2);
	mvTriangleNormal.Normalize();

	cVector3f vRight, vUp, vFwd;
	cVector3f vWorldRight = cVector3f(1,0,0);

	vUp = mvTriangleNormal;
	vRight = vWorldRight - cMath::Vector3Cross(cMath::Vector3Dot(vWorldRight, mvTriangleNormal), mvTriangleNormal);
	vRight = cMath::Vector3Project(vRight, vWorldRight);
	vFwd = cMath::Vector3Cross(vRight, mvTriangleNormal);

	cMath::Vector3OrthonormalizeBasis(vUp, vRight, vFwd, mvTriangleNormal, mvRight, mvForward);


	/*float fDP = cMath::Vector3Dot(vForward, mvTriangleNormal);
	float fAbsDP = cMath::Abs(fDP);
	if(fAbsDP-1<kEpsilonf && fAbsDP-1>-kEpsilonf)
		vForward = cVector3f(0,-1,0);
		*/

	/*
	if(vDiffRight.Length()<kEpsilonf)
	{
		float fDP = cMath::Vector3Dot(mvTriangleNormal, vRight);
		vRight = cVector3f(0,-1,0) * cMath::Sign(fDP);
	}
	else if(vDiffForward.Length()<kEpsilonf)
	{
		float fDP = cMath::Vector3Dot(mvTriangleNormal, vForward);
		vForward = cVector3f(0,-1,0) * cMath::Sign(fDP);
	}
	*/

	//cMath::Vector3OrthonormalizeBasis(mvTriangleNormal, vRight, vForward, mvTriangleNormal, mvRight, mvForward);

	//cVector3f vRight = cMath::Vector3Cross(mvTriangleNormal, vForward);
	//mvForward = cMath::Vector3Cross(vRight, mvTriangleNormal);
	//mvForward.Normalize();
	//mvRight = cMath::Vector3Cross(mvTriangleNormal, mvForward);
	//mvRight.Normalize();
	//vForward = cMath::Vector3Project(vEdge2, vForward);

	/*mvForward = cMath::Vector3Cross(mvTriangleNormal, vRight);
	mvForward.Normalize();
	mvRight = cMath::Vector3Cross(mvForward, mvTriangleNormal);
	mvRight.Normalize();
	*/

	//Log("[SurfacePicker] Right: (%s) Up: (%s) Fwd: (%s)\n", mvRight.ToFileString().c_str(),
	//														mvTriangleNormal.ToFileString().c_str(),
	//														mvForward.ToFileString().c_str());

	if(mbAverageNormal==false)
		return;

	mAvgVolume.SetPosition(GetPositionInSurface());
	/////////////////////////////
	// Search nodes in containers
	mvSubMeshesToAverage.clear();
	for(int i=0; i<2; ++i)
	{
		pContainers[i]->UpdateBeforeRendering();
		IterateRenderableNode(pContainers[i]->GetRoot(), 0, 0, &mAvgVolume, NULL, true);
	}

	ComputeAverageNormal(mvTriangleNormal);
}

void cSurfacePicker::IterateRenderableNode(iRenderableContainerNode *apNode, const cVector3f& avStart, const cVector3f& avEnd, cBoundingVolume *apBV, int* apTriIndex, bool abAveragingNormal)
{
	apNode->UpdateBeforeUse();

	if(	apNode->GetParent()!=NULL)
	{
		if(cMath::CheckAABBIntersection(apNode->GetMin(), apNode->GetMax(), apBV->GetMin(), apBV->GetMax())==false) return;

		if(abAveragingNormal==false && cMath::CheckPointInAABBIntersection(avStart, apNode->GetMin(), apNode->GetMax())==false)
		{
			float fT=0;
			if(cMath::CheckAABBLineIntersection(apNode->GetMin(), apNode->GetMax(), avStart, avEnd,NULL, &fT)==false) return;
			if(fT > mfMinT) return;
		}
	}

	/////////////////////////////
	//Iterate objects
	if(apNode->HasObjects())
	{
		tRenderableListIt it = apNode->GetObjectList()->begin();
		for(; it != apNode->GetObjectList()->end(); ++it)
		{
			iRenderable *pObject = *it;
			if(pObject->GetRenderType() != eRenderableType_SubMesh) continue;

			cSubMeshEntity* pSubMesh = (cSubMeshEntity*)pObject;
			iEntityWrapper* pEnt = (iEntityWrapper*) pSubMesh->GetUserData();

			if(pEnt==NULL || pEnt->IsVisible()==false || IsAffected(pEnt->GetTypeID())==false)
				continue;

			//mvFilteredEntities.push_back(pEnt);
			//mvFilteredSubMeshes.push_back(pSubMesh);

			if(CheckObjectIntersection(pObject, avStart, avEnd, apBV, apTriIndex, abAveragingNormal) && abAveragingNormal==false)
				mpClosestSurfaceEntity = pEnt;
		}
	}

	////////////////////////
	//Iterate children
	if(apNode->HasChildNodes())
	{
		tRenderableContainerNodeListIt childIt = apNode->GetChildNodeList()->begin();
		for(; childIt != apNode->GetChildNodeList()->end(); ++childIt)
		{
			iRenderableContainerNode *pChildNode = *childIt;
			IterateRenderableNode(pChildNode, avStart, avEnd, apBV, apTriIndex, abAveragingNormal);
		}
	}

}

bool cSurfacePicker::CheckObjectIntersection(iRenderable *apObject, const cVector3f& avStart, const cVector3f& avEnd, cBoundingVolume *apBV, int* apTriIndex, bool abAveragingNormal)
{
	cBoundingVolume *pObjectBV = apObject->GetBoundingVolume();

	cVector3f vIntersection;

	if(cMath::CheckBVIntersection(*pObjectBV, *apBV)==false) return false;

	if(abAveragingNormal)
	{
		mvSubMeshesToAverage.push_back((cSubMeshEntity*)apObject);
		return true;
	}

	float fT=0;

	if(cMath::CheckPointInBVIntersection(avStart, *pObjectBV)==false)
	{
		if(cMath::CheckAABBLineIntersection(pObjectBV->GetMin(), pObjectBV->GetMax(), avStart, avEnd, &vIntersection, &fT)==false) return false;
		if(fT > mfMinT) return false;
	}

	int lTriIndex;
	cMatrixf mtxInvModel = cMath::MatrixInverse(apObject->GetWorldMatrix());
	bool bIntersect = cMath::CheckLineTriVertexBufferIntersection(	avStart, avEnd,mtxInvModel, apObject->GetVertexBuffer(),&vIntersection, &fT, &lTriIndex,true);
	if(bIntersect==false || fT > mfMinT) return false;

	mfMinT = fT;
	mvIntersection = vIntersection;
	mpClosestMeshEntity = static_cast<cSubMeshEntity*>(apObject);
	if(apTriIndex)
		*apTriIndex = lTriIndex;

	return true;
}

void cSurfacePicker::ComputeAverageNormal(const cVector3f& avBaseNormal)
{
	tVector3fVec vNormalsToAverage;
	float fOneOverThree = 1.0f/3.0f;

	mvTrianglesToAverage.clear();

	for(int i=0;i<(int)mvSubMeshesToAverage.size();++i)
	{
		cSubMeshEntity* pSubMesh = mvSubMeshesToAverage[i];
		cMatrixf mtxWorldMatrix = pSubMesh->GetWorldMatrix();
		cMatrixf mtxInvSubMeshMatrix = cMath::MatrixInverse(mtxWorldMatrix);
		cMatrixf mtxWorldMatrixNoTranslation = mtxWorldMatrix;
		mtxWorldMatrixNoTranslation.SetTranslation(0);

		cVector3f vTransPos = cMath::MatrixMul(mtxInvSubMeshMatrix, mvIntersection);
		mAvgVolume.SetTransform(mtxInvSubMeshMatrix);
		mAvgVolume.SetPosition(vTransPos);

		iVertexBuffer* pVB = pSubMesh->GetVertexBuffer();
		float* pVertices = pVB->GetFloatArray(eVertexBufferElement_Position);
		int lPosStride = pVB->GetElementNum(eVertexBufferElement_Position);

		unsigned int* pIndices = pVB->GetIndices();
		for(int i=0;i<pVB->GetIndexNum();i+=3)
		{
			tVector3fVec vTriangle;
			for(int j=0;j<3;++j)
			{
				int lBaseIndex = pIndices[i+j];
				int lPosIndex = lBaseIndex*lPosStride;
				vTriangle.push_back(cVector3f(pVertices[lPosIndex],pVertices[lPosIndex+1], pVertices[lPosIndex+2]));
			}
			cVector3f vTriNormal = cMath::Vector3Cross(vTriangle[2]-vTriangle[0], vTriangle[1]-vTriangle[0]);
			vTriNormal.Normalize();

			if(cMath::Vector3Dot(vTriNormal, avBaseNormal)<= mfBackfacingAngleThreshold)
				continue;

			cBoundingVolume triBV;
			triBV.SetLocalMinMax(cMath::Vector3Min(vTriangle[0], cMath::Vector3Min(vTriangle[1], vTriangle[2])),
									cMath::Vector3Max(vTriangle[0], cMath::Vector3Max(vTriangle[1], vTriangle[2])));

			if(cMath::CheckBVIntersection(triBV, mAvgVolume)==false)
				continue;

			vNormalsToAverage.push_back(cMath::MatrixMul3x3(mtxWorldMatrix,
															vTriNormal));

			mvTrianglesToAverage.push_back(cMath::MatrixMul(mtxWorldMatrix, vTriangle[0]));
			mvTrianglesToAverage.push_back(cMath::MatrixMul(mtxWorldMatrix, vTriangle[1]));
			mvTrianglesToAverage.push_back(cMath::MatrixMul(mtxWorldMatrix, vTriangle[2]));
		}
	}

	if(vNormalsToAverage.empty()) return;

	mvAverageUp = 0;
	int lNumNormals = (int)vNormalsToAverage.size();
	float fOneOverNumNormals = 1.0f*(float)lNumNormals;
	for(int i=0;i<lNumNormals;++i)
		mvAverageUp += vNormalsToAverage[i]*fOneOverNumNormals;

	cVector3f vRight, vUp, vFwd;
	cVector3f vWorldRight = cVector3f(1,0,0);

	vUp = mvAverageUp;
	vRight = vWorldRight - cMath::Vector3Cross(cMath::Vector3Dot(vWorldRight, vUp), vUp);
	vRight = cMath::Vector3Project(vRight, vWorldRight);
	vFwd = cMath::Vector3Cross(vRight, vUp);

	cMath::Vector3OrthonormalizeBasis(vUp, vRight, vFwd, mvAverageUp, mvAverageRight, mvAverageForward);
}

void cSurfacePicker::SetAverageVolumeSize(const cVector3f& avX)
{
	mAvgVolume.SetSize(avX);
}

void cSurfacePicker::SetAverageBackfacingAngleThreshold(float afAngle)
{
	mfBackfacingAngleThreshold = cos(afAngle);
}

bool cSurfacePicker::IsAffected(int alTypeID, bool abTranslate)
{
	int lTypeID = abTranslate? ToSurfaceTypeID(alTypeID) : alTypeID;
	if(lTypeID==-1) return false;

	return mvAffectedSurface[lTypeID];
}
