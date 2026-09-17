#include "graphics/RendererBackendSwitch.h"
#include "utest.h"

// The renderer backend can change while the game is running: the option is
// requested from inside a frame and applied at the next frame boundary, where
// the outgoing renderer is destroyed and the incoming one built. Everything
// that decides WHETHER that happens lives in EvaluateRendererBackendSwitch, so
// the edge cases are testable without a device.

using namespace hpl;

namespace {
cRendererBackendSwitchRequest Request(eRendererBackend requested, eRendererBackend current) {
  cRendererBackendSwitchRequest out;
  out.mbPending = true;
  out.mRequested = requested;
  out.mCurrent = current;
  out.mbDeviceCanRayTrace = true;
  out.mbHaveRenderers = true;
  return out;
}
} // namespace

UTEST(RendererBackendSwitch, NoRequestDoesNothing) {
  cRendererBackendSwitchRequest request;
  request.mbPending = false;
  ASSERT_EQ(eRendererBackendSwitch_Ignore, EvaluateRendererBackendSwitch(request));
}

UTEST(RendererBackendSwitch, SwitchingToTheRunningBackendCostsNothing) {
  // A -> B -> A inside one frame collapses to this: applying it would stall and
  // throw away temporal history for no change at all.
  ASSERT_EQ(eRendererBackendSwitch_Ignore,
            EvaluateRendererBackendSwitch(Request(eRendererBackend_Standard,
                                                  eRendererBackend_Standard)));
  ASSERT_EQ(eRendererBackendSwitch_Ignore,
            EvaluateRendererBackendSwitch(Request(eRendererBackend_RayTraced,
                                                  eRendererBackend_RayTraced)));
}

UTEST(RendererBackendSwitch, AppliesBothWaysOnACapableDevice) {
  ASSERT_EQ(eRendererBackendSwitch_Apply,
            EvaluateRendererBackendSwitch(Request(eRendererBackend_RayTraced,
                                                  eRendererBackend_Standard)));
  ASSERT_EQ(eRendererBackendSwitch_Apply,
            EvaluateRendererBackendSwitch(Request(eRendererBackend_Standard,
                                                  eRendererBackend_RayTraced)));
}

UTEST(RendererBackendSwitch, RayTracedIsRefusedWhenTheDeviceCannotHostIt) {
  // The ray-traced renderer builds ray-tracing pipelines with no capability
  // check, so this must be refused rather than attempted.
  cRendererBackendSwitchRequest request =
      Request(eRendererBackend_RayTraced, eRendererBackend_Standard);
  request.mbDeviceCanRayTrace = false;
  ASSERT_EQ(eRendererBackendSwitch_Refuse, EvaluateRendererBackendSwitch(request));
}

UTEST(RendererBackendSwitch, StandardIsAlwaysHostable) {
  // Falling BACK is always possible, even on a device that cannot ray trace --
  // which is the state a failed ray-traced start leaves behind.
  cRendererBackendSwitchRequest request =
      Request(eRendererBackend_Standard, eRendererBackend_RayTraced);
  request.mbDeviceCanRayTrace = false;
  ASSERT_EQ(eRendererBackendSwitch_Apply, EvaluateRendererBackendSwitch(request));
}

UTEST(RendererBackendSwitch, HeadlessRefuses) {
  // No renderer was ever built (no screen), so there is nothing to swap.
  cRendererBackendSwitchRequest request =
      Request(eRendererBackend_RayTraced, eRendererBackend_Standard);
  request.mbHaveRenderers = false;
  ASSERT_EQ(eRendererBackendSwitch_Refuse, EvaluateRendererBackendSwitch(request));
}

UTEST(RendererBackendSwitch, OutOfRangeRequestIsIgnored) {
  cRendererBackendSwitchRequest request =
      Request(eRendererBackend_Standard, eRendererBackend_Standard);
  request.mRequested = (eRendererBackend)(eRendererBackend_LastEnum + 3);
  ASSERT_EQ(eRendererBackendSwitch_Ignore, EvaluateRendererBackendSwitch(request));
}
