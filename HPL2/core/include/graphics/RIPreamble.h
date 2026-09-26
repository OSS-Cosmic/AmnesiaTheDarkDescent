#pragma once


// Macro anguish
#ifdef LoadBitmap
#undef LoadBitmap
#endif

#ifdef SendMessage
#undef SendMessage
#endif

#ifdef CreateEvent
#undef CreateEvent
#endif

#ifdef CreateWindow
#undef CreateWindow
#endif

#include <cstdio>
#include <stdint.h>
#include <variant>
#include <vector>
#undef DestroyAll
#undef ButtonPress

// Backend-selection macros (DEVICE_SUPPORT_VULKAN / DEVICE_IMPL_VULKAN) must be
// defined before we decide whether to pull in the Vulkan headers AND before any
// RI struct evaluates `#if DEVICE_IMPL_VULKAN` to size its `vk` union. Include
// them here so this preamble is self-sufficient: an RI header that includes only
// RIPreamble.h gets both the macros and the Vulkan types, and its layout no
// longer depends on include order (the ODR hazard that skewed cGraphics per-TU).
#include "graphics/RIDefines.h"

#ifdef DEVICE_SUPPORT_VULKAN
#include "volk.h"

// volk has already declared the entire Vulkan API, including the platform
// surface entry points: under VK_USE_PLATFORM_XLIB_KHR it forward-declares
// Display/Window/VisualID and includes <vulkan/vulkan_xlib.h> itself rather than
// <X11/Xlib.h>, "to avoid unprefixed macros which can cause conflicts" in its
// own words (volk.h:113-170).
//
// It reaches the core API through <vulkan/vulkan_core.h>, so it never defines
// VULKAN_H_ -- and vk_mem_alloc.h guards its own include on exactly that name:
//
//     #if !defined(VULKAN_H_)
//     #include <vulkan/vulkan.h>
//     #endif
//
// vulkan.h then does `#include <X11/Xlib.h>` for the Xlib surface, which defines
// Bool, Status, None, Success, Always and Complex as bare macros that mangle
// whatever is parsed after this header. rapidjson's Handler::Bool(bool) in
// RIProgram.cpp is one casualty; the #undef blocks in DebugDraw.h,
// DisplayDepthPolicy.h, Viewport.h and FsrUpscaler.cpp are older ones.
//
// Claiming the guard costs nothing -- everything vulkan.h would declare, volk
// has declared already -- and it keeps X11 out of every TU, not just this one.
#define VULKAN_H_

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include "vk_mem_alloc.h"
#endif

#ifdef DEVICE_SUPPORT_D3D12
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#endif

// vulkan.h drags in X11 headers (via the Xlib platform surface), which #define
// bare identifiers like DestroyAll / ButtonPress as macros and collide with
// engine method names (e.g. iResourceManager::DestroyAll). Neutralize them here,
// AFTER Vulkan is pulled in — the copies in the "macro anguish" prologue above run
// before Vulkan and are therefore ineffective against X11.
#ifdef LoadBitmap
#undef LoadBitmap
#endif
#ifdef SendMessage
#undef SendMessage
#endif
#ifdef CreateEvent
#undef CreateEvent
#endif
#ifdef CreateWindow
#undef CreateWindow
#endif
#ifdef DestroyAll
#undef DestroyAll
#endif
#ifdef ButtonPress
#undef ButtonPress
#endif

// hpl::Log, used by the VK result wrapper below.
#include "system/LowLevelSystem.h"

// Backend-neutral result code returned across the RI API.
enum RIResult_e {
  RI_INCOMPLETE_DEVICE = -2,
  RI_FAIL = -1,
  RI_SUCCESS = 0,
  RI_INCOMPLETE
};

// Queue-capability bits (adapter queue families / RIQueue::getFlags).
#define RI_QUEUE_GRAPHICS_BIT 0x1
#define RI_QUEUE_COMPUTE_BIT 0x2
#define RI_QUEUE_TRANSFER_BIT 0x4
#define RI_QUEUE_SPARSE_BINDING_BIT 0x8
#define RI_QUEUE_VIDEO_DECODE_BIT 0x10
#define RI_QUEUE_VIDEO_ENCODE_BIT 0x20
#define RI_QUEUE_PROTECTED_BIT 0x40
#define RI_QUEUE_OPTICAL_FLOW_BIT_NV 0x80
#define RI_QUEUE_INVALID 0x0

#ifdef DEVICE_SUPPORT_D3D12
// Logs + returns false on a failed HRESULT; true otherwise.
#define D3D12_WrapResult(res)                                                   \
  __D3D12_WrapResult(res, __FILE__, __FUNCTION__, __LINE__)

static inline bool __D3D12_WrapResult(HRESULT result, const char *sourceFilename,
                                      const char *functionName, int sourceLine) {
  if (FAILED(result)) {
    hpl::Log("RI: D3D12 HRESULT 0x%08lX, file %s:%i (%s)\n",
             (unsigned long)result, sourceFilename, sourceLine, functionName);
    return false;
  }
  return true;
}
#endif

#ifdef DEVICE_SUPPORT_VULKAN
// Splice `next` into the front of `current`'s pNext chain.
#define R_VK_ADD_STRUCT(current, next)                                         \
  {                                                                            \
    void *__pNext = (void *)((current)->pNext);                                \
    (current)->pNext = (next);                                                 \
    (next)->pNext = __pNext;                                                   \
  }
// Device loss is terminal: continuing can reuse unfinished resources or wait
// on semaphores that will never signal. Other failures are returned to callers.
#define VK_WrapResult(res)                                                     \
  __VK_WrapResult(res, __FILE__, __FUNCTION__, __LINE__)

static inline bool __VK_WrapResult(VkResult result, const char *sourceFilename,
                                   const char *functionName, int sourceLine) {
  if (result != VK_SUCCESS) {
    hpl::Log("RI: VK %i, file %s:%i (%s)\n", result, sourceFilename, sourceLine,
             functionName);
    if (result == VK_ERROR_DEVICE_LOST)
      hpl::FatalError("Vulkan device lost in %s (%s:%i). Rendering cannot "
                      "continue. See hpl.log for details.\n",
                      functionName, sourceFilename, sourceLine);
    return false;
  }
  return true;
}
#endif

