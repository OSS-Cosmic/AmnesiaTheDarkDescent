#ifndef HPL_RENDERER_BACKEND_SWITCH_H
#define HPL_RENDERER_BACKEND_SWITCH_H

#include "graphics/GraphicsTypes.h"

namespace hpl {

    //------------------------------------------------------------------------
    // Whether a requested renderer backend should be applied at this frame
    // boundary. Pulled out of cGraphics so the decision -- which is all the
    // edge cases live in -- can be exercised without a device.

    enum eRendererBackendSwitch
    {
        // Build the incoming renderer and destroy the outgoing one.
        eRendererBackendSwitch_Apply,
        // Nothing to do: no request, or the request names the backend already
        // running. Costs nothing and must not stall.
        eRendererBackendSwitch_Ignore,
        // Asked for, but impossible here: no renderer was ever built
        // (headless), or the device cannot host the ray-traced renderer. The
        // caller keeps the backend it has and says so.
        eRendererBackendSwitch_Refuse,
    };

    struct cRendererBackendSwitchRequest
    {
        bool mbPending = false;
        eRendererBackend mRequested = eRendererBackend_Standard;
        eRendererBackend mCurrent = eRendererBackend_Standard;
        // The device came up ray-tracing capable. An adapter that merely could
        // is not enough: the ray-traced renderer builds its pipelines with no
        // capability check.
        bool mbDeviceCanRayTrace = false;
        // A lit renderer exists at all, i.e. this is not a headless session.
        bool mbHaveRenderers = true;
    };

    eRendererBackendSwitch EvaluateRendererBackendSwitch(const cRendererBackendSwitchRequest &aRequest);
}

#endif // HPL_RENDERER_BACKEND_SWITCH_H
