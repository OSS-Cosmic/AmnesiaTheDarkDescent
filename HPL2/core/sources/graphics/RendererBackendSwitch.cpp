#include "graphics/RendererBackendSwitch.h"

namespace hpl {

    eRendererBackendSwitch EvaluateRendererBackendSwitch(const cRendererBackendSwitchRequest &aRequest)
    {
        if(aRequest.mbPending == false) return eRendererBackendSwitch_Ignore;

        if(aRequest.mRequested < 0 || aRequest.mRequested >= eRendererBackend_LastEnum)
            return eRendererBackendSwitch_Ignore;

        // A->B->A within one frame lands here: no stall, and no temporal
        // history is thrown away for nothing.
        if(aRequest.mRequested == aRequest.mCurrent) return eRendererBackendSwitch_Ignore;

        if(aRequest.mbHaveRenderers == false) return eRendererBackendSwitch_Refuse;

        if(aRequest.mRequested == eRendererBackend_RayTraced && aRequest.mbDeviceCanRayTrace == false)
            return eRendererBackendSwitch_Refuse;

        return eRendererBackendSwitch_Apply;
    }
}
