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

#ifndef HPL_PARTICLE_EMITTER_H
#define HPL_PARTICLE_EMITTER_H

#include "graphics/Renderable.h"
#include "scene/Entity3D.h"
#include "math/MathTypes.h"
#include "system/SystemTypes.h"
#include "graphics/GraphicsTypes.h"

namespace hpl {

	class cFrameSubImage;
	class cGraphics;
	class cResources;
	class cWorld;
	class cParticleSystem;

	//-------------------------------------------------------------------

	enum ePEType
	{
		ePEType_Normal,
		ePEType_Beam,
		ePEType_LastEnum,
	};


	//-------------------------------------------------------------------

	enum eParticleEmitterType
	{
		eParticleEmitterType_FixedPoint,
		eParticleEmitterType_DynamicPoint,
		eParticleEmitterType_Line,
		eParticleEmitterType_Axis,
		eParticleEmitterType_LastEnum,
	};

	enum eParticleEmitterCoordSystem
	{
		eParticleEmitterCoordSystem_World,
		eParticleEmitterCoordSystem_Local,
		eParticleEmitterCoordSystem_LastEnum,
	};

	//------------------------------------------

	class cPESubDivision
	{
	public:
		cVector3f mvUV[4];
	};
	
	//-------------------------------------------------------------------

	enum ePENoiseType
	{
		ePENoiseType_LowFreq,
		ePENoiseType_HighFreq,
		ePENoiseType_Both,
		ePENoiseType_None,
		ePENoiseType_LastEnum,
	};

	
	typedef struct 
	{
		float				fRelToBeamPos;
		float				fRelToBendPos;
		int					lLowFreqNoiseIdx;
		int					lHighFreqNoiseIdx;
		ePENoiseType	noiseType;
	} tBeamNoisePoint;


	//-------------------------------------------------------------------


	//////////////////////////////////////////////////////
	/////////////// PARTICLE ///////////////////////////// 
	//////////////////////////////////////////////////////

	//-------------------------------------------------------------------

	class cParticle
	{
	public:
		cParticle() : mfSpin(0), mfRenderPrevSpin(0), mbRenderSnap(true) {}

		cVector3f mvPos;
		cVector3f mvLastPos;
		cVector3f mvLastCollidePos;
		cVector3f mvAcc;
		cVector3f mvVel;

		float mfSpeedMul;
		float mfMaxSpeed;

		cColor mStartColor;
		cColor mColor;

		cVector2f mvStartSize;
		cVector2f mvSize;

		float mfStartLife;
		float mfLife;
		float mfLifeSize_MiddleStart;
		float mfLifeSize_MiddleEnd;

		float mfLifeColor_MiddleStart;
		float mfLifeColor_MiddleEnd;

		int mlSubDivNum;

		float mfBounceAmount;
		int mlBounceCount;

		cVector3f mvExtra;

		// NEW

		float mfSpin;
		float mfSpinVel;
		float mfSpinFactor;


		cVector3f mvRevolutionVel;

		// Beam Specific

		int mlLowFreqPoints;
		int mlHighFreqPoints;
		std::vector<cVector3f> mvBeamPoints;

		// Render interpolation: state before the last update
		cVector3f mvRenderPrevPos;
		cVector2f mvRenderPrevSize;
		cColor mRenderPrevColor;
		float mfRenderPrevSpin;
		bool mbRenderSnap;

	};

	//-------------------------------------------------------------------

	typedef std::vector<cParticle*> tParticleVec;
	typedef tParticleVec::iterator tParticleVecIt;

	//-------------------------------------------------------------------

	//////////////////////////////////////////////////////
	/////////////// PARTICLE SYSTEM ////////////////////// 
	//////////////////////////////////////////////////////

	class iParticleEmitter :public iRenderable
	{
	public:
		iParticleEmitter(	tString asName,tMaterialVec* avMaterials,unsigned int alMaxParticles, 
							cVector3f avSize, cGraphics* apGraphics,cResources *apResources);
		virtual ~iParticleEmitter();

		void UpdateLogic(float afTimeStep);

		void Render(){}

		void SetSubDivUV(const cVector2l &avSubDiv);

		void SetWorld(cWorld *apWorld) { mpWorld = apWorld;}

		void SetSystem(cParticleSystem *apSystem){ mpParentSystem = apSystem;}

		virtual bool IsDead(){ return mlNumOfParticles==0 && mbDying;}
		virtual bool IsDying(){ return mbDying;}
		virtual void Kill(){ mbDying = true;}
		void KillInstantly();

		void SetDataName(const tString &asName){msDataName = asName;}
		void SetDataSize(const cVector3f &avSize){mvDataSize = avSize;}

		int GetParticleNum(){ return mlNumOfParticles;}

		//Entity implementation
		tString GetEntityType(){ return "ParticleEmitter"; }
		bool IsVisible();
		
		//Renderable implementation
		// No UpdateGraphicsForViewport override — particles have no persistent
		// VB; renderers call BuildScratchGeometry into the shared per-frame
		// scratch buffers (one copy per viewport, billboarded to its camera).

		// Placement of one frame/viewport's particle geometry inside the shared
		// scratch buffers (Interface<cGraphics>::Get()->translucentVtx/IdxBuffer). All offsets are bytes.
		struct ParticleScratchGeometry {
			bool     valid = false;        // false ⇒ skip the draw (0 particles / faded / OOM)
			uint32_t vertexCount = 0;      // numParticles * 4
			uint32_t indexCount  = 0;      // numParticles * 6
			size_t   posByteOffset = 0;    // into Interface<cGraphics>::Get()->translucentVtxBuffer
			size_t   colByteOffset = 0;
			size_t   uvByteOffset  = 0;    // valid only when abWithUv
			size_t   idxByteOffset = 0;    // into Interface<cGraphics>::Get()->translucentIdxBuffer
		};

		/**
		 * Build this frame/viewport's camera-facing quads + quad indices into
		 * the shared per-frame scratch buffers (Interface<cGraphics>::Get()->translucentVtx/IdxBuffer) —
		 * the single producer for all renderers (wireframe/simple panes bind
		 * the returned offsets; the hybrid resolves them to BDAs). abWithUv
		 * builds the uv stream too (8 vs 11 floats/vertex). Returns
		 * {valid=false} when there's nothing to draw (no particles, distance-
		 * faded, or scratch exhausted).
		 */
		ParticleScratchGeometry BuildScratchGeometry(cFrustum *apFrustum,
													 float afFrameTime, bool abWithUv);

		cMaterial *GetMaterial();
		cVertexBuffer* GetVertexBuffer();

		cBoundingVolume* GetBoundingVolume();

		cMatrixf* GetModelMatrix(cFrustum *apFrustum);

		int GetMatrixUpdateCount(){return GetTransformUpdateCount();}
		eRenderableType GetRenderType(){ return eRenderableType_ParticleEmitter;}

	protected:
		// Inner builder for BuildScratchGeometry: writes the camera-facing quads
		// for apFrustum into caller memory (apPosArray: alPosStrideFloats floats/
		// vertex; apColArray: 4 floats/vertex; apUvArray: 3 floats/vertex, NULL
		// to skip — when given it is always filled). Returns false when
		// distance-faded out.
		bool BuildViewportVertices(cFrustum *apFrustum, float afFrameTime,
								   float *apPosArray, int alPosStrideFloats,
								   float *apColArray, float *apUvArray);

		void SwapRemove(unsigned int alIndex);
		cParticle* CreateParticle();

		void SaveRenderInterpState();
		void FixSpawnedRenderInterpState();
		int mlRenderInterpTick;

		virtual void UpdateMotion(float afTimeStep)=0;
		virtual void SetParticleDefaults(cParticle *apParticle)=0;
		
		cGraphics *mpGraphics;
		cResources *mpResources;

		tString msDataName;
		cVector3f mvDataSize;

		tParticleVec mvParticles;
		unsigned int mlNumOfParticles;
		unsigned int mlMaxParticles;

		cMatrixf m_mtxTemp;

		tMaterialVec* mvMaterials;

		int mlSleepCount;

		//Vars for easier updating.
		bool mbDying;
		float mfFrame;

		bool mbUpdateGfx;
		bool mbUpdateBV;

		std::vector<cPESubDivision> mvSubDivUV;

		cVector3f mvDirection;
		int mlDirectionUpdateCount;

		cVector3f mvRight;
		cVector3f mvForward;
		int mlAxisDrawUpdateCount;

		cParticleSystem *mpParentSystem;

		//Set by the particle system implementation.
		cVector2f mvDrawSize;
		cVector2f mvMaxDrawSize;

		eParticleEmitterType mDrawType;

		eParticleEmitterCoordSystem mCoordSystem;

		bool mbMultiplyRGBWithAlpha;

		cWorld *mpWorld;

		bool mbUsesDirection;

		bool mbUsePartSpin;
		bool mbUseRevolution;

		ePEType mPEType;
	};

	//-----------------------------------------------------------------

	typedef std::list<iParticleEmitter*> tParticleEmitterList;
	typedef tParticleEmitterList::iterator tParticleEmitterListIt;

	//-----------------------------------------------------------------

	//////////////////////////////////////////////////////
	/////////////// PARTICLE EMITTER DATA //////////////// 
	//////////////////////////////////////////////////////

	//-----------------------------------------------------------------

	class cResources;

	class iParticleEmitterData
	{
		friend class cParticleSystemData;
	public:
		/**
		* This inits the data needed for the particles system type
		* \param &asName name of the type
		* \param apResources 
		* \param apGraphics 
		*/
		iParticleEmitterData(const tString &asName,cResources* apResources,cGraphics *apGraphics);
		virtual ~iParticleEmitterData();

		void AddMaterial(cMaterial *apMaterial);

		const tString& GetName(){ return msName;}

		virtual iParticleEmitter* Create(tString asName, cVector3f avSize)=0;

		float GetWarmUpTime() const { return mfWarmUpTime;}
		float GetWarmUpStepsPerSec() const { return mfWarmUpStepsPerSec;}

	protected:
		cResources *mpResources;
		cGraphics *mpGraphics;

		tString msName;
		tMaterialVec mvMaterials;

		float mfWarmUpTime;
		float mfWarmUpStepsPerSec;
	};

	//-----------------------------------------------------------------

	typedef std::map<tString,iParticleEmitterData*> tParticleEmitterDataMap;
	typedef tParticleEmitterDataMap::iterator tParticleEmitterDataMapIt;

	//-----------------------------------------------------------------
};

#endif // HPL_PARTICLE_EMITTER_H

