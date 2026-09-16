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

#ifndef HPL_RENDERABLE_SET_H
#define HPL_RENDERABLE_SET_H

#include "math/Math.h"

#include <span>
#include <vector>

namespace hpl {

	class iRenderable;

	//-------------------------------------------

	/**
	 * Flat replacement for the renderable box trees: two parallel arrays, one
	 * with SIMD-friendly world AABBs and one with the objects themselves.
	 * Queries are linear scans (ml::cFrustum::CheckAabb per element).
	 */
	class cRenderableSet
	{
	public:
		struct cAabb {
			ml::float3 mvMin;	// ml::float3 is a 16 byte __m128 union -> 32B per entry, 16-aligned
			ml::float3 mvMax;
		};

		void Add(iRenderable *apObject);
		void Remove(iRenderable *apObject);

		void UpdateBeforeRendering();

		/**
		 * Freezes AABB refresh; after this, object orientation may not change
		 * (same contract as the old static box tree). Worlds that never call
		 * Compile (the editors) keep refreshing every frame instead.
		 */
		void Compile();
		bool IsCompiled() const { return mbCompiled; }

		bool GetBounds(cVector3f &avMin, cVector3f &avMax) const;

		inline int Size() const { return (int)mvObjects.size(); }
		inline std::span<iRenderable* const> GetObjects() const { return mvObjects; }
		inline std::span<const cAabb> GetAabbs() const { return mvAabbs; }

		/**
		 * Linear scan for objects whose world AABB overlaps a box.
		 *
		 * Used by the shadow cull to gather, once per frame, every caster that
		 * could reach any shadow-casting light -- replacing the per-tile
		 * QueryFrustum walks the renderer used to run (two per tile, so ~100
		 * full scans a frame for a dozen lights).
		 */
		template<typename F>
		void QueryAabb(const cVector3f &avMin, const cVector3f &avMax, F &&handler) const {
			for(size_t i=0, n=mvAabbs.size(); i<n; ++i)
			{
				const cAabb &aabb = mvAabbs[i];
				if(aabb.mvMin.x > avMax.x || aabb.mvMax.x < avMin.x) continue;
				if(aabb.mvMin.y > avMax.y || aabb.mvMax.y < avMin.y) continue;
				if(aabb.mvMin.z > avMax.z || aabb.mvMax.z < avMin.z) continue;
				handler(mvObjects[i]);
			}
		}

		template<typename F>
		void QueryFrustum(const ml::cFrustum &aFrustum, uint32_t alPlanes, F &&handler) const {
			for(size_t i=0, n=mvAabbs.size(); i<n; ++i)
			{
				if(aFrustum.CheckAabb(mvAabbs[i].mvMin, mvAabbs[i].mvMax, alPlanes))
				{
					handler(mvObjects[i]);
				}
			}
		}

	private:
		void RefreshAabbs();

		std::vector<cAabb> mvAabbs;
		std::vector<iRenderable*> mvObjects;
		bool mbCompiled = false;
	};

	//-------------------------------------------
};
#endif // HPL_RENDERABLE_SET_H
