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

#ifndef HPL_GRAPHIUTILS_H
#define HPL_GRAPHIUTILS_H

#include <functional>
#include <span>

#include "graphics/GraphicsTypes.h"
#include "math/MathTypes.h"

namespace hpl {

	class iRenderable;
	class cRenderableSet;
	class cFrustum;

	namespace rendering {

		bool IsObjectIsVisible(iRenderable* apObject, tRenderableFlag neededFlags,
			std::span<cPlanef> clipPlanes = {});

		cRect2l GetClipRectFromObject(iRenderable* apObject, float afPaddingPercent,
			cFrustum* apFrustum, const cVector2l& avScreenSize, float afHalfFovTan);

		// Linear SIMD scan over the set's AABBs (ml::cFrustum::CheckAabb per
		// element). A null frustum or abIgnoreFrustumCull emits every object —
		// the hybrid renderer uses that so the TLAS sees whole-map geometry.
		//
		// clipPlanes is forwarded to IsObjectIsVisible, so a caller with extra
		// half-spaces (the planar water reflection's max-distance and screen-rect
		// bounds) gets them applied on the same pass as the visibility and flag
		// tests, exactly as the legacy iRenderer::CheckObjectIsVisible did.
		void WalkAndPrepareRenderList(cRenderableSet* apSet, cFrustum* apFrustum,
			std::function<void(iRenderable*)> handler, tRenderableFlag renderableFlag,
			bool abIgnoreFrustumCull = false, std::span<cPlanef> clipPlanes = {});

	} // namespace rendering

} // namespace hpl

#endif // HPL_GRAPHIUTILS_H
