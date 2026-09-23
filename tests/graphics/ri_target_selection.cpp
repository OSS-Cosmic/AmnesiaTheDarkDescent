#include "graphics/RIDevice.h"
#include "utest.h"

// RIIsTargetSelected answers "is this backend the active one?". In a build with
// more than one backend compiled in it reads the renderer's live choice; in a
// single-backend build it is a compile-time constant.
//
// The constant is the part worth pinning. It used to be `assert(match); return
// true;`, and NDEBUG deletes an assert -- so a release Vulkan-only build said
// yes to every backend, including ones it cannot possibly be running.
// RIProgram::loadShaderArtifact asks exactly this question to choose between
// ".spv" and ".dxil", so the Linux build went looking for gui.vert.dxil and
// died at startup with "Couldn't find shader artifact".
//
// A source-text contract test cannot catch that: the naming line in RIProgram
// was correct the whole time. Only evaluating the predicate does.

#if !DEVICE_MULTI_BACKEND

UTEST(RITargetSelection, SingleBackendClaimsOnlyItsOwnApi) {
  ASSERT_TRUE(RIIsTargetSelected(RI_ACTIVE_BACKEND_API));

  ASSERT_FALSE(RIIsTargetSelected(RI_DEVICE_API_UNKNOWN));
  ASSERT_FALSE(RIIsTargetSelected(RI_DEVICE_API_D3D11));
  ASSERT_FALSE(RIIsTargetSelected(RI_DEVICE_API_MTL));
}

// The specific question that broke startup. On this build Vulkan is the only
// backend, so the D3D12 answer has to be no -- that is what keeps the shader
// artifact extension at ".spv".
UTEST(RITargetSelection, VulkanOnlyBuildDoesNotClaimD3D12) {
#if DEVICE_IMPL_VULKAN && !DEVICE_IMPL_D3D12
  ASSERT_TRUE(RIIsTargetSelected(RI_DEVICE_API_VK));
  ASSERT_FALSE(RIIsTargetSelected(RI_DEVICE_API_D3D12));
#else
  UTEST_SKIP("D3D12 is compiled in; the answer depends on a live renderer");
#endif
}

#else

// Both backends compiled: the predicate reads RIActiveBackendApi(), which the
// engine defines in RIRenderer.cpp. This project links no engine sources (that
// would drag in the whole renderer), so the TU supplies the definition itself
// and drives it: whichever backend is active, exactly that one is selected.
static uint8_t g_fakeActiveApi = RI_DEVICE_API_UNKNOWN;
uint8_t RIActiveBackendApi() { return g_fakeActiveApi; }

UTEST(RITargetSelection, MultiBackendSelectsOnlyTheActiveApi) {
  const uint8_t apis[] = {RI_DEVICE_API_VK, RI_DEVICE_API_D3D12};
  for (uint8_t active : apis) {
    g_fakeActiveApi = active;
    for (uint8_t target : apis) {
      ASSERT_EQ(RIIsTargetSelected(target), target == active);
    }
    ASSERT_FALSE(RIIsTargetSelected(RI_DEVICE_API_UNKNOWN));
  }
  g_fakeActiveApi = RI_DEVICE_API_UNKNOWN;
}

#endif
