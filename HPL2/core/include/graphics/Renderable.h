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

#ifndef HPL_RENDERABLE_H
#define HPL_RENDERABLE_H

#include "math/MathTypes.h"
#include "graphics/GraphicsTypes.h"
#include "graphics/RendererMask.h"
#include "system/SystemTypes.h"
#include "scene/SceneTypes.h"

#include "scene/Entity3D.h"
#include <span>


namespace hpl {

	class cMaterial;
	class cVertexBuffer;
	class cBoundingVolume;
	class cFrustum;
	class iLight;
	class cCamera;
	class cRenderList;
	class iPhysicsBody;
	class iRenderer;

	//------------------------------------------

	class iRenderable : public iEntity3D
	{
	public:
		iRenderable(const tString &asName);
		virtual ~iRenderable() {}

		// Stable per-instance cookie (random, assigned in the constructor). Used
		// as a renderer cache key in place of the renderable pointer, which is
		// unsafe across free + realloc (a reused address would alias the old slot).
		uint64_t GetUniqueCookie() const { return mUniqueCookie; }

		virtual cMaterial *GetMaterial()=0;
		virtual cVertexBuffer* GetVertexBuffer()=0;

		virtual bool CollidesWithBV(cBoundingVolume *apBV);
		virtual bool CollidesWithFrustum(cFrustum *apFrustum);

		virtual cMatrixf* GetModelMatrix(cFrustum *apFrustum)=0;

		virtual eRenderableType GetRenderType()=0;

		virtual void UpdateGraphicsForFrame(float afFrameTime){}
		virtual bool UpdateGraphicsForViewport(cFrustum *apFrustum,float afFrameTime){ return true;}

		virtual void SetRenderFlagBit(tRenderableFlag alFlagBit, bool abSet);
		bool GetRenderFlagBit(tRenderableFlag alFlagBit){ return (mlRenderFlags & alFlagBit)!=0;} 
		inline tRenderableFlag GetRenderFlags() const { return mlRenderFlags;}

		// Renderer backends this object belongs to (kRendererMask*). Loaders skip
		// objects whose mask excludes the running backend; the default is all.
		virtual void SetRendererMask(unsigned alMask);
		unsigned GetRendererMask() const { return mRendererMask; }
		bool IsRendererEnabled(unsigned alBackendBit) const { return IsRendererMaskEnabled(mRendererMask, alBackendBit); }
		bool IsLegacyRendererEnabled() const { return IsRendererEnabled(kRendererMaskStandard); }
		bool IsOverdriveEnabled() const { return IsRendererEnabled(kRendererMaskOverdrive); }

		virtual bool IsVisible(){ return mbIsVisible && mfCoverageAmount >0; }
		void SetVisible(bool abVisible);
		/**
		 * This is needed since IsVisible does not return the actual var value!
		 */
		bool GetVisibleVar(){ return mbIsVisible;}
		virtual void OnChangeVisible(){}
		
		virtual void SetIlluminationAmount(float afX){ mfIlluminationAmount = afX;}
		inline float GetIlluminationAmount()const { return mfIlluminationAmount;}
		
		void SetCoverageAmount(float afX);
		inline float GetCoverageAmount()const { return mfCoverageAmount;}

		void SetLargePlaneSurfacePlacement(int alX){ mlLargePlaneSurfacePlacement = alX;};
		inline int GetLargePlaneSurfacePlacement(){ return mlLargePlaneSurfacePlacement;};

		/**
		 * Should return a different number each time the renderable model matrix is updated. never -1
		 * \return
		 */
		virtual int GetMatrixUpdateCount()=0;

		inline void SetModelMatrixPtr(cMatrixf *apMtx){ mpModelMatrix = apMtx;}
		inline cMatrixf* GetModelMatrixPtr() const { return mpModelMatrix;}

		inline float GetViewSpaceZ() const { return mfViewSpaceZ;}
		inline void SetViewSpaceZ(float afZ){ mfViewSpaceZ = afZ;}

		cMatrixf* GetInvModelMatrix();

		inline void SetPrevMatrix(const cMatrixf& a_mtxPrev){m_mtxPrevious = a_mtxPrev;}
		inline cMatrixf& GetPrevMatrix(){ return m_mtxPrevious;}

		const cVector3f& GetCalcScale();

		void SetStatic(bool abX){ mbStatic = abX;}
		bool IsStatic() const { return mbStatic;}

		inline int GetRenderFrameCount() const { return mlRenderFrameCount;}
		inline void SetRenderFrameCount(const int alCount) { mlRenderFrameCount = alCount;}
		
		bool GetIsOneSided(){ return mbIsOneSided;}
		const cVector3f& GetOneSidedNormal(){ return mvOneSidedNormal;}

		void SetRenderableUserData(void* apData) { mpRenderableUserData = apData; }
		void* GetRenderableUserData() { return mpRenderableUserData; }

		// Precomputed static decal association (cWorld::Compile): [offset,count)
		// into cWorld::GetDecalObjectIndices(). count==0 means no decals (all
		// dynamic objects, and statics no decal was clipped to). Uploaded into
		// UniformObject.decalList; read by the decal-composite in the GI pass.
		inline void SetDecalList(int alOffset, int alCount){ mlDecalListOffset = alOffset; mlDecalListCount = alCount; }
		inline int GetDecalListOffset() const { return mlDecalListOffset; }
		inline int GetDecalListCount() const { return mlDecalListCount; }

	protected:
		uint64_t mUniqueCookie;

		cMatrixf m_mtxInvModel;
		cMatrixf m_mtxPrevious;
		cMatrixf *mpModelMatrix;

		bool mbIsOneSided;
		cVector3f mvOneSidedNormal;

		int mlLastMatrixCount;

		tRenderableFlag mlRenderFlags;
		unsigned mRendererMask = kRendererMaskAll;

		bool mbIsVisible;

		bool mbStatic;

		bool mbForceShadow;
		
		int mlLargePlaneSurfacePlacement;

		int mlRenderFrameCount;

		int mlCalcScaleMatrixCount;
		cVector3f mvCalcScale;

		float mfViewSpaceZ;

		float mfIlluminationAmount;
		float mfCoverageAmount;

		void* mpRenderableUserData;

		int mlDecalListOffset = 0;
		int mlDecalListCount = 0;
	};
};
#endif // HPL_RENDERABLE_H
