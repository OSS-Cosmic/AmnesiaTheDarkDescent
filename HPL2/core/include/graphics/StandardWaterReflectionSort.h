#pragma once

namespace hpl {
class cFrustum;
class cBoundingVolume;

// View-space depth key for a translucent drawn inside the planar water
// reflection capture.
//
// This is cRenderList2::AddObject's translucent branch
// (RenderList.cpp:348-365) lifted out so the reflection can sort without
// touching iRenderable::mfViewSpaceZ. The capture runs mid-iteration over the
// main list's translucent span, so writing the shared sort key would leave
// every object carrying a reflected depth for the rest of the frame.
//
// View space is negative forward, so ascending order is back-to-front, the
// same ordering SortFunc_Translucent produces.
float StandardReflectionTranslucentViewZ(cFrustum *frustum,
                                         cBoundingVolume &bounds);
} // namespace hpl
