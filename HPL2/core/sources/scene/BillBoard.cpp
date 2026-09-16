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

#include "scene/BillBoard.h"

#include "resources/Resources.h"
#include "resources/MaterialManager.h"
#include "resources/FileSearcher.h"

#include "graphics/VertexBuffer.h"
#include "graphics/Material.h"
#include "graphics/Graphics.h"
#include "graphics/Renderer.h"
#include "graphics/VertexBuffer.h"

#include "scene/Camera.h"
#include "scene/World.h"
#include "scene/Scene.h"
#include "math/Math.h"
#include "system/String.h"

#include "engine/Engine.h"

#include <algorithm>

namespace hpl {

	//////////////////////////////////////////////////////////////////////////
	// CONSTRUCTORS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	cBillboard::cBillboard(const tString asName,const cVector2f& avSize,eBillboardType aType, cResources *apResources,cGraphics *apGraphics) :
	iRenderable(asName)
	{
		mpMaterialManager = apResources->GetMaterialManager();
		
		mvSize = avSize;
		mvAxis = cVector3f(0,1,0);

		mColor = cColor(1,1,1,1);
		mfForwardOffset =0;
		mfHaloAlpha = 1.0f;
	
		mType = aType;

		mlLastRenderCount = -1;

		mpVtxBuffer = new cVertexBuffer(eVertexBufferType_Hardware,eVertexBufferDrawType_Tri, eVertexBufferUsageType_Dynamic,4,6);
		
		mpVtxBuffer->CreateElementArray(eVertexBufferElement_Position,eVertexBufferElementFormat_Float,4);
		mpVtxBuffer->CreateElementArray(eVertexBufferElement_Normal,eVertexBufferElementFormat_Float,3);
		mpVtxBuffer->CreateElementArray(eVertexBufferElement_Color0,eVertexBufferElementFormat_Float,4);
		mpVtxBuffer->CreateElementArray(eVertexBufferElement_Texture0,eVertexBufferElementFormat_Float,3);

		cVector3f vCoords[4] = {cVector3f((mvSize.x/2),-(mvSize.y/2),0),
								cVector3f(-(mvSize.x/2),-(mvSize.y/2),0),
								cVector3f(-(mvSize.x/2),(mvSize.y/2),0),
								cVector3f((mvSize.x/2),(mvSize.y/2),0)};

		cVector3f vTexCoords[4] = {cVector3f(1,-1,0),
								cVector3f(-1,-1,0),
								cVector3f(-1,1,0),
								cVector3f(1,1,0)};
		for(int i=0;i<4;i++)
		{
			mpVtxBuffer->AddVertexVec3f(eVertexBufferElement_Position, vCoords[i]);
			mpVtxBuffer->AddVertexColor(eVertexBufferElement_Color0, cColor(1,1,1,1));
			mpVtxBuffer->AddVertexVec3f(eVertexBufferElement_Texture0, (vTexCoords[i] + cVector2f(1,1))/2 );
			mpVtxBuffer->AddVertexVec3f(eVertexBufferElement_Normal,cVector3f(0,0,1));
		}

		for(int i=0;i<3;i++) mpVtxBuffer->AddIndex(i);
		for(int i=2;i<5;i++) mpVtxBuffer->AddIndex(i==4?0:i);

		//If the type is fixed, then we need a backside too
		//To do this, just all all the same indices in reversed order.
		if(mType == eBillboardType_FixedAxis)
		{
			for(int i=2; i>=0; i--) mpVtxBuffer->AddIndex(i);
			for(int i=4; i>=2; i--) mpVtxBuffer->AddIndex(i==4?0:i);
		}

		mpVtxBuffer->Compile(eVertexCompileFlag_CreateTangents);

		mbIsHalo = false;
		mvHaloSourceSize = 1;
		mpHaloSourceBV = NULL;
		mlHaloBVMatrixCount = -1;
		mbHaloSizeUpdated = true;

		mBoundingVolume.SetSize(cVector3f(mvSize.x, mvSize.y, mvSize.x));
	}

	//-----------------------------------------------------------------------

	cBillboard::~cBillboard()
	{
		// mpMaterial (SharedResourceHandle) releases its reference automatically.
		if(mpVtxBuffer) hplDelete(mpVtxBuffer);
		if(mpHaloSourceBV) hplDelete(mpHaloSourceBV);
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PUBLIC METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	void cBillboard::SetSize(const cVector2f& avSize)
	{
		mvSize = avSize;
		mBoundingVolume.SetSize(cVector3f(mvSize.x, mvSize.y, mvSize.x));

		float *pPos = mpVtxBuffer->GetFloatArray(eVertexBufferElement_Position);
		
		cVector3f vCoords[4] = {cVector3f((mvSize.x/2),-(mvSize.y/2),0),
								cVector3f(-(mvSize.x/2),-(mvSize.y/2),0),
								cVector3f(-(mvSize.x/2),(mvSize.y/2),0),
								cVector3f((mvSize.x/2),(mvSize.y/2),0)};

		for(int i=0; i<4;++i)
		{
			pPos[0] = vCoords[i].x;
			pPos[1] = vCoords[i].y;
			pPos[2] = vCoords[i].z;
			pPos+=4;
		}

		mpVtxBuffer->UpdateData(eVertexElementFlag_Position,false);

		if(mType == eBillboardType_Axis) SetAxis(mvAxis);

		SetTransformUpdated();
	}

	//-----------------------------------------------------------------------

	void cBillboard::SetAxis(const cVector3f& avAxis)
	{
		mvAxis = avAxis;
		mvAxis.Normalize();
		
		//This is a quick fix so the bounding box is correct for non up-pointing axises
		if(mType == eBillboardType_Axis && mvAxis != cVector3f(0,1,0))
		{
			float fMax = mvSize.x;
			if(fMax < mvSize.y) fMax = mvSize.y;
			
			fMax *= kSqrt2f;

			mBoundingVolume.SetSize(fMax);

			SetTransformUpdated();
		}
	}

	//-----------------------------------------------------------------------

	void cBillboard::SetColor(const cColor &aColor)
	{
		if(mColor == aColor) return;

		mColor = aColor;

		float *pColors = mpVtxBuffer->GetFloatArray(eVertexBufferElement_Color0);

		for(int i=0; i<4;++i)
		{
			pColors[0] = mColor.r * mfHaloAlpha;
			pColors[1] = mColor.g * mfHaloAlpha;
			pColors[2] = mColor.b * mfHaloAlpha;
			pColors[3] = mColor.a * mfHaloAlpha;
			pColors+=4;
		}

		mpVtxBuffer->UpdateData(eVertexElementFlag_Color0,false);
	}

	//-----------------------------------------------------------------------

	void cBillboard::SetHaloAlpha(float afX)
	{
		if(mfHaloAlpha == afX)
		{
			return;
		}

		mfHaloAlpha = afX;

		float *pColors = mpVtxBuffer->GetFloatArray(eVertexBufferElement_Color0);

		for(int i=0; i<4;++i)
		{
			pColors[0] = mColor.r * mfHaloAlpha;
			pColors[1] = mColor.g * mfHaloAlpha;
			pColors[2] = mColor.b * mfHaloAlpha;
			pColors[3] = mColor.a * mfHaloAlpha;
			pColors+=4;
		}

		mpVtxBuffer->UpdateData(eVertexElementFlag_Color0,false);
	}

	//-----------------------------------------------------------------------

	void cBillboard::SetForwardOffset(float afOffset)
	{
		mfForwardOffset = afOffset;
	}

	//-----------------------------------------------------------------------

	void cBillboard::SetMaterial(SharedResourceHandle<cMaterial> apMaterial)
	{
		mpMaterial = std::move(apMaterial);
	}

	//-----------------------------------------------------------------------

	cMatrixf* cBillboard::GetModelMatrix(cFrustum *apFrustum)
	{
		if(apFrustum==NULL)return &GetWorldMatrix();

		//////////////////////
		// Fixed Axis
		if(mType == eBillboardType_FixedAxis)
		{
			return &GetWorldMatrix();
		}

		//////////////////////
		// Set up for all rotating billboards
		m_mtxTempTransform = GetWorldMatrix();
		cVector3f vForward, vRight, vUp;

		cVector3f vCameraForward = apFrustum->GetOrigin() - GetWorldPosition();
		vCameraForward.Normalize();

		//////////////////////
		// Point
		if(mType == eBillboardType_Point)
		{
			vForward = vCameraForward;
			vRight = cMath::Vector3Cross(apFrustum->GetViewMatrix().GetUp(), vForward);
			vUp = cMath::Vector3Cross(vForward,vRight);
		}
		//////////////////////
		// Axis
		else if(mType == eBillboardType_Axis)
		{
			vUp = cMath::MatrixMul(GetWorldMatrix().GetRotation(),mvAxis);
			vUp.Normalize();
			
			if(vUp == vCameraForward)
			{
				vRight = cMath::Vector3Cross(vUp, vCameraForward);
				Warning("Billboard Right vector is not correct! Contact programmer!\n");
			}
			else
				vRight = cMath::Vector3Cross(vUp, vCameraForward);

			vRight.Normalize();
			vForward = cMath::Vector3Cross(vRight, vUp);

			//vForward.Normalize();
			//vUp.Normalize();
		}

		if(mfForwardOffset!=0)
		{
			cVector3f vPos = m_mtxTempTransform.GetTranslation();
			vPos +=  vCameraForward * mfForwardOffset;
			m_mtxTempTransform.SetTranslation(vPos);
		}

		//Set right vector
		m_mtxTempTransform.m[0][0] = vRight.x;
		m_mtxTempTransform.m[1][0] = vRight.y;
		m_mtxTempTransform.m[2][0] = vRight.z;

		//Set up vector
		m_mtxTempTransform.m[0][1] = vUp.x;
		m_mtxTempTransform.m[1][1] = vUp.y;
		m_mtxTempTransform.m[2][1] = vUp.z;

		//Set forward vector
		m_mtxTempTransform.m[0][2] = vForward.x;
		m_mtxTempTransform.m[1][2] = vForward.y;
		m_mtxTempTransform.m[2][2] = vForward.z;

		return &m_mtxTempTransform;
	}

	//-----------------------------------------------------------------------

	int cBillboard::GetMatrixUpdateCount()
	{
		return GetTransformUpdateCount();
	}

	//-----------------------------------------------------------------------


	bool cBillboard::IsVisible()
	{
		if(mColor.r <= 0 && mColor.g <= 0 && mColor.b <= 0) return false;

		return mbIsVisible;
	}

	//-----------------------------------------------------------------------

	// Inside area over total area of a normalized clip rectangle, the inside
	// part clamped to the [-1,1] screen square.
	static float HaloClipRectCoverage(cVector3f avMin, cVector3f avMax)
	{
		const cVector3f vTotalSize = avMax - avMin;
		avMin.x = std::max(avMin.x, -1.0f);
		avMin.y = std::max(avMin.y, -1.0f);
		avMax.x = std::min(avMax.x, 1.0f);
		avMax.y = std::min(avMax.y, 1.0f);
		const cVector3f vInsideSize = avMax - avMin;
		const float fTotalArea = vTotalSize.x * vTotalSize.y;
		if(fTotalArea <= 0 || vInsideSize.x <= 0 || vInsideSize.y <= 0) return 0;
		return (vInsideSize.x * vInsideSize.y) / fTotalArea;
	}

	float cBillboard::GetHaloScreenCoverage(cFrustum *apFrustum)
	{
		if(mbIsHalo == false || mpHaloSourceBV == NULL || apFrustum == NULL) return 0;

		if(mbHaloSizeUpdated)
		{
			mpHaloSourceBV->SetSize(mvHaloSourceSize);
			mbHaloSizeUpdated = false;
		}
		if(mlHaloBVMatrixCount != GetTransformUpdateCount())
		{
			mpHaloSourceBV->SetTransform(GetWorldMatrix());
			mlHaloBVMatrixCount = GetTransformUpdateCount();
		}

		cVector3f vMin, vMax;
		if(cMath::GetNormalizedClipRectFromBV(vMin, vMax, *mpHaloSourceBV, apFrustum, 0) == false) return 0;
		return HaloClipRectCoverage(vMin, vMax);
	}

	//-----------------------------------------------------------------------

	void cBillboard::SetIsHalo(bool abX)
	{
		mbIsHalo = abX;

		if(mbIsHalo)
		{
			mfHaloAlpha = 1; //THis is to make sure that the new alpha is set to the mesh.
			SetHaloAlpha(0);

			//Create source bv
			if(mpHaloSourceBV == NULL) mpHaloSourceBV = hplNew(cBoundingVolume, ());
		}
		else
		{
			//Delete source bv
			if(mpHaloSourceBV)
			{
				hplDelete(mpHaloSourceBV);
				mpHaloSourceBV = NULL;
			}
		}
	}

	//-----------------------------------------------------------------------

	void cBillboard::SetHaloSourceSize(const cVector3f &avSize)
	{
		mvHaloSourceSize = avSize;

		mbHaloSizeUpdated = true;
	}


	//-----------------------------------------------------------------------


	//////////////////////////////////////////////////////////////////////////
	// PRIVATE METHODS
	//////////////////////////////////////////////////////////////////////////

	
	//-----------------------------------------------------------------------

	

	//-----------------------------------------------------------------------

}
