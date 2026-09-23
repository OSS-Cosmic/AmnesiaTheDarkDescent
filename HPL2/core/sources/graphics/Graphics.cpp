/*
 * Copyright © 2009-2020 Frictional Games
 *
 * This file is part of Amnesia: The Dark Descent.
 *
 * Amnesia: The Dark Descent is free software: you can redistribute it and/or
 modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * Amnesia: The Dark Descent is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Amnesia: The Dark Descent.  If not, see
 <https://www.gnu.org/licenses/>.
 */

#include "graphics/Graphics.h"
#include "graphics/RendererBackendSwitch.h"

#include "engine/EngineTypes.h"
#include "engine/Updateable.h"

#include "graphics/RIFormat.h"
#include "system/LowLevelSystem.h"
#include "system/Platform.h"
#include "system/String.h"

#include "graphics/DecalCreator.h"
#include "graphics/HybridRenderer.h"
#include "graphics/StandardRenderer.h"
#include "graphics/GpuParticles.h"
#include "graphics/LightProbeQuery.h"
#include "graphics/MaterialType.h"
#include "graphics/MeshCreator.h"
#include "graphics/NrdIntegration.h"
#include "graphics/PostEffect.h"
#include "graphics/PostEffectComposite.h"
#include "graphics/RIRenderer.h"
#include "graphics/RIResourceUploader.h"
#include "graphics/RITypes.h"
#include "graphics/RIVK.h"
#include "graphics/TextureCreator.h"
#include "graphics/XessUpscaler.h"

#include "resources/FileSearcher.h"
#include "resources/LowLevelResources.h"
#include "resources/Resources.h"

#include "graphics/MaterialType_BasicSolid.h"
#include "graphics/MaterialType_BasicTranslucent.h"
#include "graphics/MaterialType_Decal.h"
#include "graphics/MaterialType_Water.h"

#include "graphics/PostEffect_Bloom.h"
#include "graphics/PostEffect_ColorConvTex.h"
#include "graphics/PostEffect_ImageTrail.h"
#include "graphics/PostEffect_RadialBlur.h"
#include "graphics/PostEffect_ToneMap.h"

#include "graphics/DebugDraw.h"
#include "graphics/RIScratchAlloc.h"
#include "graphics/RendererSimple.h"
#include "graphics/RendererWireFrame.h"
#include "graphics/Window.h"
#include <cassert>
#include <vulkan/vulkan_core.h>

#include "graphics/RISwapchain.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <optional>

namespace hpl {

// What the HPL2 renderer needs from any adapter, stated in the backend-neutral
// terms every backend publishes while enumerating (see RIPhysicalAdapter in
// RIDevice.h). Returns the first unmet requirement as a label for the user, or
// NULL when the adapter is acceptable.
//
// RIDevice::init builds whatever device it is asked for and asserts only its
// own caller contract; deciding that an adapter is unfit to run the game is the
// engine's call, so it is made here, once, before the device exists.
static const char *RendererUnmetAdapterRequirement(const RIPhysicalAdapter &adapter) {
  if (!adapter.isSwapChainSupported)
    return "a presentable swapchain";
  if (adapter.bindlessTier < 1)
    return "bindless descriptors";
  if (!adapter.isBufferDeviceAddressSupported)
    return "buffer device addresses";
  if (!adapter.isShaderStorageScalarLayoutSupported)
    return "scalar block layout";
  // SceneTypes buffer addresses and the shared particle/triangle shaders use
  // uint64_t.
  if (!adapter.isShaderNativeI64Supported)
    return "64-bit integer shader operations";
  if (!adapter.isDynamicRenderingSupported)
    return "dynamic rendering";
  return NULL;
}

//////////////////////////////////////////////////////////////////////////
// CONSTRUCTORS
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

cGraphics::cGraphics(cWindow *apWindow, iLowLevelResources *apLowLevelResources)
    : iUpdateable("HPL_Graphics") {
  mpWindow = apWindow;
  mpLowLevelResources = apLowLevelResources;

  mpMeshCreator = NULL;
  mpTextureCreator = NULL;
  mpDecalCreator = NULL;
  mpDebugDraw = NULL;
}

cGraphics::~cGraphics() {
  Log("Exiting Graphics Module\n");
  Log("--------------------------------------------------------\n");

  // Interface<cGraphics> is still registered here (cEngine unregisters
  // everything at the very end of ~cEngine): InitGlobalManagedSets and
  // GPU-resource destructors reach this object via
  // Interface<cGraphics>::Get() throughout its lifetime, including the
  // teardown drains below.
  // Phase 1 backstop — a no-op when cEngine::~cEngine already ran it
  // before deleting cResources (the normal path).
  DestroyRenderObjects();
  // Phase 2: full RI teardown, including the deferrals ~cResources pushed.
  Dispose();

  Log("--------------------------------------------------------\n\n");
}

void cGraphics::DestroyRenderObjects() {
  // Nothing to destroy if the RI device never came up (Init not run).
  if (device.vk.device == VK_NULL_HANDLE)
    return;

  // Wait for the GPU to finish before tearing anything down. The renderer
  // and material destructors below free GPU objects (TLAS, buffers, image
  // views) directly on the assumption the device is idle. Without this the
  // TLAS is destroyed while still referenced by the last frame's command
  // buffer (VUID-...-accelerationStructure-02442).
  device.queues[RI_QUEUE_GRAPHICS].waitIdle(&device);

  tMaterialTypeMapIt it = m_mapMaterialTypes.begin();
  for (; it != m_mapMaterialTypes.end(); ++it) {
    iMaterialType *pType = it->second;
    pType->DestroyData();
  }
  STLMapDeleteAll(m_mapMaterialTypes);

  STLDeleteAll(mvPostEffectTypes);

  // mvOwnedRenderers owns them; mvRenderers only points at them.
  for (size_t i = 0; i < mvOwnedRenderers.size(); ++i) {
    if (mvOwnedRenderers[i]) {
      mvOwnedRenderers[i]->DestroyData();
      hplDelete(mvOwnedRenderers[i]);
    }
  }
  mvOwnedRenderers.clear();
  mvRenderers.clear();
  for (int i = 0; i < eRendererBackend_LastEnum; ++i)
    mLitRenderers[i] = NULL;

  // Before the managed set, and after the waitIdle above — the probe's readback
  // copies are recorded into the frame command buffers being retired here.
  if (lightProbe) {
    lightProbe->Dispose(&device);
    hplDelete(lightProbe);
    lightProbe = nullptr;
  }

  // Same reasoning as the probe above: the pool's buffers are referenced by the
  // compute passes recorded into the frame command buffers being retired here,
  // so it must go after the waitIdle and before the managed set.
  if (gpuParticles) {
    hplDelete(gpuParticles);
    gpuParticles = nullptr;
  }

  // Destroy the global managed set after the renderers (which only borrow
  // its layout). Idempotent.
  ShutdownGlobalManagedSets(&device);

  STLDeleteAll(mlstPostEffectComposites);
  STLDeleteAll(mlstPostEffects);

  if (mpMeshCreator) {
    hplDelete(mpMeshCreator);
    mpMeshCreator = NULL;
  }
  if (mpTextureCreator) {
    hplDelete(mpTextureCreator);
    mpTextureCreator = NULL;
  }
  if (mpDecalCreator) {
    hplDelete(mpDecalCreator);
    mpDecalCreator = NULL;
  }
  if (mpDebugDraw) {
    hplDelete(mpDebugDraw);
    mpDebugDraw = NULL;
  }

  // Release everything deferred so far while the resource managers (owned
  // by cResources) are still alive: SharedResourcePins call back into their
  // owning manager's FreeResource(), and the freed textures reach the
  // device via Interface<cGraphics>::Get() — both must still exist.
  // ~cResources pushes a few final deferrals after this; Dispose() drains
  // those with the device still alive.
  graphicsDefer.drainAll();
}

void cGraphics::Init(const cEngineInitVars::cGraphicsVars &aVars,
                     cResources *apResources, tFlag alHplSetupFlags) {
  // Present-mode choice for the initial swapchain; preserved across recreates.
  m_vsync = aVars.mbVsync;
  m_requestedVsync = aVars.mbVsync;

  mRendererBackend = aVars.mRendererBackend;
  if (mRendererBackend != eRendererBackend_Standard && mRendererBackend != eRendererBackend_RayTraced) {
    mRendererBackend = eRendererBackend_RayTraced;
  }
  mbRuntimeBackendSwitchAllowed = aVars.mbAllowRuntimeBackendSwitch;

  Log("Initializing Graphics Module\n");
  Log("--------------------------------------------------------\n");

  mpResources = apResources;

  ////////////////////////////////////////////////
  // Setup the graphic directories:
  apResources->AddResourceDir(_W("core/shaders"), false);
  apResources->AddResourceDir(_W("core/textures"), false);
  apResources->AddResourceDir(_W("core/models"), false);
  // Each backend's artifacts live in their own child directory. Resource
  // directories are indexed non-recursively, so register each one explicitly.
  //
  // The flat compiled_shaders/ is deliberately absent. Nothing has written it
  // since the per-backend split, but a tree built before that still has the old
  // artifacts sitting in it, and registering it ahead of these served those in
  // preference to the current ones -- stale shaders, silently, on any machine
  // that had built once before the move.
  apResources->AddResourceDir(_W("compiled_shaders/vk"), false);
#if DEVICE_IMPL_D3D12
  apResources->AddResourceDir(_W("compiled_shaders/d3d12"), false);
#endif

  ////////////////////////////////////////////////
  // LowLevel Init
  if (alHplSetupFlags & eHplSetup_Screen) {
    Log("Init window: %dx%d disp:%d fs:%d cap:'%s'\n", aVars.mvScreenSize.x,
        aVars.mvScreenSize.y, aVars.mlDisplay, aVars.mbFullscreen,
        aVars.msWindowCaption.c_str());
    mpWindow->Init(aVars.mvScreenSize, aVars.mlDisplay, aVars.mbFullscreen,
                   aVars.msWindowCaption);
    mbScreenIsSetup = true;
  } else {
    mbScreenIsSetup = false;
  }
#if !DEVICE_IMPL_VULKAN && !DEVICE_IMPL_D3D12
#error "No RI graphics backend compiled"
#endif
  {
    struct RIBackendInit backendInit = {};
    backendInit.applicationName = "HPL2";

    // D3D12 wherever it is compiled in (Windows), Vulkan everywhere else,
    // unless the command line asked for one of them specifically.
    uint8_t requestedApi;
    switch (aVars.mRenderApi) {
    case eRenderApiPreference_Vulkan:
      requestedApi = RI_DEVICE_API_VK;
      break;
    case eRenderApiPreference_D3D12:
      requestedApi = RI_DEVICE_API_D3D12;
      break;
    case eRenderApiPreference_Auto:
    default:
      requestedApi = DEVICE_IMPL_D3D12 ? RI_DEVICE_API_D3D12 : RI_DEVICE_API_VK;
      break;
    }

    // Asking for a backend this build does not contain is a hard stop rather
    // than a fallback: the renderer never silently substitutes another API, and
    // an override that is quietly ignored is how you spend an afternoon on a
    // repro that was running the other backend the whole time.
    // DEVICE_IMPL_* are 0/1, so these fold away in single-backend builds while
    // both arms stay type-checked everywhere.
    if (requestedApi == RI_DEVICE_API_D3D12 && !DEVICE_IMPL_D3D12) {
      FatalError("Direct3D 12 was not compiled into this build. Rebuild with "
                 "--with-d3d12=yes, or omit --d3d12 to use Vulkan.\n");
    }
    if (requestedApi == RI_DEVICE_API_VK && !DEVICE_IMPL_VULKAN) {
      FatalError("Vulkan was not compiled into this build. Omit --vulkan to "
                 "use this build's default backend.\n");
    }

    backendInit.api = requestedApi;
    const char *pBackendDisplayName =
        (requestedApi == RI_DEVICE_API_D3D12) ? "Direct3D 12" : "Vulkan";

#if (DEVICE_IMPL_D3D12)
    if (requestedApi == RI_DEVICE_API_D3D12) {
      // HPL_D3D12_VALIDATION=1 enables the debug layer; =2 adds GPU-based
      // validation, which slows the GPU enough to trip a TDR on full maps.
      const char *pValidationEnv = getenv("HPL_D3D12_VALIDATION");
      const int lValidationLevel = pValidationEnv ? atoi(pValidationEnv) : 0;
      backendInit.d3d12.validationLevel =
          lValidationLevel >= 2   ? RI_D3D12_VALIDATION_LEVEL_GPU_BASED
          : lValidationLevel == 1 ? RI_D3D12_VALIDATION_LEVEL_STANDARD
                                  : RI_D3D12_VALIDATION_LEVEL_NONE;

      // HPL_D3D12_DRED=0/1 overrides RI's default (on with the debug layer or
      // in debug builds).
      const char *pDredEnv = getenv("HPL_D3D12_DRED");
      backendInit.d3d12.dredMode =
          !pDredEnv              ? RI_D3D12_DRED_DEFAULT
          : atoi(pDredEnv) != 0 ? RI_D3D12_DRED_ON
                                : RI_D3D12_DRED_OFF;
    }
#endif

#if (DEVICE_IMPL_VULKAN)
    if (requestedApi == RI_DEVICE_API_VK) {
      // OFF unless the user opts in with HPL_VK_VALIDATION=1, in any build.
      // Nobody should pay for the layer without asking for it, and leaving it
      // on by default also stacks it under driver-debug modes (RADV_DEBUG=hang
      // etc.) -- both intercept submits, and the combination is the
      // least-tested path as well as one that perturbs hang repros.
      //
      // Turn it on while working on the renderer: it is what catches unbound
      // descriptor sets, malformed copies and bad barriers at the call that
      // causes them rather than as corruption several frames later.
      const char *pValidationEnv = getenv("HPL_VK_VALIDATION");
      backendInit.vk.enableValidationLayer =
          pValidationEnv != NULL && atoi(pValidationEnv) != 0;
    }

    // XeSS needs instance extensions that cannot be added after vkCreateInstance,
    // so it contributes them here. RI declines anything this instance cannot
    // provide, which vetoes XeSS rather than failing initialization -- so this
    // is pure opt-in and never affects whether the renderer comes up. Must
    // outlive InitRIRenderer below.
    struct RIVkInstanceRequirements instanceRequirements[1] = {};
    if (requestedApi == RI_DEVICE_API_VK &&
        XessVkInstanceRequirements(&instanceRequirements[0])) {
      backendInit.vk.optionalRequirements = instanceRequirements;
      backendInit.vk.optionalRequirementCount = 1;
    }
#endif

    // Two backends can ship in one binary now, so name the one that came up:
    // without this a log from a bug report does not say which API ran.
    Log("Graphics API: %s\n", pBackendDisplayName);

    if (InitRIRenderer(&backendInit) != RI_SUCCESS) {
      FatalError(
          "Failed to initialize the %s renderer! Make sure your graphics "
          "card supports %s and your drivers are up to date.\n",
          pBackendDisplayName, pBackendDisplayName);
    }

    uint32_t numAdapters = 0;
    if (EnumerateRIAdapters(NULL, &numAdapters) != RI_SUCCESS ||
        numAdapters == 0) {
      FatalError("No %s-compatible graphics adapter found! Make sure your "
                 "drivers are up to date.\n",
                 pBackendDisplayName);
    }
    std::vector<RIPhysicalAdapter> physicalAdapters(numAdapters);

    if (EnumerateRIAdapters(physicalAdapters.data(), &numAdapters) !=
        RI_SUCCESS) {
      FatalError("Failed to enumerate %s graphics adapters!\n",
                 pBackendDisplayName);
    }
    uint32_t selectedAdapterIdx = 0;
    for (size_t i = 1; i < numAdapters; i++) {
      if (physicalAdapters[i].type > physicalAdapters[selectedAdapterIdx].type)
        selectedAdapterIdx = static_cast<uint32_t>(i);
      if (physicalAdapters[i].type < physicalAdapters[selectedAdapterIdx].type)
        continue;

      if (physicalAdapters[i].presetLevel >
          physicalAdapters[selectedAdapterIdx].presetLevel)
        selectedAdapterIdx = static_cast<uint32_t>(i);
      if (physicalAdapters[i].presetLevel <
          physicalAdapters[selectedAdapterIdx].presetLevel)
        continue;

      if (physicalAdapters[i].videoMemorySize >
          physicalAdapters[selectedAdapterIdx].videoMemorySize)
        selectedAdapterIdx = static_cast<uint32_t>(i);
    }
    const RIPhysicalAdapter &selectedAdapter =
        physicalAdapters[selectedAdapterIdx];
    // The renderer's unconditional requirements, checked before the
    // backend-specific gates below and before any device is created, so a
    // shortfall is named rather than surfacing as a generic device failure.
    if (const char *pUnmetRequirement =
            RendererUnmetAdapterRequirement(selectedAdapter)) {
      FatalError("The %s renderer requires %s, which adapter '%s' does not "
                 "support. Update your graphics driver or use a different "
                 "GPU.\n",
                 pBackendDisplayName, pUnmetRequirement, selectedAdapter.name);
    }
#if (DEVICE_IMPL_D3D12)
    // The engine's DXIL is built at SM 6.8: raster vertex shaders read the
    // bindless object slot from SV_StartInstanceLocation (the draw's
    // firstInstance). Below 6.8 every draw would read object 0 and nothing
    // renders, so refuse up front with an actionable message.
    if (RIIsTargetSelected(RI_DEVICE_API_D3D12)) {
      const uint32_t smMajor = selectedAdapter.d3d12.highestShaderModelMajor;
      const uint32_t smMinor = selectedAdapter.d3d12.highestShaderModelMinor;
      if (smMajor < 6 || (smMajor == 6 && smMinor < 8)) {
        FatalError("The %s renderer requires Shader Model 6.8, but adapter "
                   "'%s' reports %u.%u. Update your graphics driver or use "
                   "the Vulkan renderer.\n",
                   pBackendDisplayName, selectedAdapter.name, smMajor,
                   smMinor);
      }
    }
#endif
    struct RIDeviceDesc deviceInit = {0};
    deviceInit.physicalAdapter = &physicalAdapters[selectedAdapterIdx];
    // Only the ray-traced backend needs hardware ray tracing; Standard's raster shaders use
    // no ray query or acceleration structure, so it runs on any Vulkan GPU.
    // Always ask for an RT-capable device when the adapter can give one, even
    // when starting on Standard: the device's capability mode is fixed for the
    // session, so this is what lets the backend change later without
    // recreating the device, the swapchain, the global bindless set and every
    // RIProgram. The ray-traced WORK stays gated on the active backend
    // (cWorld::BuildTlas), so a Standard session pays for the capability, not
    // for acceleration structures nothing reads.
    const bool bAdapterCanRayTrace = selectedAdapter.isRayTracingSupported &&
                                     selectedAdapter.isRayQuerySupported;
    // Decided from what the adapter advertises, before the device exists: the
    // RI layer no longer rejects a request it cannot service, so an
    // unserviceable one must never be made. Only the active backend that NEEDS
    // ray tracing falls back; an editor that merely wanted the option keeps the
    // backend it asked for and loses the switch.
    if (!bAdapterCanRayTrace) {
      if (mRendererBackend == eRendererBackend_RayTraced) {
        Log("Renderer backend: ray tracing unsupported on '%s', falling back to "
            "Standard\n",
            selectedAdapter.name);
        mRendererBackend = eRendererBackend_Standard;
      } else {
        Log("Renderer backend: ray tracing unavailable on '%s', backend "
            "switching disabled\n",
            selectedAdapter.name);
      }
    }
    deviceInit.requestRayTracing = bAdapterCanRayTrace ? 1 : 0;
    // The ray-traced shaders trace with inline ray query, so it is requested
    // together with the acceleration structures it reads.
    deviceInit.requestRayQuery = deviceInit.requestRayTracing;
#if (DEVICE_IMPL_VULKAN)
    // XeSS's device extensions and feature chain, which likewise have to be in
    // place before vkCreateDevice. Queried against the adapter just selected;
    // RI drops the whole contribution rather than failing if it cannot be
    // honoured. Must outlive device.init below.
    struct RIVkDeviceRequirements deviceRequirements[1] = {};
    if (RIIsTargetSelected(RI_DEVICE_API_VK) &&
        XessVkDeviceRequirements(
            &deviceRequirements[0], RIGetVkInstance(),
            physicalAdapters[selectedAdapterIdx].vk.physicalDevice)) {
      deviceInit.optionalRequirements = deviceRequirements;
      deviceInit.optionalRequirementCount = 1;
    }
#endif
    if (device.init(&deviceInit) != RI_SUCCESS) {
      FatalError("Failed to create %s device on adapter '%s'! Make sure "
                 "your drivers are up to date.\n",
                 pBackendDisplayName, selectedAdapter.name);
    }
    // What the DEVICE came up with, not what the adapter advertised: an adapter
    // that claims ray tracing but whose device comes up without it must not be
    // offered a switch it cannot perform.
    mbRayTracedSupported = device.accelerationStructureEnabled &&
                           device.rayTracingPipelineEnabled && device.rayQueryEnabled;
    RI_InitResourceUploader(&device, &uploader);

    // Swapchain + per-image views. Same RISwapchain::create path as the rebuild
    // in BeginActiveSet; m_vsync carries the startup present mode (threaded
    // from config). Per-viewport render targets (backbuffer, overscan,
    // depth, visibility) are created lazily by each renderer's Draw on its
    // cViewport (see scene/Viewport.h) — only the swapchain views are global.
    // Headless runs (no eHplSetup_Screen: CLI tools like texconverter) have
    // no window to create a surface from — the device and everything below
    // still come up so offscreen GPU work keeps functioning.
    if (mbScreenIsSetup) {
      struct RIWindowHandle windowHandle = mpWindow->GetHandle();
      if (windowHandle.type == RI_WINDOW_UNKNOWN) {
        FatalError("Failed to find valid window handle!\n");
      }

      // First create: hand create() the window handle so it makes the surface
      // (RICreateWindowSurface) and the new swapchain adopts ownership. The
      // surface then rides inside the swapchain across every recreate and is
      // freed by the last live swapchain's dispose() — cGraphics never owns it.
      RISwapchainDesc desc = {};
      desc.requestImageCount = RI_NUMBER_FRAMES_FLIGHT;
      desc.queue = &device.queues[RI_QUEUE_GRAPHICS];
      desc.width = (uint16_t)aVars.mvScreenSize.x;
      desc.height = (uint16_t)aVars.mvScreenSize.y;
      desc.format = RI_SWAPCHAIN_BT709_G22_8BIT;
      desc.vsync = m_vsync;
      desc.source = windowHandle;

      swapchain = RISharedPointer<RISwapchain>(
          &device, RISwapchain::create(&device, desc));
      if (swapchain.isEmpty()) {
        FatalError("Failed to create swapchain (%ux%u)!\n",
                   (uint32_t)aVars.mvScreenSize.x,
                   (uint32_t)aVars.mvScreenSize.y);
      }
    }

    struct RIQueue *graphicsQueue = &device.queues[RI_QUEUE_GRAPHICS];
    graphicsCmdRing.init(&device, graphicsQueue, RI_NUMBER_FRAMES_FLIGHT,
                         RI_NUMBER_SUB_COMMANDS, true);
    graphicsTimeline.init(&device);
    profiler.init(&device);
    for (auto &set : frameSets) {
      struct RIScratchAllocDesc uboDesc = {
          .blockSize = 256 * 128,
          .alignmentReq = device.physicalAdapter.constantBufferOffsetAlignment,
          .alloc = RIUniformScratchAllocHandler};
      InitRIScratchAlloc(&device, &set.uboScratchAlloc, &uboDesc);

      if (device.accelerationStructureEnabled) {
        // AS build scratch pool. 1 MiB blocks fit typical TLAS/BLAS scratch
        // for moderate scenes; oversized builds spill through the allocator's
        // one-shot path.
        struct RIScratchAllocDesc accelDesc = {
            .blockSize = 1024 * 1024,
            .alignmentReq = device.physicalAdapter
                                .accelerationStructureScratchOffsetAlignment,
            .alloc = RIAccelScratchAllocHandler};
        InitRIScratchAlloc(&device, &set.accelScratchAlloc, &accelDesc);
      }
    }
  }
  {
    struct RICommandRingElement initElem = graphicsCmdRing.acquire(&device, 1);
    initElem.pool->reset(&device);
    initElem.cmds[0].begin(&device);

    RIBuffer whiteUploadStaging = {};

    // 1x1 white texture — staged upload, then transitioned for shader reads.
    {
      RITextureDesc whiteDesc = {};
      whiteDesc.type = RI_TEXTURE_2D;
      whiteDesc.format = RI_FORMAT_RGBA8_UNORM;
      whiteDesc.width = 1;
      whiteDesc.height = 1;
      whiteDesc.usage = RI_USAGE_SHADER_RESOURCE | RI_USAGE_TRANSFER_DST;
      whiteTexture2D = RITexture::create(&device, whiteDesc);
      if (whiteTexture2D.isEmpty()) {
        FatalError("Failed to create white texture image!\n");
      }
      whiteTexture2D.setDebugObjectName(&device, "Graphics.whiteTexture2D");

      const uint8_t whitePixel[4] = {255, 255, 255, 255};
      const RIDeviceSize whiteRowPitch = RIFormatAlignRowPitch(
          sizeof(whitePixel),
          device.physicalAdapter.uploadBufferTextureRowAlignment, 4);
      whiteUploadStaging = RIBuffer::create(
          &device, {whiteRowPitch, RI_BUFFER_USAGE_TRANSFER_SRC,
                    RI_MEMORY_HOST_UPLOAD, 0});
      if (whiteUploadStaging.isEmpty()) {
        FatalError("Failed to create white texture staging buffer!\n");
      }
      if (whiteUploadStaging.mappedAddress == nullptr) {
        FatalError("Failed to map white texture staging buffer!\n");
      }
      memcpy(whiteUploadStaging.mappedAddress, whitePixel, sizeof(whitePixel));
      whiteUploadStaging.flushMappedRange(&device, 0, 0);

      RITextureBarrier toTransfer = {};
      toTransfer.texture = &whiteTexture2D;
      toTransfer.before = RI_RESOURCE_STATE_UNDEFINED;
      toTransfer.after = RI_RESOURCE_STATE_COPY_DST;
      toTransfer.afterStages = RI_STAGE_COPY;
      toTransfer.mipCount = 1;
      toTransfer.layerCount = 1;
      initElem.cmds[0].vk_d3d12_textureBarrier(toTransfer);

      RIBufferTextureCopyDesc copyRegion = {};
      copyRegion.bufferOffset = 0;
      copyRegion.bufferRowLength = (uint32_t)(whiteRowPitch / 4);
      copyRegion.bufferImageHeight = 1;
      copyRegion.bytesPerRow = (uint32_t)whiteRowPitch;
      copyRegion.bytesPerImage = (uint32_t)whiteRowPitch;
      copyRegion.mipLevel = 0;
      copyRegion.arrayLayer = 0;
      copyRegion.x = 0;
      copyRegion.y = 0;
      copyRegion.z = 0;
      copyRegion.width = 1;
      copyRegion.height = 1;
      copyRegion.depth = 1;
      initElem.cmds[0].copyBufferToTexture(&device, &whiteUploadStaging,
                                           &whiteTexture2D, copyRegion);

      // afterStages 0 derives the all-shader mask — sampled reads can
      // only happen in shader stages.
      RITextureBarrier toShaderRead = {};
      toShaderRead.texture = &whiteTexture2D;
      toShaderRead.before = RI_RESOURCE_STATE_COPY_DST;
      toShaderRead.beforeStages = RI_STAGE_COPY;
      toShaderRead.after = RI_RESOURCE_STATE_SHADER_RESOURCE;
      toShaderRead.mipCount = 1;
      toShaderRead.layerCount = 1;
      initElem.cmds[0].vk_d3d12_textureBarrier(toShaderRead);

      RITextureViewDesc whiteViewDesc = {};
      whiteViewDesc.viewType = RI_VIEWTYPE_SHADER_RESOURCE_2D;
      whiteViewDesc.format = RI_FORMAT_RGBA8_UNORM;
      whiteViewDesc.mipNum = 1;
      whiteViewDesc.layerNum = 1;
      whiteTexture2DView =
          RITextureView::create(&device, &whiteTexture2D, whiteViewDesc);
      if (whiteTexture2DView.isEmpty()) {
        FatalError("Failed to create white texture image view!\n");
      }
    }

    // Zero-filled vertex buffer — small mapped buffer, never modified after
    // init.
    {
      constexpr RIDeviceSize kNulVertexSize = 64;
      nulVertexBuffer =
          RIBuffer::create(&device, {(uint64_t)kNulVertexSize,
                                     RI_BUFFER_USAGE_VERTEX_BUFFER |
                                         RI_BUFFER_USAGE_TRANSFER_DST,
                                     RI_MEMORY_HOST_UPLOAD, 0});
      if (nulVertexBuffer.isEmpty()) {
        FatalError("Failed to create null vertex buffer!\n");
      }
      if (nulVertexBuffer.mappedAddress == nullptr) {
        FatalError("Failed to map null vertex buffer!\n");
      }
      memset(nulVertexBuffer.mappedAddress, 0, kNulVertexSize);
      nulVertexBuffer.flushMappedRange(&device, 0, 0);
    }

    // Default-value fallback vertex streams (see Graphics.h). Each is a
    // single vertex, host-mapped and filled once here; bound for
    // renderables that omit an optional stream, where the pipeline zeroes
    // that binding's stride so this one element feeds every vertex.
    {
      struct FallbackSpec {
        struct RIBuffer *target;
        uint32_t size; // single-vertex byte size (the binding stride)
        float value[4];
      };
      const FallbackSpec specs[] = {
          {&fallbackNormalVertex,
           sizeof(float) * 3,
           {0.f, 0.f, 1.f, 0.f}}, // +Z
          {&fallbackTangentVertex,
           sizeof(float) * 4,
           {1.f, 0.f, 0.f, 1.f}}, // +X, handedness +1
          {&fallbackColorVertex,
           sizeof(float) * 4,
           {1.f, 1.f, 1.f, 1.f}}, // white
          {&fallbackUv0Vertex,
           sizeof(float) * 3,
           {0.f, 0.f, 0.f, 0.f}}, // origin
      };
      for (const FallbackSpec &s : specs) {
        *s.target = RIBuffer::create(&device, {(uint64_t)s.size,
                                               RI_BUFFER_USAGE_VERTEX_BUFFER |
                                                   RI_BUFFER_USAGE_TRANSFER_DST,
                                               RI_MEMORY_HOST_UPLOAD, 0});
        if (s.target->isEmpty()) {
          FatalError("Failed to create fallback vertex buffer!\n");
        }
        if (s.target->mappedAddress == nullptr) {
          FatalError("Failed to map fallback vertex buffer!\n");
        }
        std::memcpy(s.target->mappedAddress, s.value, s.size);
        s.target->flushMappedRange(&device, 0, 0);
      }
    }

    initElem.cmds[0].end(&device);

    RICmd *initCmds[] = {&initElem.cmds[0]};
    RISubmitDesc initSubmit = {};
    initSubmit.cmds = initCmds;
    initSubmit.cmdCount = 1;
    initSubmit.completion = nullptr;
    if (device.queues[RI_QUEUE_GRAPHICS].submit(&device, initSubmit) !=
        RI_SUCCESS) {
      FatalError("Failed to submit fallback-resource initialization!\n");
    }
    device.queues[RI_QUEUE_GRAPHICS].waitIdle(&device);

    if (!whiteUploadStaging.isEmpty()) {
      whiteUploadStaging.dispose(&device);
    }
    // The one-time init upload used graphicsCmdRing, but it has completed.
    // Rewind the ring so the first real frame starts with a clean command pool.
    graphicsCmdRing.cmdIndex = 0;
    graphicsCmdRing.fenceIndex = 0;
    primary = {};
  }
  {
    auto vert_stage = RIProgram::loadShaderStage(apResources->GetFileSearcher(),
                                                 "gui.vert");
    auto frag_stage = RIProgram::loadShaderStage(apResources->GetFileSearcher(),
                                                 "gui.frag");
    std::array<RIProgram::ModuleStage, 2> stages = {
        RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, vert_stage,
                               "vsMain"},
        RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, frag_stage,
                               "psMain"}};
    gui.initialize(&device, stages, {}, "gui");
  }
  {
    auto vert_stage = RIProgram::loadShaderStage(
        apResources->GetFileSearcher(), "posteffect_fullscreen.vert");
    auto frag_stage = RIProgram::loadShaderStage(apResources->GetFileSearcher(),
                                                 "posteffect_present.frag");
    std::array<RIProgram::ModuleStage, 2> stages = {
        RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, vert_stage,
                               "vsMain"},
        RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, frag_stage,
                               "psMain"}};
    postEffectBlit.initialize(&device, stages, {}, "postEffectBlit");
  }

  // Build the engine-lifetime global managed set (set 0) before any renderer
  // or texture needs it. cTextureManager (already constructed with cResources)
  // writes texture descriptors into it; renderers borrow its layout.
  InitGlobalManagedSets(&device, apResources);

  // Gameplay illumination sensor. Lives for the life of the device: its buffers
  // are a few hundred bytes and the alternative (allocating per map) would make
  // the first sensor reading of every map a fallback.
  lightProbe = hplNew(cLightProbeQuery, ());
  lightProbe->Init(&device);

  // GPU particle pool. Allocated unconditionally so the buffers exist for the
  // whole device lifetime, but AcquireSlice is only ever called when
  // cGpuParticleSystem::Enabled() is true, so an unset HPL_GPU_PARTICLES costs
  // one allocation and nothing else.
  gpuParticles = hplNew(cGpuParticleSystem, (this));
  if (!gpuParticles->IsReady())
    Log("GPU particles: pool unavailable; emitters use the CPU path\n");
  else
    Log("GPU particles: pool ready (%s)\n",
        cGpuParticleSystem::Enabled() ? "enabled" : "gated off");

  ////////////////////////////////////////////////
  // Create systems
  mpMeshCreator = hplNew(cMeshCreator, (apResources));
  mpTextureCreator = hplNew(cTextureCreator, (apResources));
  mpDecalCreator = hplNew(cDecalCreator, (apResources));

  // Create Renderers
  if (alHplSetupFlags & eHplSetup_Screen) {

    mvRenderers.resize(eRenderer_LastEnum, NULL);

    // cHybridRenderer's constructor creates ray-tracing pipelines with no
    // capability check, so it may only ever be built on a device that actually
    // came up ray-tracing-capable — not merely on an adapter that could.
    const bool bDeviceRayTraced = device.accelerationStructureEnabled &&
                                  device.rayTracingPipelineEnabled &&
                                  device.rayQueryEnabled;
    const bool bBuildBoth = mbRuntimeBackendSwitchAllowed && bDeviceRayTraced;

    if (bBuildBoth || mRendererBackend == eRendererBackend_Standard)
      mLitRenderers[eRendererBackend_Standard] =
          hplNew(cStandardRenderer, (this, apResources));
    if (bBuildBoth ||
        (mRendererBackend == eRendererBackend_RayTraced && bDeviceRayTraced))
      mLitRenderers[eRendererBackend_RayTraced] =
          hplNew(cHybridRenderer, (this, apResources));

    // Ray traced was asked for on a device that cannot do it. The adapter check
    // before device creation normally catches this; this is the belt-and-braces
    // path for a device that came up without what its adapter advertised.
    if (mLitRenderers[mRendererBackend] == NULL) {
      mRendererBackend = eRendererBackend_Standard;
      mbRayTracedSupported = false;
      if (mLitRenderers[eRendererBackend_Standard] == NULL)
        mLitRenderers[eRendererBackend_Standard] =
            hplNew(cStandardRenderer, (this, apResources));
    }

    Log("Renderer backend: %s%s\n",
        mRendererBackend == eRendererBackend_Standard ? "Standard" : "Ray Traced",
        bBuildBoth ? " (runtime switching available)" : "");

    for (int i = 0; i < eRendererBackend_LastEnum; ++i) {
      if (mLitRenderers[i])
        mvOwnedRenderers.push_back(mLitRenderers[i]);
    }

    mvRenderers[eRenderer_Main] = mLitRenderers[mRendererBackend];
    mvRenderers[eRenderer_WireFrame] =
        hplNew(cRendererWireFrame, (this, apResources));
    mvRenderers[eRenderer_Simple] =
        hplNew(cRendererSimple, (this, apResources));
    mvOwnedRenderers.push_back(mvRenderers[eRenderer_WireFrame]);
    mvOwnedRenderers.push_back(mvRenderers[eRenderer_Simple]);

    // Editor / debug overlay batcher — flushed by HybridRenderer's
    // offscreen tail (and reusable for thumbnails / previews).
    mpDebugDraw = hplNew(DebugDraw, ());
    mpDebugDraw->Init(apResources);

    // for(size_t i=0; i<mvRenderers.size(); ++i)
    //{
    //	if(mvRenderers[i])
    //	{
    //		if(mvRenderers[i]->LoadData()==false)
    //		{
    //			FatalError("Renderer #%d could not be initialized! Make
    //sure your graphic card drivers are up to date. Check log file for more
    //information.\n", i);
    //		}
    //	}
    // }
  }

  ////////////////////////////////////////////////
  // Create Data
  if (alHplSetupFlags & eHplSetup_Screen) {
    ////////////////////////////////////////////////
    // Add all the materials.
    Log(" Adding engine materials\n");

    AddMaterialType(hplNew(cMaterialType_SolidDiffuse, (this, apResources)),
                    "soliddiffuse");
    AddMaterialType(hplNew(cMaterialType_Translucent, (this, apResources)),
                    "translucent");
    AddMaterialType(hplNew(cMaterialType_Water, (this, apResources)), "water");
    AddMaterialType(hplNew(cMaterialType_Decal, (this, apResources)), "decal");

    ////////////////////////////////////////////////
    // Add all the post effects
    Log(" Adding engine post effects\n");
    AddPostEffectType(hplNew(cPostEffectType_Bloom, (this, apResources)));
    AddPostEffectType(hplNew(cPostEffectType_ToneMap, (this, apResources)));
    AddPostEffectType(
        hplNew(cPostEffectType_ColorConvTex, (this, apResources)));
    AddPostEffectType(hplNew(cPostEffectType_ImageTrail, (this, apResources)));
    AddPostEffectType(hplNew(cPostEffectType_RadialBlur, (this, apResources)));
  }

  Log("--------------------------------------------------------\n\n");
}

//-----------------------------------------------------------------------

void cGraphics::Update(float afTimeStep) {
  // Every renderer built, including an inactive lit one, so a backend switch
  // never hands out a renderer that missed a frame of updates.
  for (size_t i = 0; i < mvOwnedRenderers.size(); ++i)
    mvOwnedRenderers[i]->Update(afTimeStep);
}

//-----------------------------------------------------------------------

iRenderer *cGraphics::GetRenderer(eRenderer aType) {
  if (aType >= (int)mvRenderers.size())
    return NULL;

  return mvRenderers[aType];
}

//-----------------------------------------------------------------------

iRenderer *cGraphics::GetLitRenderer(eRendererBackend aBackend) {
  if (aBackend < 0 || aBackend >= eRendererBackend_LastEnum)
    return NULL;

  return mLitRenderers[aBackend];
}

//-----------------------------------------------------------------------

bool cGraphics::CanSwitchRendererBackend() const {
  return mLitRenderers[eRendererBackend_Standard] != NULL &&
         mLitRenderers[eRendererBackend_RayTraced] != NULL;
}

//-----------------------------------------------------------------------

bool cGraphics::SetRendererBackend(eRendererBackend aBackend) {
  if (aBackend < 0 || aBackend >= eRendererBackend_LastEnum)
    return false;
  if (aBackend == mRendererBackend)
    return true;
  if (mLitRenderers[aBackend] == NULL)
    return false;
  if (eRenderer_Main >= (int)mvRenderers.size())
    return false;

  mRendererBackend = aBackend;
  mvRenderers[eRenderer_Main] = mLitRenderers[aBackend];

  return true;
}

//-----------------------------------------------------------------------

void cGraphics::RequestRendererBackend(eRendererBackend aBackend) {
  mRequestedBackend = aBackend;
  mbBackendSwitchPending = true;
}

//-----------------------------------------------------------------------

bool cGraphics::CanHostRendererBackend(eRendererBackend aBackend) const {
  if (aBackend == eRendererBackend_Standard)
    return true;
  // cHybridRenderer's constructor creates ray-tracing pipelines with no
  // capability check, so only a device that actually came up ray-tracing
  // capable can host it -- an adapter that merely could is not enough.
  return device.accelerationStructureEnabled && device.rayTracingPipelineEnabled &&
         device.rayQueryEnabled;
}

//-----------------------------------------------------------------------

void cGraphics::SetRendererBackendHandlers(std::function<void(iRenderer *)> aDetach,
                                           std::function<void(eRendererBackend)> aAdopt) {
  mBackendDetachHandler = std::move(aDetach);
  mBackendAdoptHandler = std::move(aAdopt);
}

//-----------------------------------------------------------------------

// Runs at the top of BeginActiveSet, where no command buffer is recording and
// the swapchain has not been acquired yet. Everything here is one synchronous
// block: the stall the player sees when they change the setting.
void cGraphics::ApplyPendingBackendSwitch() {
  if (!mbBackendSwitchPending)
    return;
  // Cleared first, so a request raised from anything this switch calls lands on
  // the NEXT boundary instead of recursing.
  mbBackendSwitchPending = false;

  const eRendererBackend target = mRequestedBackend;

  cRendererBackendSwitchRequest request;
  request.mbPending = true;
  request.mRequested = target;
  request.mCurrent = mRendererBackend;
  request.mbDeviceCanRayTrace = CanHostRendererBackend(eRendererBackend_RayTraced);
  request.mbHaveRenderers =
      !mvRenderers.empty() && eRenderer_Main < (int)mvRenderers.size();

  const eRendererBackendSwitch decision = EvaluateRendererBackendSwitch(request);
  if (decision == eRendererBackendSwitch_Refuse) {
    Warning("Renderer backend: cannot switch to %s here; keeping %s\n",
            target == eRendererBackend_Standard ? "Standard" : "Ray Traced",
            mRendererBackend == eRendererBackend_Standard ? "Standard" : "Ray Traced");
    return;
  }
  if (decision != eRendererBackendSwitch_Apply)
    return;

  iRenderer *pOutgoing = mLitRenderers[mRendererBackend];

  // Nothing submitted may still reference the renderer being destroyed:
  // ~cHybridRenderer disposes its programs and SBT immediately rather than
  // through graphicsDefer.
  device.queues[RI_QUEUE_GRAPHICS].waitIdle(&device);

  // Viewports let go BEFORE the destroy. Not just to avoid a dangling pointer:
  // cViewport::SetRenderer early-outs on pointer equality, so if the incoming
  // renderer lands on the address the outgoing one just freed, re-stamping
  // would silently no-op and the viewport would keep a stale temporal history.
  if (mBackendDetachHandler && pOutgoing)
    mBackendDetachHandler(pOutgoing);

  if (pOutgoing) {
    pOutgoing->DestroyData();
    for (size_t i = 0; i < mvOwnedRenderers.size(); ++i) {
      if (mvOwnedRenderers[i] == pOutgoing) {
        mvOwnedRenderers.erase(mvOwnedRenderers.begin() + i);
        break;
      }
    }
    mLitRenderers[mRendererBackend] = NULL;
    hplDelete(pOutgoing);
  }

  // Turn the deferred releases into actual frees before the incoming renderer
  // allocates, so peak memory is one renderer rather than two. Safe here only
  // because of the waitIdle above.
  graphicsDefer.drainAll();

  iRenderer *pIncoming =
      target == eRendererBackend_Standard
          ? static_cast<iRenderer *>(hplNew(cStandardRenderer, (this, mpResources)))
          : static_cast<iRenderer *>(hplNew(cHybridRenderer, (this, mpResources)));
  mLitRenderers[target] = pIncoming;
  mvOwnedRenderers.push_back(pIncoming);

  // Only now is the backend the new one: a future fallible build wants a
  // coherent rollback point, and a crash dump should name the backend that was
  // actually running.
  mRendererBackend = target;
  mvRenderers[eRenderer_Main] = pIncoming;

  // Re-stamps viewports and every live world, which is what re-resolves the
  // lights and publishes the active renderer mask.
  if (mBackendAdoptHandler)
    mBackendAdoptHandler(target);

  Log("Renderer backend: switched to %s\n",
      target == eRendererBackend_Standard ? "Standard" : "Ray Traced");
}

//-----------------------------------------------------------------------

eRendererBackend cGraphics::GetRendererBackend() const {
  return mRendererBackend;
}

//-----------------------------------------------------------------------

void cGraphics::ReloadRendererData() {
  for (size_t i = 0; i < mvOwnedRenderers.size(); ++i) {
    iRenderer *pRenderer = mvOwnedRenderers[i];

    pRenderer->DestroyData();
    pRenderer->LoadData();
  }
}

//-----------------------------------------------------------------------

cPostEffectComposite *cGraphics::CreatePostEffectComposite() {
  cPostEffectComposite *pComposite = hplNew(cPostEffectComposite, (this));
  mlstPostEffectComposites.push_back(pComposite);

  return pComposite;
}

void cGraphics::DestroyPostEffectComposite(cPostEffectComposite *apComposite) {
  STLFindAndDelete(mlstPostEffectComposites, apComposite);
}

//-----------------------------------------------------------------------

void cGraphics::AddPostEffectType(iPostEffectType *apPostEffectBase) {
  mvPostEffectTypes.push_back(apPostEffectBase);
}

//-----------------------------------------------------------------------

iPostEffect *cGraphics::CreatePostEffect(iPostEffectParams *apParams) {
  iPostEffectType *pType =
      (iPostEffectType *)STLFindByName(mvPostEffectTypes, apParams->GetName());
  if (pType == NULL) {
    Error("Could not find post effect type %s\n", apParams->GetName().c_str());
    return NULL;
  }

  iPostEffect *pPostEffect = pType->CreatePostEffect(apParams);
  pPostEffect->SetParams(apParams);

  mlstPostEffects.push_back(pPostEffect);

  return pPostEffect;
}

//-----------------------------------------------------------------------

void cGraphics::DestroyPostEffect(iPostEffect *apPostEffect) {
  STLFindAndDelete(mlstPostEffects, apPostEffect);
}

//-----------------------------------------------------------------------

void cGraphics::AddMaterialType(iMaterialType *apType, const tString &asName) {
  apType->SetName(asName);
  apType->LoadData();
  m_mapMaterialTypes.insert(tMaterialTypeMap::value_type(asName, apType));
}

iMaterialType *cGraphics::GetMaterialType(const tString &asName) {
  tString sLowName = cString::ToLowerCase(asName);

  tMaterialTypeMapIt it = m_mapMaterialTypes.find(sLowName);
  if (it == m_mapMaterialTypes.end())
    return NULL;

  return it->second;
}

tStringVec cGraphics::GetMaterialTypeNames() {
  tStringVec vNames;
  tMaterialTypeMapIt it = m_mapMaterialTypes.begin();
  for (; it != m_mapMaterialTypes.end(); ++it) {
    vNames.push_back(it->first);
  }

  return vNames;
}

void cGraphics::ReloadMaterials() {
  tMaterialTypeMapIt it = m_mapMaterialTypes.begin();
  for (; it != m_mapMaterialTypes.end(); ++it) {
    iMaterialType *pType = it->second;
    pType->Reload();
  }
}

//-----------------------------------------------------------------------
// Render-interface frame orchestration (merged from the former RIBootstrap).
// These are cGraphics members, so they touch device/swapchain/etc. directly.
//-----------------------------------------------------------------------

void cGraphics::IncrementFrame() { frameIndex++; }

RIDescriptor cGraphics::whiteTexture2DDescriptor() {
  return RIDescriptor::sampledImage(&device, &whiteTexture2DView);
}

// Shared grow-and-recreate for the per-frame scratch buffers: try to claim a
// segment; on miss, grow the allocator ×1.5 until the request fits, recreate
// the host-mapped buffer (old one parked on the freelist so in-flight frames
// keep their data) and claim from the fresh allocator.
bool cGraphics::RequestScratchSegment(
    RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS> &alloc,
    RISharedPointer<RIBuffer> &buffer, uint16_t elementStride, uint32_t usage,
    size_t numElements, struct RISegmentReq *req) {
  if (!buffer.isEmpty() && alloc.request(frameIndex, numElements, req)) {
    return true;
  }

  struct RISegmentAllocDesc segmentAllocDesc = {0};
  segmentAllocDesc.numSegments = RI_NUMBER_FRAMES_FLIGHT;
  segmentAllocDesc.elementStride = elementStride;
  segmentAllocDesc.maxElements =
      static_cast<uint32_t>(std::max<size_t>(alloc.maxElements, 4096));
  do {
    segmentAllocDesc.maxElements =
        segmentAllocDesc.maxElements + (segmentAllocDesc.maxElements >> 1);
  } while (segmentAllocDesc.maxElements < numElements);
  alloc = RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS>(&segmentAllocDesc);
  if (!alloc.request(frameIndex, numElements, req)) {
    assert(false);
    return false;
  }

  if (!buffer.isEmpty()) {
    graphicsDefer.push(buffer);
  }
  buffer = RISharedPointer<RIBuffer>(
      &device,
      RIBuffer::create(&device, {(uint64_t)segmentAllocDesc.maxElements *
                                     segmentAllocDesc.elementStride,
                                 usage, RI_MEMORY_HOST_UPLOAD, 0}));
  return true;
}

bool cGraphics::RequestTranslucentVtx(FrameContext *cntx, size_t numFloats,
                                      struct RISegmentReq *req) {
  // SHADER_DEVICE_ADDRESS: the hybrid ParticlePass pulls these streams via BDA.
  return RequestScratchSegment(
      translucentVtxAlloc, translucentVtxBuffer, sizeof(float),
      RI_BUFFER_USAGE_VERTEX_BUFFER | RI_BUFFER_USAGE_DEVICE_ADDRESS, numFloats,
      req);
}

bool cGraphics::RequestTranslucentIdx(FrameContext *cntx, size_t numIndices,
                                      struct RISegmentReq *req) {
  return RequestScratchSegment(
      translucentIdxAlloc, translucentIdxBuffer, sizeof(uint32_t),
      RI_BUFFER_USAGE_INDEX_BUFFER | RI_BUFFER_USAGE_DEVICE_ADDRESS, numIndices,
      req);
}

void cGraphics::Dispose() {
  if (m_disposed || device.vk.device == VK_NULL_HANDLE)
    return;
  m_disposed = true;

  device.queues[RI_QUEUE_GRAPHICS].waitIdle(&device);

  // The GPU is idle; release everything still deferred (including what
  // ~cResources pushed after DestroyRenderObjects) while the device and
  // this object's Interface registration are both still valid.
  graphicsDefer.drainAll();

  // RISharedPointer members would otherwise auto-dispose during member
  // destruction — after device.dispose() below. Release them explicitly
  // while the device is alive.
  translucentVtxBuffer = {};
  translucentIdxBuffer = {};
  guiVertexBuffer = {};
  guiIndexBuffer = {};

  // Free the owned samplers (cookie != 0 marks an occupied slot).
  for (size_t i = 0; i < cachedSamplers.size(); i++) {
    if (cachedSamplers[i].cookie) {
      cachedSamplers[i].dispose(&device);
      cachedSamplers[i] = RISampler{};
    }
  }

  whiteTexture2DView.dispose(&device);
  whiteTexture2D.dispose(&device);

  nulVertexBuffer.dispose(&device);
  fallbackNormalVertex.dispose(&device);
  fallbackTangentVertex.dispose(&device);
  fallbackColorVertex.dispose(&device);
  fallbackUv0Vertex.dispose(&device);

  gui.dispose(&device);
  postEffectBlit.dispose(&device);

  profiler.dispose(&device);
  graphicsTimeline.dispose(&device);

  for (auto &set : frameSets) {
    FreeRIScratchAlloc(&device, &set.uboScratchAlloc);
    if (device.accelerationStructureEnabled)
      FreeRIScratchAlloc(&device, &set.accelScratchAlloc);
  }

  graphicsCmdRing.dispose(&device);
  // Non-owning views into the ring — zero so no dangling handles survive.
  primary = {};
  blasSubmit = {};

  // The swapchain owns its per-image views + semaphores (need the device)
  // AND the surface (needs the instance) — both still alive here. Releasing
  // the live swapchain's last ref (RISwapchain::dispose) frees all three:
  // views/semaphores, the VkSwapchainKHR, then the surface. All retired
  // swapchains were already drained (graphicsDefer.drainAll above) and had
  // transferred the surface away, so this last live swapchain frees it.
  swapchain = {};

  RI_FreeResourceUploader(&device, &uploader);

  // Every VMA allocation must be gone by now (vmaDestroyAllocator asserts
  // on leaks in debug), then the instance goes last.
  device.dispose();
  ShutdownRIRenderer();
}

void cGraphics::SetVsync(bool vsync) {
  m_requestedVsync = vsync;
}

void cGraphics::CloseAndSubmitActiveSet() {
  // The frame loop presents to the swapchain — it must not run headless
  // (Init without eHplSetup_Screen never creates one).
  assert(!swapchain.isEmpty() && swapchain->IsValid() &&
         "CloseAndSubmitActiveSet requires a swapchain (eHplSetup_Screen)");
  FrameContext *cntx = GetActiveSet();

  if (!m_frameAcquired) {
    // No valid swapchain image for this frame (minimized / zero-size window,
    // or the acquire returned OUT_OF_DATE and flagged a rebuild for the next
    // boundary). Submit and present nothing — waiting on the never-signaled
    // acquire semaphore is exactly what deadlocks the queue. Keep the command
    // ring and frame counter consistent: end the (begun) command buffers so the
    // later vkResetCommandPool can recycle them, and advance the frame index
    // in lockstep with BeginActiveSet's graphicsCmdRing.advance(). Touch no
    // fences (they stay signaled) and reserve no timeline value.
    profiler.endFrame(&primary.cmds[0], false);
    primary.cmds[0].end(&device);
    if (device.accelerationStructureEnabled)
      blasSubmit.cmds[0].end(&device);
    IncrementFrame();
    return;
  }

  struct RIQueue *graphicsQueue = &device.queues[RI_QUEUE_GRAPHICS];

  {
    // Swapchain image: COLOR -> PRESENT for the queue present.
    RITextureBarrier toPresent = {};
    toPresent.texture = &swapchain->textures[swapchainIndex];
    toPresent.before = RI_RESOURCE_STATE_RENDER_TARGET_READ;
    toPresent.after = RI_RESOURCE_STATE_PRESENT;
    primary.cmds[0].vk_d3d12_textureBarrier(toPresent);
  }
  profiler.endFrame(&primary.cmds[0]);
  primary.cmds[0].end(&device);
  if (device.accelerationStructureEnabled)
    blasSubmit.cmds[0].end(&device);

#if (DEVICE_IMPL_D3D12)
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12)) {
    // The uploader owns its transfer timeline. Keep the explicit queue wait so
    // this remains correct if the uploader ever uses a different queue.
    const RIResourceUploaderD3D12Result uploadResult =
        RI_D3D12FlushResourceUpdate(&device, &uploader);

    if (!uploadResult.success) {
      profiler.endFrame(&primary.cmds[0], false);
      IncrementFrame();
      return;
    }

    RITimelineOp uploadWait = {};
    if (uploadResult.signaled) {
      uploadWait.timeline = uploadResult.timeline;
      uploadWait.value = uploadResult.value;
      uploadWait.stages = RI_STAGE_NONE;
    }

    if (device.accelerationStructureEnabled) {
      RISubmitDesc blasDesc = {};
      RICmd *blasCmds[] = {&blasSubmit.cmds[0]};
      blasDesc.cmds = blasCmds;
      blasDesc.cmdCount = 1;
      blasDesc.waits = uploadResult.signaled ? &uploadWait : nullptr;
      blasDesc.waitCount = uploadResult.signaled ? 1u : 0u;
      blasDesc.completion = &blasSubmit;
      if (graphicsQueue->submit(&device, blasDesc) == RI_FAIL) {
        profiler.endFrame(&primary.cmds[0], false);
        IncrementFrame();
        return;
      }
    }

    const uint64_t frameTimelineValue = graphicsTimeline.next();
    // A D3D12 queue is ordered, so the BLAS list follows the uploader and
    // precedes the primary list. The primary signal is the frame lifetime
    // token used by deferred resources and profiler resolution.
    RITimelineOp graphicsSignal = {&graphicsTimeline, frameTimelineValue,
                                   RI_STAGE_NONE};
    RISubmitDesc primaryDesc = {};
    RICmd *primaryCmds[] = {&primary.cmds[0]};
    primaryDesc.cmds = primaryCmds;
    primaryDesc.cmdCount = 1;
    // Without a BLAS submission there is no ordered intermediate queue wait,
    // so make the primary list wait for resource uploads directly.
    if (!device.accelerationStructureEnabled && uploadResult.signaled) {
      primaryDesc.waits = &uploadWait;
      primaryDesc.waitCount = 1;
    }
    primaryDesc.signals = &graphicsSignal;
    primaryDesc.signalCount = 1;
    primaryDesc.completion = &primary;
    if (graphicsQueue->submit(&device, primaryDesc) == RI_FAIL) {
      profiler.endFrame(&primary.cmds[0], false);
      IncrementFrame();
      return;
    }

    // Capture the command submission completion before Present adds its own
    // queue-fence stamp. This is also the value used if Present fails.
    swapchain->d3d12.frameFenceValues[swapchainIndex] = primary.d3d12.value;
    const RISwapchainStatus_e presentStatus =
        RISwapchainPresent(&device, swapchain.Get());
    if (presentStatus == RI_SWAPCHAIN_STATUS_OUT_OF_DATE)
      m_forceSwapchainRebuild = true;

    graphicsDefer.seal(frameTimelineValue);
    RISealRetiredBuffers(&device, frameTimelineValue);
    IncrementFrame();
    return;
  }
#endif

  // Flush pending resource uploads once, up front, so both the BLAS submit
  // and the primary chain off it.
  RIResourceUploaderVKResult uploadResult =
      RI_VKFlushResourceUpdate(&device, &uploader, 0, NULL);

  // Submit the dedicated BLAS-build command buffer ahead of the primary.
  if (device.accelerationStructureEnabled) {
    VkCommandBufferSubmitInfo blasCmd = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    blasCmd.commandBuffer = blasSubmit.cmds[0].vk.cmd;

    VkSemaphoreSubmitInfo blasWait = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    blasWait.semaphore = uploadResult.vk.semaphore;
    blasWait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkSemaphoreSubmitInfo blasSignal = {
        VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    blasSignal.semaphore = blasSubmit.vk.semaphore;
    blasSignal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkSubmitInfo2 blasSubmitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    blasSubmitInfo.commandBufferInfoCount = 1;
    blasSubmitInfo.pCommandBufferInfos = &blasCmd;
    blasSubmitInfo.waitSemaphoreInfoCount = uploadResult.signaled ? 1u : 0u;
    blasSubmitInfo.pWaitSemaphoreInfos = &blasWait;
    blasSubmitInfo.signalSemaphoreInfoCount = 1;
    blasSubmitInfo.pSignalSemaphoreInfos = &blasSignal;

    if (!VK_WrapResult(vkResetFences(device.vk.device, 1, &blasSubmit.vk.fence)) ||
        !VK_WrapResult(vkQueueSubmit2(graphicsQueue->vk.queue, 1,
                                      &blasSubmitInfo, blasSubmit.vk.fence))) {
      profiler.endFrame(&primary.cmds[0], false);
      IncrementFrame();
      return;
    }
  }
  {
    VkCommandBufferSubmitInfo cmdSubmitInfo = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmdSubmitInfo.commandBuffer = primary.cmds[0].vk.cmd;

    VkSubmitInfo2 submitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submitInfo.pCommandBufferInfos = &cmdSubmitInfo;
    submitInfo.commandBufferInfoCount = 1;

    // Wait on acquire (layout transition), and on the uploader directly when
    // acceleration structures are disabled (the enabled path chains through
    // the BLAS submit above).
    VkSemaphoreSubmitInfo waitInfos[2] = {};
    uint32_t waitCount = 0;
    if (!device.accelerationStructureEnabled && uploadResult.signaled) {
      waitInfos[waitCount].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
      waitInfos[waitCount].semaphore = uploadResult.vk.semaphore;
      waitInfos[waitCount].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
      waitCount++;
    }
    waitInfos[waitCount].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    waitInfos[waitCount].semaphore =
        swapchain->vk.imageAcquireSem[swapchain->vk.frameIndex];
    waitInfos[waitCount].stageMask =
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    waitCount++;
    if (device.accelerationStructureEnabled) {
      waitInfos[waitCount].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
      waitInfos[waitCount].semaphore = blasSubmit.vk.semaphore;
      waitInfos[waitCount].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
      waitCount++;
    }
    submitInfo.waitSemaphoreInfoCount = waitCount;
    submitInfo.pWaitSemaphoreInfos = waitInfos;

    // Reserve this frame's timeline value; parked resources latch to it.
    const uint64_t frameTimelineValue = graphicsTimeline.next();

    VkSemaphoreSubmitInfo signalInfos[2] = {};
    signalInfos[0].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signalInfos[0].semaphore =
        swapchain->vk.finishSem[swapchain->vk.frameIndex];
    signalInfos[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    signalInfos[1].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signalInfos[1].semaphore = graphicsTimeline.vk.semaphore;
    signalInfos[1].value = frameTimelineValue;
    signalInfos[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    submitInfo.signalSemaphoreInfoCount = 2;
    submitInfo.pSignalSemaphoreInfos = signalInfos;

    if (!VK_WrapResult(vkResetFences(device.vk.device, 1, &primary.vk.fence)) ||
        !VK_WrapResult(vkQueueSubmit2(graphicsQueue->vk.queue, 1, &submitInfo,
                                      primary.vk.fence))) {
      profiler.endFrame(&primary.cmds[0], false);
      IncrementFrame();
      return;
    }
    const RISwapchainStatus_e presentStatus =
        RISwapchainPresent(&device, swapchain.Get());
    if (presentStatus == RI_SWAPCHAIN_STATUS_OUT_OF_DATE) {
      // The present was rejected: its wait on finishSem[frameIndex] did
      // NOT run, so that binary semaphore is left signaled. Reusing this
      // swapchain would double-signal it and deadlock the next submit, so
      // force a fresh rebuild before the next acquire.
      m_forceSwapchainRebuild = true;
    }
    // SUBOPTIMAL needs no action: the present executed (finishSem consumed),
    // and BeginActiveSet's window-vs-swapchain compare rebuilds next frame
    // if the size actually changed.

    // Seal this frame's deferred destroys against the timeline value.
    graphicsDefer.seal(frameTimelineValue);
    RISealRetiredBuffers(&device, frameTimelineValue);
  }
  IncrementFrame();
}

void cGraphics::BeginActiveSet() {
  // See CloseAndSubmitActiveSet — the frame loop is swapchain-bound.
  assert(!swapchain.isEmpty() && swapchain->IsValid() &&
         "BeginActiveSet requires a swapchain (eHplSetup_Screen)");

  // Before the command ring advances, before the defer queue drains and before
  // any command buffer is begun: the one point in the frame where a renderer
  // may be destroyed. It also lands before this frame's cScene::Render, so the
  // switched-to backend draws this frame rather than one frame late.
  ApplyPendingBackendSwitch();

  FrameContext *cntx = GetActiveSet();

  graphicsCmdRing.advance();
  primary = graphicsCmdRing.acquire(&device, 1);
  if (device.accelerationStructureEnabled)
    blasSubmit = graphicsCmdRing.acquire(&device, 1);
  primary.wait(&device);
  if (device.accelerationStructureEnabled)
    blasSubmit.wait(&device);
  primary.pool->reset(&device);

  const uint64_t completedTimeline = graphicsTimeline.completed(&device);
  graphicsDefer.drain(completedTimeline);
  // Buffers the backend kept alive past dispose (D3D12 raw-SRV registry) are
  // released on the same timeline as graphicsDefer.
  RIReclaimRetiredBuffers(&device, completedTimeline);
  // Read back any GPU timing slot whose frame has finished.
  profiler.resolve(&device, completedTimeline);

  // Periodic residency trace, so a long session leaves a record of whether GPU
  // memory drifts over budget as frame time climbs.
  {
    static std::chrono::steady_clock::time_point lastMemoryLog;
    const auto now = std::chrono::steady_clock::now();
    if (now - lastMemoryLog >= std::chrono::seconds(10)) {
      lastMemoryLog = now;
      const std::string memory = GpuMemoryDiagnostics();
      if (!memory.empty())
        Log("GPU memory: %s  gpu %.2f ms\n", memory.c_str(),
            profiler.lastTotalMs());
    }
  }

  // Recreate the swapchain before we acquire when the live window size (owned
  // by cWindow, polled here each frame) or the requested vsync differs from the
  // live swapchain, or a prior present/acquire OUT_OF_DATE forced a rebuild —
  // mandatory even at unchanged dimensions, because a surface can go out of date
  // without a reported size change and only a fresh swapchain refreshes a
  // possibly-orphaned finish semaphore. Skip while the window can't present
  // (minimized / zero-size) and retry at the next boundary. This is the single
  // place the swapchain is rebuilt; everything else just flags a force. The
  // compare is against the *actual* swapchain extent, so a compositor that
  // clamps the requested size does not cause a per-frame rebuild loop.
  const cVector2l winSize = mpWindow->GetSize();
  const bool bPresentable = winSize.x > 0 && winSize.y > 0 &&
                            !mpWindow->IsMinimized();
  const bool bConfigChanged = swapchain.isEmpty() || !swapchain->IsValid() ||
                              swapchain->width != winSize.x ||
                              swapchain->height != winSize.y ||
                              m_vsync != m_requestedVsync;
  if (bPresentable && (m_forceSwapchainRebuild || bConfigChanged)) {
    m_vsync = m_requestedVsync;

    // Point the desc at the live swapchain: create() reuses its surface
    // (transferring ownership to the new swapchain on success) and hands its
    // VkSwapchainKHR to oldSwapchain so the driver can reuse resources. The
    // swapchain is non-empty here (Init created it, the failure guard below
    // never swaps in an empty one); the window-handle fallback only matters if
    // that ever changes, letting a lost swapchain rebuild a fresh surface.
    RISwapchainDesc desc = {};
    desc.requestImageCount = RI_NUMBER_FRAMES_FLIGHT;
    desc.queue = &device.queues[RI_QUEUE_GRAPHICS];
    desc.width = (uint16_t)winSize.x;
    desc.height = (uint16_t)winSize.y;
    desc.format = RI_SWAPCHAIN_BT709_G22_8BIT;
    desc.vsync = m_vsync;
    if (swapchain.isEmpty())
      desc.source = mpWindow->GetHandle();
    else
      desc.source = swapchain.Get();

    RISwapchain next = RISwapchain::create(&device, desc);

    if (!next.isEmpty()) {
      graphicsDefer.push(swapchain); // ref-counted retire
      swapchain = RISharedPointer<RISwapchain>(&device, next);
      Log("Swapchain recreated: %ux%u vsync:%d\n", swapchain->width,
          swapchain->height, (int)m_vsync);

      swapchainIndex = 0;
      m_forceSwapchainRebuild = false;
      // Resize notification is cWindow's job (cWindow::OnScreenSizeChanged,
      // driven off the SDL resize event via cEngine's bus); this rebuild just
      // keeps the swapchain following the window size and stays silent.
    }
  }

  // Acquire the next swapchain image. On OUT_OF_DATE the acquire semaphore is
  // NOT signaled, so this frame has no usable image: flag a forced rebuild for
  // the next boundary (mandatory even at unchanged size — the surface went out
  // of date) and skip the frame. m_frameAcquired stays false, so the barrier
  // below and CloseAndSubmitActiveSet skip all swapchain work — nothing waits
  // on the unsignaled semaphore. All rebuilds happen at the top of
  // BeginActiveSet; this path only flags. SUBOPTIMAL still yields a usable,
  // signaled image, so we use it and let the next requested-vs-swapchain
  // compare rebuild if the size actually changed.
  m_frameAcquired = false;
  const RISwapchainStatus_e status =
      RISwapchainAcquireNextTexture(&device, swapchain.Get(), &swapchainIndex);
  if (status == RI_SWAPCHAIN_STATUS_OUT_OF_DATE) {
    m_forceSwapchainRebuild = true;
  } else {
    m_frameAcquired = true;
  }

  // cleanup
  RIResetScratchAlloc(&device, &cntx->uboScratchAlloc);
  if (device.accelerationStructureEnabled)
    RIResetScratchAlloc(&device, &cntx->accelScratchAlloc);

  if (device.accelerationStructureEnabled)
    blasSubmit.cmds[0].begin(&device);
  primary.cmds[0].begin(&device);
  // Only touch the swapchain image when we actually acquired one — a skipped
  // frame (see CloseAndSubmitActiveSet) has a stale swapchainIndex.
  if (m_frameAcquired) {
    // Swapchain image: UNDEFINED -> COLOR for the frame.
    RITextureBarrier toColor = {};
    toColor.texture = &swapchain->textures[swapchainIndex];
    toColor.before = RI_RESOURCE_STATE_UNDEFINED;
    toColor.after = RI_RESOURCE_STATE_RENDER_TARGET_READ;
    primary.cmds[0].vk_d3d12_textureBarrier(toColor);
  }

  // Reset this frame's GPU-timing query pool on the primary CB.
  profiler.beginFrame(&primary.cmds[0], frameIndex % RI_NUMBER_FRAMES_FLIGHT,
                      graphicsTimeline.pending() + 1);
}

void cGraphics::UpdateFrameUBO(RIDescriptor *descriptor, void *data,
                               size_t size) {
  auto *activeSet = GetActiveSet();
  struct RIBufferScratchAllocReq scratchReq =
      RIAllocBufferFromScratchAlloc(&device, &activeSet->uboScratchAlloc, size);
  if (scratchReq.pMappedAddress == nullptr ||
      scratchReq.block.buffer.isEmpty() || scratchReq.bufferSize < size) {
    FatalError("Failed to allocate %zu bytes from the frame uniform-buffer "
               "scratch allocator.\n",
               size);
  }
  memcpy((uint8_t *)scratchReq.pMappedAddress + scratchReq.bufferOffset, data,
         size);
  // Transient descriptor shim: cookie derives from the scratch buffer
  // identity folded with the sub-allocation offset/range.
  *descriptor = RIDescriptor::uniformBuffer(&device, &scratchReq.block.buffer,
                                            scratchReq.bufferOffset, size);
  RIFinishScrachReq(&device, &scratchReq);
}

std::string cGraphics::GpuMemoryDiagnostics() const {
  RIMemoryStats stats = {};
  if (!RIQueryMemoryStats(&device, &stats))
    return {};
  constexpr double kMiB = 1024.0 * 1024.0;
  uint32_t descriptorCacheEntries = 0;
#if (DEVICE_IMPL_D3D12)
  descriptorCacheEntries = g_riD3D12DescriptorCacheEntries;
#endif
  char line[256];
  snprintf(line, sizeof(line),
           "VRAM local %.0f/%.0f MB  nonlocal %.0f/%.0f MB  heaps %.0f MB "
           "(live %.0f MB)  retired %.0f MB  reg %u  descCache %u  nrdInst %u",
           stats.localUsage / kMiB, stats.localBudget / kMiB,
           stats.nonLocalUsage / kMiB, stats.nonLocalBudget / kMiB,
           stats.allocatorBlockBytes / kMiB,
           stats.allocatorAllocationBytes / kMiB,
           stats.retiredBufferBytes / kMiB, stats.registeredBuffers,
           descriptorCacheEntries, NrdIntegration::InstancesCreated());
  return line;
}

std::optional<RIDescriptor>
cGraphics::resolve_filter_descriptor(eTextureWrap wrapS, eTextureWrap wrapT,
                                     eTextureWrap wrapR,
                                     eTextureFilter filter) {
  constexpr uint32_t kWrapCount =
      static_cast<uint32_t>(eTextureWrap_LastEnum);
  constexpr uint32_t kFilterCount =
      static_cast<uint32_t>(eTextureFilter_LastEnum);
  const uint32_t wrapSIndex = static_cast<uint32_t>(wrapS);
  const uint32_t wrapTIndex = static_cast<uint32_t>(wrapT);
  const uint32_t wrapRIndex = static_cast<uint32_t>(wrapR);
  const uint32_t filterIndex = static_cast<uint32_t>(filter);

  if (wrapSIndex >= kWrapCount || wrapTIndex >= kWrapCount ||
      wrapRIndex >= kWrapCount || filterIndex >= kFilterCount) {
    assert(false && "Invalid sampler configuration");
    return std::nullopt;
  }

  // Collision-free mixed-radix key in the range [0, 191].
  const size_t cacheIndex =
      (((static_cast<size_t>(wrapSIndex) * kWrapCount + wrapTIndex) *
            kWrapCount +
        wrapRIndex) *
           kFilterCount +
       filterIndex);
  assert(cacheIndex < cachedSamplers.size());
  RISampler &sampler = cachedSamplers[cacheIndex];
  sampler.wrapS = wrapSIndex;
  sampler.wrapT = wrapTIndex;
  sampler.wrapR = wrapRIndex;
  sampler.filter = filterIndex;

  // cookie == 0 means the slot has not been initialized (+1 since index 0 is
  // valid). The cookie is published only after backend initialization succeeds.
  const hash_t samplerCookie = static_cast<hash_t>(cacheIndex) + 1;
  if (sampler.cookie != 0) {
    assert(sampler.cookie == samplerCookie);
    return RIDescriptor::sampler(&device, &sampler);
  }

#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    VkSamplerCreateInfo info = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.addressModeU = RI_VK_TextureWrap(wrapS);
    info.addressModeV = RI_VK_TextureWrap(wrapT);
    info.addressModeW = RI_VK_TextureWrap(wrapR);
    info.compareEnable = VK_FALSE;
    info.anisotropyEnable = VK_FALSE;
    info.minLod = 0.0f;
    info.maxLod = 16.0f;
    switch (filter) {
    case eTextureFilter_Nearest:
      info.minFilter = VK_FILTER_NEAREST;
      info.magFilter = VK_FILTER_NEAREST;
      info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
      break;
    case eTextureFilter_Bilinear:
      info.minFilter = VK_FILTER_LINEAR;
      info.magFilter = VK_FILTER_LINEAR;
      info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
      break;
    case eTextureFilter_Trilinear:
      info.minFilter = VK_FILTER_LINEAR;
      info.magFilter = VK_FILTER_LINEAR;
      info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
      break;
    case eTextureFilter_LastEnum:
      assert(false && "Invalid sampler filter");
      return std::nullopt;
    }
    if (!VK_WrapResult(vkCreateSampler(device.vk.device, &info, nullptr,
                                       &sampler.vk.sampler)))
      return std::nullopt;
    sampler.cookie = samplerCookie;
    return RIDescriptor::sampler(&device, &sampler);
  }
#endif

#if (DEVICE_IMPL_D3D12)
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12)) {
    D3D12_SAMPLER_DESC &desc = sampler.d3d12.desc;
    desc = {};
    switch (wrapS) {
    case eTextureWrap_Repeat: desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP; break;
    case eTextureWrap_Clamp:
    case eTextureWrap_ClampToEdge: desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP; break;
    case eTextureWrap_ClampToBorder: desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER; break;
    case eTextureWrap_LastEnum: assert(false && "Invalid sampler wrap"); return std::nullopt;
    }
    switch (wrapT) {
    case eTextureWrap_Repeat: desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP; break;
    case eTextureWrap_Clamp:
    case eTextureWrap_ClampToEdge: desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP; break;
    case eTextureWrap_ClampToBorder: desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER; break;
    case eTextureWrap_LastEnum: assert(false && "Invalid sampler wrap"); return std::nullopt;
    }
    switch (wrapR) {
    case eTextureWrap_Repeat: desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP; break;
    case eTextureWrap_Clamp:
    case eTextureWrap_ClampToEdge: desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP; break;
    case eTextureWrap_ClampToBorder: desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER; break;
    case eTextureWrap_LastEnum: assert(false && "Invalid sampler wrap"); return std::nullopt;
    }
    switch (filter) {
    case eTextureFilter_Nearest: desc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT; break;
    case eTextureFilter_Bilinear: desc.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT; break;
    case eTextureFilter_Trilinear: desc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR; break;
    case eTextureFilter_LastEnum: assert(false && "Invalid sampler filter"); return std::nullopt;
    }
    desc.MipLODBias = 0.0f;
    desc.MaxAnisotropy = 1;
    desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    desc.BorderColor[0] = 0.0f;
    desc.BorderColor[1] = 0.0f;
    desc.BorderColor[2] = 0.0f;
    desc.BorderColor[3] = 0.0f;
    desc.MinLOD = 0.0f;
    desc.MaxLOD = 16.0f;
    sampler.d3d12.initialized = 1;
    sampler.cookie = samplerCookie;
    return RIDescriptor::sampler(&device, &sampler);
  }
#endif

  return std::nullopt;
}

//-----------------------------------------------------------------------

} // namespace hpl
