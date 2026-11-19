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

#ifndef HPL_ENTITY3D_H
#define HPL_ENTITY3D_H

#include <list>
#include "math/MathTypes.h"
#include "system/SystemTypes.h"
#include "scene/SceneTypes.h"
#include "system/Container.h"

#include "math/BoundingVolume.h"

namespace hpl {

	//------------------------------------

	//Used for the render container to add specific data to
	//the object.
	class iRenderContainerData
	{

	};

	typedef std::list<iRenderContainerData*> tRenderContainerDataList;
	typedef tRenderContainerDataList::iterator tRenderContainerDataListIt;
	
	//-----------------------------------------

	class cNode3D;
	class cCamera;

	//-----------------------------------------

	class iEntity3D
	{
	public:
		iEntity3D(tString asName);
		virtual ~iEntity3D();

		virtual tString GetEntityType()=0;

		virtual void UpdateLogic(float afTimeStep){}

		tString& GetName(){return msName;}
		void SetName(const tString& asName){msName = asName;}

		cNode3D* GetParent(){ return mpParentNode;}
		void SetParent(cNode3D* apNode){ mpParentNode = apNode;}
		bool HasParent(){ return mpParentNode!=NULL;}

		bool IsActive(){ return mbIsActive; }
		void SetActive(bool abActive){ mbIsActive = abActive; }

		cVector3f GetLocalPosition();
		cMatrixf& GetLocalMatrix();

		cVector3f GetWorldPosition();
		cMatrixf& GetWorldMatrix();

		void SetPosition(const cVector3f& avPos);
		void SetMatrix(const cMatrixf& a_mtxTransform);

		void SetWorldPosition(const cVector3f& avWorldPos);
		void SetWorldMatrix(const cMatrixf& a_mtxWorldTransform);

		void SetTransformUpdated(bool abUpdateCallbacks = true);
		bool GetTransformUpdated();

		int GetTransformUpdateCount();

		void AddCallback(iEntityCallback *apCallback);
		void RemoveCallback(iEntityCallback *apCallback);

		void SetSourceFile(const tString& asFile){ msSourceFile = asFile;}
		const tString& GetSourceFile(){ return msSourceFile;}

		virtual cBoundingVolume* GetBoundingVolume();

		bool IsSaved(){ return mbIsSaved; }
		void SetIsSaved(bool abX){ mbIsSaved = abX; }

		void SetUniqueID(int alX){ mlUniqueID = alX;}
		int GetUniqueID(){ return mlUniqueID;}

        /**
		 * The entity3d child hierarchy will only work if the child has no node parent.
		 **/
		void AddChild(iEntity3D *apEntity);
		void RemoveChild(iEntity3D *apEntity);
		bool IsChild(iEntity3D *apEntity);
		iEntity3D *GetEntityParent();
		cEntity3DIterator GetChildIterator();
		
		/**
		* The node3d child hierarchy will only work if the node child has no node parent.
		**/
		void AddNodeChild(cNode3D *apNode);
		void RemoveNodeChild(cNode3D *apNode);
		bool IsNodeChild(cNode3D *apNode);

		inline int GetIteratorCount(){ return mlIteratorCount;}
		inline void SetIteratorCount(const int alX){ mlIteratorCount = alX;}

		/**
		 * Makes the next render interpolation snapshot of this entity (and its
		 * subclass state) start fresh, so a teleport is not smeared over a frame.
		 */
		void SnapRenderInterpolation();

		/**
		 * Return false for entities that are never drawn (physics bodies, sounds...),
		 * so render interpolation does not spend time on them.
		 */
		virtual bool IsRenderInterpolated(){ return true; }

		/**
		 * View model mode: this entity (and everything attached to it) is interpolated relative
		 * to the camera and drawn relative to the camera as it is rendered (incl. per frame mouse
		 * look). Use for things glued to the view, like the player's hands. NULL turns it off.
		 */
		void SetRenderInterpCamera(cCamera *apCamera);
		cCamera *GetRenderInterpCamera(){ return mpInterpCamera; }
		/** The view model camera of this entity or the first ancestor that has one. */
		cCamera *FindRenderInterpCamera();

	protected:
		virtual void OnTransformUpdated(){}

		////////////////////////////////////
		// Render interpolation hooks (see scene/RenderInterpolation.h)

		/**
		 * Called after a tick for every touched entity. Store the state that should be
		 * interpolated. abSnap means the history is invalid (teleport / first time).
		 * Return true if there is non-transform state that differs between the two
		 * last ticks and should be interpolated.
		 */
		virtual bool OnRenderInterpSnapshot(bool abSnap){ return false; }
		/** Write the interpolated (non-transform) render state. Called after the transform was applied. */
		virtual void OnRenderInterpApply(float afT){}
		/** Put the exact simulation state back. Called after the transform was restored. */
		virtual void OnRenderInterpRestore(){}
		/**
		 * Make sure all cached world transforms owned by this entity are up to date
		 * (so nothing is lazily computed from an interpolated parent during rendering).
		 */
		virtual void OnRenderInterpResolve(){ UpdateWorldTransform(); }

		/**
		 * Writes a world matrix directly (used by render interpolation). Bumps the
		 * transform count and runs callbacks, but does not touch children or the
		 * local matrix.
		 */
		void SetRenderWorldMatrix(const cMatrixf& a_mtxWorld);
		
		cNode3D* mpParentNode;

		tString msName;
		bool mbIsActive;

		bool mbIsSaved;
		int mlUniqueID;

		cMatrixf m_mtxLocalTransform;
		cMatrixf m_mtxWorldTransform;

		cBoundingVolume mBoundingVolume;
		bool mbUpdateBoundingVolume;
		bool mbApplyTransformToBV;

		bool mbTransformUpdated;
		
		int mlCount;

		tString msSourceFile;

		tEntityCallbackList mlstCallbacks;

		tEntity3DList mlstChildren;
		iEntity3D *mpParent;

		tNode3DList mlstNodeChildren;
		
		int mlIteratorCount;

		void UpdateWorldTransform();

	private:
		friend class cRenderInterpolation;

		int mlInterpTouchIdx;
		int mlInterpActiveIdx;
		bool mbInterpSnap;
		cCamera *mpInterpCamera;
	};

};
#endif // HPL_ENTITY3D_H
