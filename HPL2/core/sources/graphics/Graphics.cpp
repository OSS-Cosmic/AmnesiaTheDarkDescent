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
#include "graphics/PostEffect.h"
#include "graphics/PostEffectComposite.h"
#include "graphics/RIRenderer.h"
#include "graphics/RIResourceUploader.h"
#include "graphics/RITypes.h"
#include "graphics/RIVK.h"
#include "graphics/TextureCreator.h"

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
#include <optional>

namespace hpl {

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

  for (size_t i = 0; i < mvRenderers.size(); ++i) {
    if (mvRenderers[i]) {
      mvRenderers[i]->DestroyData();
      hplDelete(mvRenderers[i]);
    }
  }
  mvRenderers.clear();

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
  if (mRendererBackend != eRendererBackend_Standard && mRendererBackend != eRendererBackend_Overdrive) {
    mRendererBackend = eRendererBackend_Overdrive;
  }

  Log("Initializing Graphics Module\n");
  Log("--------------------------------------------------------\n");

  mpResources = apResources;

  ////////////////////////////////////////////////
  // Setup the graphic directories:
  apResources->AddResourceDir(_W("core/shaders"), false);
  apResources->AddResourceDir(_W("core/textures"), false);
  apResources->AddResourceDir(_W("core/models"), false);
  apResources->AddResourceDir(_W("compiled_shaders"), false);

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
  {
    struct RIBackendInit backendInit = {};
    backendInit.api = RI_DEVICE_API_VK;
    backendInit.applicationName = "HPL2";
    // OFF unless the user opts in with HPL_VK_VALIDATION=1, in any build.
    // Nobody should pay for the layer without asking for it, and leaving it on
    // by default also stacks it under driver-debug modes (RADV_DEBUG=hang
    // etc.) -- both intercept submits, and the combination is the least-tested
    // path as well as one that perturbs hang repros.
    //
    // Turn it on while working on the renderer: it is what catches unbound
    // descriptor sets, malformed copies and bad barriers at the call that
    // causes them rather than as corruption several frames later.
    const char *pValidationEnv = getenv("HPL_VK_VALIDATION");
    backendInit.vk.enableValidationLayer =
        pValidationEnv != NULL && atoi(pValidationEnv) != 0;

    if (InitRIRenderer(&backendInit) != RI_SUCCESS) {
      FatalError(
          "Failed to initialize the Vulkan renderer! Make sure your graphics "
          "card supports Vulkan and your drivers are up to date.\n");
    }

    uint32_t numAdapters = 0;
    if (EnumerateRIAdapters(NULL, &numAdapters) != RI_SUCCESS ||
        numAdapters == 0) {
      FatalError("No Vulkan-compatible graphics adapter found! Make sure your "
                 "drivers are up to date.\n");
    }
    std::vector<RIPhysicalAdapter> physicalAdapters(numAdapters);

    if (EnumerateRIAdapters(physicalAdapters.data(), &numAdapters) !=
        RI_SUCCESS) {
      FatalError("Failed to enumerate Vulkan graphics adapters!\n");
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
    struct RIDeviceDesc deviceInit = {0};
    deviceInit.physicalAdapter = &physicalAdapters[selectedAdapterIdx];
    // Only Overdrive needs hardware ray tracing; Standard's raster shaders use
    // no ray query or acceleration structure, so it runs on any Vulkan GPU.
    deviceInit.requestRayTracing =
        (mRendererBackend == eRendererBackend_Overdrive) ? 1 : 0;
    int deviceResult = device.init(&deviceInit);
    bool overdriveFallback = false;
    if (deviceResult != RI_SUCCESS && deviceInit.requestRayTracing) {
      Log("Renderer backend: ray tracing unsupported on '%s', falling back to "
          "Standard\n",
          selectedAdapter.name);
      mRendererBackend = eRendererBackend_Standard;
      overdriveFallback = true;
      deviceInit.requestRayTracing = 0;
      deviceResult = device.init(&deviceInit);
    }
    if (deviceResult != RI_SUCCESS) {
      FatalError("Failed to create Vulkan device on adapter '%s'! Make sure "
                 "your drivers are up to date.\n",
                 selectedAdapter.name);
    }
    if (overdriveFallback)
      mbOverdriveSupported = false;
    else if (mRendererBackend == eRendererBackend_Overdrive)
      mbOverdriveSupported = true;
    else
      mbOverdriveSupported = selectedAdapter.rayTracingTier >= 1 &&
                             selectedAdapter.isRayQuerySupported;
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

    // 1x1 white texture — staged upload, then transitioned to
    // SHADER_READ_ONLY_OPTIMAL.
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

      const uint8_t whitePixel[4] = {255, 255, 255, 255};
      whiteUploadStaging = RIBuffer::create(
          &device, {(uint64_t)sizeof(whitePixel), RI_BUFFER_USAGE_TRANSFER_SRC,
                    RI_MEMORY_HOST_UPLOAD, 0});
      if (whiteUploadStaging.isEmpty()) {
        FatalError("Failed to create white texture staging buffer!\n");
      }
      memcpy(whiteUploadStaging.mappedAddress, whitePixel, sizeof(whitePixel));
      vmaFlushAllocation(device.vk.vmaAllocator,
                         whiteUploadStaging.vk.allocation, 0, VK_WHOLE_SIZE);

      RITextureBarrier toTransfer = {};
      toTransfer.texture = &whiteTexture2D;
      toTransfer.before = RI_RESOURCE_STATE_UNDEFINED;
      toTransfer.after = RI_RESOURCE_STATE_COPY_DST;
      toTransfer.afterStages = RI_STAGE_COPY;
      toTransfer.mipCount = 1;
      toTransfer.layerCount = 1;
      initElem.cmds[0].vk_d3d12_textureBarrier(toTransfer);

      VkBufferImageCopy copyRegion = {};
      copyRegion.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      copyRegion.imageExtent = {1, 1, 1};
      vkCmdCopyBufferToImage(
          initElem.cmds[0].vk.cmd, whiteUploadStaging.vk.buffer,
          whiteTexture2D.vk.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
          &copyRegion);

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
      constexpr VkDeviceSize kNulVertexSize = 64;
      nulVertexBuffer =
          RIBuffer::create(&device, {(uint64_t)kNulVertexSize,
                                     RI_BUFFER_USAGE_VERTEX_BUFFER |
                                         RI_BUFFER_USAGE_TRANSFER_DST,
                                     RI_MEMORY_HOST_UPLOAD, 0});
      if (nulVertexBuffer.isEmpty()) {
        FatalError("Failed to create null vertex buffer!\n");
      }
      if (nulVertexBuffer.mappedAddress) {
        memset(nulVertexBuffer.mappedAddress, 0, kNulVertexSize);
        vmaFlushAllocation(device.vk.vmaAllocator,
                           nulVertexBuffer.vk.allocation, 0, VK_WHOLE_SIZE);
      }
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
        if (s.target->mappedAddress) {
          std::memcpy(s.target->mappedAddress, s.value, s.size);
          vmaFlushAllocation(device.vk.vmaAllocator, s.target->vk.allocation, 0,
                             VK_WHOLE_SIZE);
        }
      }
    }

    initElem.cmds[0].end(&device);

    VkCommandBufferSubmitInfo cmdSubmitInfo = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmdSubmitInfo.commandBuffer = initElem.cmds[0].vk.cmd;

    VkSubmitInfo2 submitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submitInfo.commandBufferInfoCount = 1;
    submitInfo.pCommandBufferInfos = &cmdSubmitInfo;

    VK_WrapResult(vkResetFences(device.vk.device, 1, &initElem.vk.fence));
    VK_WrapResult(vkQueueSubmit2(device.queues[RI_QUEUE_GRAPHICS].vk.queue, 1,
                                 &submitInfo, initElem.vk.fence));
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
                                                 "gui.vert.spv");
    auto frag_stage = RIProgram::loadShaderStage(apResources->GetFileSearcher(),
                                                 "gui.frag.spv");
    std::array<RIProgram::ModuleStage, 2> stages = {
        RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, vert_stage,
                               "vsMain"},
        RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, frag_stage,
                               "psMain"}};
    gui.initialize(&device, stages, {}, "gui");
  }
  {
    auto vert_stage = RIProgram::loadShaderStage(
        apResources->GetFileSearcher(), "posteffect_fullscreen.vert.spv");
    auto frag_stage = RIProgram::loadShaderStage(apResources->GetFileSearcher(),
                                                 "posteffect_present.frag.spv");
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
    if (mRendererBackend == eRendererBackend_Standard) {
      mvRenderers[eRenderer_Main] = hplNew(cStandardRenderer, (this, apResources));
      Log("Renderer backend: Standard\n");
    } else {
      mvRenderers[eRenderer_Main] = hplNew(cHybridRenderer, (this, apResources));
      Log("Renderer backend: Overdrive\n");
    }
    mvRenderers[eRenderer_WireFrame] =
        hplNew(cRendererWireFrame, (this, apResources));
    mvRenderers[eRenderer_Simple] =
        hplNew(cRendererSimple, (this, apResources));

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
  for (size_t i = 0; i < mvRenderers.size(); ++i) {
    iRenderer *pRenderer = mvRenderers[i];

    pRenderer->Update(afTimeStep);
  }
}

//-----------------------------------------------------------------------

iRenderer *cGraphics::GetRenderer(eRenderer aType) {
  if (aType >= (int)mvRenderers.size())
    return NULL;

  return mvRenderers[aType];
}

//-----------------------------------------------------------------------

eRendererBackend cGraphics::GetRendererBackend() const {
  return mRendererBackend;
}

//-----------------------------------------------------------------------

void cGraphics::ReloadRendererData() {
  for (size_t i = 0; i < mvRenderers.size(); ++i) {
    iRenderer *pRenderer = mvRenderers[i];

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
    primary.cmds[0].end(&device);
    if (device.accelerationStructureEnabled)
      blasSubmit.cmds[0].end(&device);
    IncrementFrame();
    return;
  }

  struct RIQueue *graphicsQueue = &device.queues[RI_QUEUE_GRAPHICS];
  {
    // Swapchain image: COLOR -> PRESENT for the queue present.
    RITexture swapchainTexture = {};
    swapchainTexture.vk.image = swapchain->vk.images[swapchainIndex];

    RITextureBarrier toPresent = {};
    toPresent.texture = &swapchainTexture;
    toPresent.before = RI_RESOURCE_STATE_RENDER_TARGET_READ;
    toPresent.after = RI_RESOURCE_STATE_PRESENT;
    primary.cmds[0].vk_d3d12_textureBarrier(toPresent);
  }
  primary.cmds[0].end(&device);
  if (device.accelerationStructureEnabled)
    blasSubmit.cmds[0].end(&device);

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

    VK_WrapResult(vkResetFences(device.vk.device, 1, &blasSubmit.vk.fence));
    VK_WrapResult(vkQueueSubmit2(graphicsQueue->vk.queue, 1, &blasSubmitInfo,
                                 blasSubmit.vk.fence));
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

    VK_WrapResult(vkResetFences(device.vk.device, 1, &primary.vk.fence));
    VK_WrapResult(vkQueueSubmit2(graphicsQueue->vk.queue, 1, &submitInfo,
                                 primary.vk.fence));
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
  }
  IncrementFrame();
}

void cGraphics::BeginActiveSet() {
  // See CloseAndSubmitActiveSet — the frame loop is swapchain-bound.
  assert(!swapchain.isEmpty() && swapchain->IsValid() &&
         "BeginActiveSet requires a swapchain (eHplSetup_Screen)");
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
  // Read back any GPU timing slot whose frame has finished.
  profiler.resolve(&device, completedTimeline);

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
    RITexture swapchainTexture = {};
    swapchainTexture.vk.image = swapchain->vk.images[swapchainIndex];

    RITextureBarrier toColor = {};
    toColor.texture = &swapchainTexture;
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
  memcpy((uint8_t *)scratchReq.pMappedAddress + scratchReq.bufferOffset, data,
         size);
  // Transient descriptor shim: cookie derives from the scratch buffer
  // identity folded with the sub-allocation offset/range.
  *descriptor = RIDescriptor::uniformBuffer(&device, &scratchReq.block.buffer,
                                            scratchReq.bufferOffset, size);
  RIFinishScrachReq(&device, &scratchReq);
}

std::optional<RIDescriptor>
cGraphics::resolve_filter_descriptor(eTextureWrap wrapS, eTextureWrap wrapT,
                                     eTextureWrap wrapR,
                                     eTextureFilter filter) {
#if (DEVICE_IMPL_VULKAN)
  {
    VkSamplerCreateInfo info = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.addressModeU = RI_VK_TextureWrap(wrapS);
    info.addressModeV = RI_VK_TextureWrap(wrapT);
    info.addressModeW = RI_VK_TextureWrap(wrapR);
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
      assert(false);
      break;
    }
    info.maxLod = 16;
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

    // cookie == 0 means the slot has not been created (+1 since index 0 is
    // valid).
    const hash_t samplerCookie = static_cast<hash_t>(cacheIndex) + 1;

    if (sampler.cookie == 0) {
      VK_WrapResult(vkCreateSampler(device.vk.device, &info, nullptr,
                                    &sampler.vk.sampler));
      sampler.cookie = samplerCookie;
    } else {
      // Direct indexing guarantees this slot belongs to this configuration.
      assert(sampler.cookie == samplerCookie);
    }

    return RIDescriptor::sampler(&device, &sampler);
  }
#endif
  return std::nullopt;
}

//-----------------------------------------------------------------------

} // namespace hpl
