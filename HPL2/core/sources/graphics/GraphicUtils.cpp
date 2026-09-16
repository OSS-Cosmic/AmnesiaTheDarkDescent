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

#include "graphics/GraphicUtils.h"

#include <cmath>

#include "graphics/Renderable.h"
#include "math/BoundingVolume.h"
#include "math/Frustum.h"
#include "math/Math.h"
#include "scene/RenderableSet.h"

namespace hpl {
	namespace rendering {

		bool IsObjectIsVisible(iRenderable* apObject, tRenderableFlag neededFlags, std::span<cPlanef> clipPlanes) {
			if (apObject->IsVisible() == false)
				return false;

			if ((apObject->GetRenderFlags() & neededFlags) != neededFlags)
				return false;

			if (!clipPlanes.empty()) {
				cBoundingVolume* pBV = apObject->GetBoundingVolume();
				for (auto& plane : clipPlanes) {
					if (cMath::CheckPlaneBVCollision(plane, *pBV) == eCollision_Outside) {
						return false;
					}
				}
			}
			return true;
		}

		cRect2l GetClipRectFromObject(iRenderable* apObject, float afPaddingPercent, cFrustum* apFrustum,
			const cVector2l& avScreenSize, float afHalfFovTan) {
			cBoundingVolume* pBV = apObject->GetBoundingVolume();

			cRect2l clipRect;
			if (afHalfFovTan == 0)
				afHalfFovTan = std::tan(apFrustum->GetFOV() * 0.5f);
			cMath::GetClipRectFromBV(clipRect, *pBV, apFrustum, avScreenSize, afHalfFovTan);

			// Add 20% padding on clip rect
			int lXInc = (int)((float)clipRect.w * afHalfFovTan);
			int lYInc = (int)((float)clipRect.h * afHalfFovTan);

			clipRect.x = cMath::Max(clipRect.x - lXInc, 0);
			clipRect.y = cMath::Max(clipRect.y - lYInc, 0);
			clipRect.w = cMath::Min(clipRect.w + lXInc * 2, avScreenSize.x - clipRect.x);
			clipRect.h = cMath::Min(clipRect.h + lYInc * 2, avScreenSize.y - clipRect.y);

			return clipRect;
		}

		void WalkAndPrepareRenderList(cRenderableSet* apSet, cFrustum* apFrustum,
			std::function<void(iRenderable*)> handler, tRenderableFlag renderableFlag,
			bool abIgnoreFrustumCull, std::span<cPlanef> clipPlanes) {

			////////////////////////////////////////////////
			// Whole-scene path: a null frustum (the viewport-less per-world TLAS
			// build) or an explicit ignore emits every object in the set.
			if (apFrustum == nullptr || abIgnoreFrustumCull) {
				for (iRenderable* pObject : apSet->GetObjects()) {
					if (IsObjectIsVisible(pObject, renderableFlag, clipPlanes)) {
						handler(pObject);
					}
				}
				return;
			}

			////////////////////////////////////////////////
			// Build the SIMD frustum once per call. STYLE_D3D: hpl::cFrustum
			// derives its near plane from viewProj row2 alone (Vulkan z in [0,1]),
			// which is exactly ml's D3D convention. Inf-far cameras are set up
			// with a FINITE projection matrix plus a flag that skips the far-plane
			// test; FAR is ml's last plane, so skip it by passing 5 planes.
			ml::cFrustum mlFrustum;
			mlFrustum.Setup(ml::STYLE_D3D, apFrustum->GetViewProjectionMat());
			const uint32_t lPlanes = apFrustum->GetInfFarPlane() ? ml::PLANES_NO_FAR
			                                                     : ml::PLANES_NUM;

			apSet->QueryFrustum(mlFrustum, lPlanes, [&](iRenderable* pObject) {
				if (IsObjectIsVisible(pObject, renderableFlag, clipPlanes)) {
					handler(pObject);
				}
			});
		}

	} // namespace rendering
} // namespace hpl
