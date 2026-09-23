#ifndef RI_TYPES_H
#define RI_TYPES_H

// Umbrella header: defines no types itself, just aggregates the use-case domain
// headers for wholesale consumers. New code should include the specific domain
// header(s) it needs.
//
// Layering (leaf -> top): prelude -> resource -> pipeline / shared-ptr ->
// descriptor -> command -> device -> command-ring. RISwapchain.h includes the
// device/command domains directly and is included by consumers as needed (it is
// not part of this umbrella, matching the pre-refactor layout).

// Prelude: backend macros, volk/VMA, RIResult_e, queue bits, VK_WrapResult.
#include "graphics/RIPreamble.h"

// Resource leaves: formats, barriers, buffers, textures, views.
#include "graphics/RIFormat.h"
#include "graphics/RIBarrier.h"
#include "graphics/RIBuffer.h"
#include "graphics/RITexture.h"
#include "graphics/RITextureView.h"

// Pipeline / draw-state enums, the backend-neutral pipeline description built
// from them, + the intrusive shared handle.
#include "graphics/RIPipeline.h"
#include "graphics/RIPipelineDesc.h"
#include "graphics/RISharedPointer.h"

// Domain types.
#include "graphics/RIDescriptor.h"        // descriptors, samplers, accel structures
#include "graphics/RICommand.h"           // queues, pools, command buffers, rect/viewport
#include "graphics/RIDevice.h"            // renderer, adapter, device, backend-select
#include "graphics/RICommandRingBuffer.h" // command ring (needs RIDevice complete)

// Retained for backward-compat: consumers historically got these transitively
// through RITypes.h.
#include "system/Hasher.h"
#include "system/LowLevelSystem.h"
#include <atomic>
#include <cassert>
#include <cstring>
#include <optional>

#endif // RI_TYPES_H
