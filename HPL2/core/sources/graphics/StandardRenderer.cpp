/*
 * Copyright © 2009-2020 Frictional Games
 *
 * This file is part of Amnesia: The Dark Descent.
 */

#include "graphics/StandardRenderer.h"

#include "graphics/DebugDraw.h"
#include "graphics/Graphics.h"
#include "graphics/RIRenderer.h"
#include "graphics/RIResourceUploader.h"
#include "graphics/RIVK.h"
#include "graphics/GlobalManagedSets.h"
#include "graphics/GBufferMRTPipelineDesc.h"
#include "graphics/StandardShadowCull.h"
#include "graphics/DecalPipelineDesc.h"
#include "graphics/Bitmap.h"
#include "graphics/Texture.h"
#include "graphics/MeshCreator.h"
#include "scene/BillBoard.h"
#include "math/BoundingVolume.h"
#include <unordered_map>
#include "graphics/GraphicsTypes.h"
#include "graphics/VertexBuffer.h"
#include "scene/Viewport.h"
#include "scene/World.h"
#include "scene/Light.h"
#include "scene/LightSpot.h"
#include "scene/LightBox.h"
#include "scene/RenderableSet.h"
#include "graphics/Renderable.h"
#include "graphics/Material.h"
#include "graphics/GraphicUtils.h"
#include "graphics/Image.h"
#include "graphics/RITypes.h"
#include "graphics/StandardEnvironmentPass.h"
#include "graphics/StandardHaloPass.h"
#include "graphics/StandardMeshDecalStreams.h"
#include "graphics/StandardDecalPass.h"
#include "graphics/StandardTranslucentPass.h"
#include "graphics/StandardAmbientOcclusionPass.h"
#include "graphics/TemporalCamera.h"
#include "graphics/TemporalReactiveMask.h"
#include "resources/Resources.h"
#include "system/LowLevelSystem.h"

#include "math/Frustum.h"

#include <new>
#include <cstring>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <functional>
#include <utility>
#include <tuple>
#include <vector>
#include <cmath>
#include <cassert>

namespace hpl {

namespace {

DecalPipelineDesc::BlendMode MeshDecalBlend(eMaterialBlendMode mode) {
  switch (mode) {
  case eMaterialBlendMode_MulX2:
    return DecalPipelineDesc::BLEND_MULX2;
  case eMaterialBlendMode_Add:
    return DecalPipelineDesc::BLEND_ADD;
  default:
    return DecalPipelineDesc::BLEND_MUL;
  }
}

// Legacy TextureCreator::GenerateScatterDiskMap2D with sorted samples: every
// size x size texel holds a jittered-grid disk warped to a circle, two
// offsets (RG, BA) per texel, stacked as samples/2 tile blocks.
SharedResourceHandle<Image> CreateStandardShadowJitter(int size, int samples) {
  const int gridSize =
      static_cast<int>(std::sqrt(static_cast<float>(samples)) + 0.5f);
  const int blocks = samples / 2;
  cBitmap bitmap;
  bitmap.CreateData(cVector3l(size, size * blocks, 1), ePixelFormat_RGBA, 0, 0);
  cBitmapData *data = bitmap.GetData(0, 0);
  if (!data || !data->mpData)
    return {};
  std::vector<cVector2f> grid(static_cast<size_t>(gridSize * gridSize));
  std::vector<cVector2f> offsets(static_cast<size_t>(samples));
  auto encode = [](float v) {
    return static_cast<unsigned char>(
        std::clamp((v + 1.0f) * 0.5f, 0.0f, 1.0f) * 255.0f);
  };
  for (int texel = 0; texel < size * size; ++texel) {
    for (int y = 0; y < gridSize; ++y)
      for (int x = 0; x < gridSize; ++x) {
        cVector2f pos(static_cast<float>(x) + 0.5f,
                      static_cast<float>(y) + 0.5f);
        pos += cMath::RandRectVector2f(cVector2f(-0.5f, -0.5f),
                                       cVector2f(0.5f, 0.5f));
        grid[static_cast<size_t>(y * gridSize + x)] =
            pos * (1.0f / static_cast<float>(gridSize));
      }
    const float sampleAdd =
        static_cast<float>(grid.size()) / static_cast<float>(samples);
    float current = 0.0f;
    for (cVector2f &offset : offsets) {
      offset = grid[static_cast<size_t>(current)];
      current += sampleAdd;
    }
    for (cVector2f &offset : offsets) {
      const cVector2f p = offset;
      offset.x = std::sqrt(p.y) * std::cos(k2Pif * p.x);
      offset.y = std::sqrt(p.y) * std::sin(k2Pif * p.x);
    }
    std::sort(offsets.begin(), offsets.end(),
              [](const cVector2f &a, const cVector2f &b) {
                return a.Length() > b.Length();
              });
    for (int block = 0; block < blocks; ++block) {
      unsigned char *px =
          data->mpData + static_cast<size_t>((block * size * size + texel) * 4);
      px[0] = encode(offsets[static_cast<size_t>(block * 2)].x);
      px[1] = encode(offsets[static_cast<size_t>(block * 2)].y);
      px[2] = encode(offsets[static_cast<size_t>(block * 2 + 1)].x);
      px[3] = encode(offsets[static_cast<size_t>(block * 2 + 1)].y);
    }
  }
  Image::SingleImage single = {};
  single.image.emplace();
  cTexture::BitmapLoadOptions opts = {};
  opts.generate_mipmaps = false;
  if (!single.image->LoadBitmap(RI_RESOURCE_STATE_SHADER_RESOURCE,
                                RI_STAGE_FRAGMENT, bitmap, opts))
    return {};
  single.image->setDebugName("Standard.shadowJitter");
  return AdoptStandaloneImage(hplNew(Image, (std::move(single))));
}

bool HasTargets(const cViewport::StandardViewportState &state,
                uint32_t imageCount) {
  if (state.width == 0 || state.height == 0 || imageCount == 0 ||
      imageCount > RI_MAX_SWAPCHAIN_IMAGES)
    return false;
  for (uint32_t i = 0; i < imageCount && i < RI_MAX_SWAPCHAIN_IMAGES; ++i) {
    if (state.aoInitialized[i])
      return true;
    if (state.renderTarget[i].isEmpty() ||
        state.renderTargetView[i].isEmpty() ||
        state.renderTargetAttachmentView[i].isEmpty() ||
        state.depthTextures[i].isEmpty() || state.depthView[i].isEmpty())
      return false;
    if (state.depthSampleView[i].isEmpty() ||
        state.positionTexture[i].isEmpty() || state.positionView[i].isEmpty() ||
        state.positionAttachmentView[i].isEmpty() ||
        state.normalTexture[i].isEmpty() || state.normalView[i].isEmpty() ||
        state.normalAttachmentView[i].isEmpty() ||
        state.shadingNormalTexture[i].isEmpty() ||
        state.shadingNormalView[i].isEmpty() ||
        state.shadingNormalAttachmentView[i].isEmpty() ||
        state.surfaceTexture[i].isEmpty() || state.surfaceView[i].isEmpty() ||
        state.surfaceAttachmentView[i].isEmpty() ||
        state.velocityTexture[i].isEmpty() || state.velocityView[i].isEmpty() ||
        state.velocityAttachmentView[i].isEmpty() ||
        state.materialColorTexture[i].isEmpty() ||
        state.materialColorView[i].isEmpty() ||
        state.materialColorAttachmentView[i].isEmpty() ||
        state.decalColorTexture[i].isEmpty() ||
        state.decalColorView[i].isEmpty() ||
        state.decalColorAttachmentView[i].isEmpty() ||
        state.decalMulTexture[i].isEmpty() || state.decalMulView[i].isEmpty() ||
        state.decalMulAttachmentView[i].isEmpty() ||
        state.decalAddTexture[i].isEmpty() || state.decalAddView[i].isEmpty() ||
        state.decalAddAttachmentView[i].isEmpty() ||
        state.environmentTexture[i].isEmpty() ||
        state.environmentView[i].isEmpty() ||
        state.environmentAttachmentView[i].isEmpty() ||
        state.translucentSceneCopy[i].isEmpty() ||
        state.translucentSceneCopyView[i].isEmpty())
      return false;
  }
  return true;
}

// Full-resolution material reconstruction writes five MRTs: diagnostic
// albedo, world position/validity, geometric view normal/validity, authored
// shading normal, and stable packed UV/material/object IDs. The hit image is
// only its input.
// writesVelocity appends cGraphics::VelocityFormat as attachment 5 for the
// fallback raster pass (Standard.fallback's SV_TARGET5); the reconstruct pass
// writes only the five material targets.
RIGraphicsPipelineDesc MakeStandardReconstructPipelineDesc(
    RI_Format_e color, RI_Format_e position, RI_Format_e normal,
    RI_Format_e shadingNormal, RI_Format_e surface, RI_Format_e depthFormat,
    bool rasterGeometry, bool writesVelocity = false) {
  RIGraphicsPipelineDesc desc = {};
  const uint32_t count = writesVelocity ? 6u : 5u;
  const RI_Format_e f[6] = {color,         position, normal,
                            shadingNormal, surface,  cGraphics::VelocityFormat};

  // No vertex input: the fullscreen/visibility VS pulls everything it needs.
  desc.topology = RI_TOPOLOGY_TRIANGLE_LIST;
  desc.raster.polygonMode = RI_POLYGON_MODE_FILL;
  desc.raster.cullMode =
      rasterGeometry ? RI_CULL_MODE_BACK : RI_CULL_MODE_NONE;
  desc.raster.frontFace = RI_FRONT_FACE_CLOCKWISE;

  desc.depthStencil.depthTest = rasterGeometry;
  desc.depthStencil.depthWrite = rasterGeometry;
  desc.depthStencil.depthCompare = RI_COMPARE_LESS_EQUAL;

  desc.blendCount = count;
  desc.renderTarget.colorCount = count;
  for (uint32_t i = 0; i < count; ++i) {
    desc.renderTarget.colorFormats[i] = f[i];
    // No blending; every target writes all four channels raw.
    desc.blend[i].blendEnable = false;
    desc.blend[i].writeMask = RI_COLOR_WRITE_RGBA;
  }
  // Depth only exists when this variant rasterizes geometry.
  desc.renderTarget.depthFormat =
      rasterGeometry ? depthFormat : RI_FORMAT_UNKNOWN;
  return desc;
}

// Single opaque full-screen target, no depth, no blend.
RIGraphicsPipelineDesc MakeStandardResolvePipelineDesc(RI_Format_e format) {
  RIGraphicsPipelineDesc desc = {};
  desc.topology = RI_TOPOLOGY_TRIANGLE_LIST;
  desc.raster.polygonMode = RI_POLYGON_MODE_FILL;
  desc.raster.cullMode = RI_CULL_MODE_NONE;
  desc.raster.frontFace = RI_FRONT_FACE_CLOCKWISE;
  desc.blendCount = 1;
  desc.blend[0].blendEnable = false;
  desc.blend[0].writeMask = RI_COLOR_WRITE_RGBA;
  desc.renderTarget.colorCount = 1;
  desc.renderTarget.colorFormats[0] = format;
  return desc;
}

bool CreateDepthSampleView(cGraphics *graphics, uint32_t index,
                           cViewport::StandardViewportState &state) {
  RITextureViewDesc desc = {};
  desc.viewType = RI_VIEWTYPE_SHADER_RESOURCE_2D;
  desc.format = cGraphics::DepthFormat;
  desc.mipNum = 1;
  desc.layerNum = 1;
  RITextureView view = RITextureView::create(
      &graphics->device, state.depthTextures[index].Get(), desc);
  if (view.isEmpty())
    return false;
  state.depthSampleView[index] =
      RISharedPointer<RITextureView>(&graphics->device, view);
  return true;
}


void DeferDepthSampleViews(cGraphics *graphics,
                           cViewport::StandardViewportState &state) {
  for (uint32_t i = 0; i < RI_MAX_SWAPCHAIN_IMAGES; ++i)
    graphics->graphicsDefer.push(state.depthSampleView[i]);
}

void MoveDepthSampleViews(cViewport::StandardViewportState &destination,
                          cViewport::StandardViewportState &source) {
  for (uint32_t i = 0; i < RI_MAX_SWAPCHAIN_IMAGES; ++i)
    destination.depthSampleView[i] = std::move(source.depthSampleView[i]);
}

// Names an image for Vulkan validation messages and captures.
// RITexture::setDebugObjectName dispatches to vkSetDebugUtilsObjectNameEXT or
// ID3D12Object::SetName, so both backends report the image by name instead of a
// bare handle.
static void NameStandardImage(cGraphics *graphics, RITexture &texture,
                              const char *name) {
  if (!graphics || !name || texture.isEmpty())
    return;
  texture.setDebugObjectName(&graphics->device, name);
}

static bool ClearStandardShadowFallback(cGraphics *graphics, RICmd *cmd,
                                        RITexture *texture) {
  if (!graphics || !cmd || !texture)
    return false;
  RITextureBarrier toDepth(texture, RI_RESOURCE_STATE_UNDEFINED,
                           RI_RESOURCE_STATE_DEPTH_WRITE, RI_STAGE_NONE,
                           RI_STAGE_FRAGMENT, RI_BARRIER_ASPECT_DEPTH);
  toDepth.mipCount = 1;
  toDepth.layerCount = 1;
  cmd->vk_d3d12_textureBarrier(toDepth);
  RITextureViewDesc vd{};
  vd.viewType = RI_VIEWTYPE_DEPTH_STENCIL_ATTACHMENT;
  vd.format = cGraphics::DepthFormat;
  vd.mipNum = 1;
  vd.layerNum = 1;
  RITextureView attachment =
      RITextureView::create(&graphics->device, texture, vd);
  if (attachment.isEmpty())
    return false;
  RIRenderingAttachment depth{};
  depth.view = attachment;
  depth.loadOp = RI_ATTACHMENT_LOAD_OP_CLEAR;
  depth.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
  depth.clearValue.depth = 1.0f;
  RIBeginRenderingDesc begin{};
  begin.renderArea.width = 1;
  begin.renderArea.height = 1;
  begin.depthStencil = &depth;
  cmd->vk_d3d12_beginRendering(&graphics->device, begin);
  cmd->vk_d3d12_endRendering(&graphics->device);
  RITextureBarrier toSample(
      texture, RI_RESOURCE_STATE_DEPTH_WRITE, RI_RESOURCE_STATE_SHADER_RESOURCE,
      RI_STAGE_FRAGMENT, RI_STAGE_FRAGMENT, RI_BARRIER_ASPECT_DEPTH);
  toSample.mipCount = 1;
  toSample.layerCount = 1;
  cmd->vk_d3d12_textureBarrier(toSample);
  graphics->graphicsDefer.push(
      RISharedPointer<RITextureView>(&graphics->device, attachment));
  return true;
}

static uint32_t StandardTextureSlot(Image *image) {
  if (!image)
    return kStandardInvalidTexture;
  Interface<cGraphics>::Get()->graphicsDefer.push(PinResource(image));
  return image->GetBindlessSlot();
}

static bool StandardFinite(float value) { return std::isfinite(value); }

static uint32_t StandardShadowResolution(eShadowMapResolution quality) {
  switch (quality) {
  case eShadowMapResolution_Low:
    return 256;
  case eShadowMapResolution_Medium:
    return 512;
  default:
    return 1024;
  }
}

template <class T>
static bool
UploadStandardLights(cGraphics *graphics, const std::vector<T> &items,
                     RISharedPointer<RIBuffer> &buffer, size_t &capacity) {
  const size_t bytes = sizeof(T) * std::max<size_t>(items.size(), 1);
  if (capacity < bytes) {
    capacity = std::max<size_t>(bytes, capacity ? capacity * 2 : sizeof(T));
    auto next = RISharedPointer<RIBuffer>(
        &graphics->device,
        RIBuffer::create(&graphics->device,
                         {(uint64_t)capacity,
                          RI_BUFFER_USAGE_SHADER_RESOURCE_STORAGE |
                              RI_BUFFER_USAGE_TRANSFER_DST,
                          RI_MEMORY_DEVICE, 0}));
    if (next.isEmpty())
      return false;
    // The old allocation may still be referenced by an earlier frame.
    graphics->graphicsDefer.push(buffer);
    buffer = std::move(next);
  }
  if (buffer.isEmpty())
    return false;
  if (items.empty()) {
    graphics->graphicsDefer.push(buffer);
    return true;
  }
  RIResourceBufferTransaction transaction = {};
  transaction.target = *buffer;
  transaction.size = sizeof(T) * items.size();
  // These per-Draw buffers are freshly allocated.  Do not claim that they
  // already contain shader-visible data: the uploader must own the initial
  // transition from UNDEFINED.
  transaction.currentState = RI_RESOURCE_STATE_UNDEFINED;
  transaction.currentStages = RI_STAGE_NONE;
  transaction.postState = RI_RESOURCE_STATE_SHADER_RESOURCE;
  transaction.postStages = RI_STAGE_FRAGMENT;
  RI_ResourceBeginCopyBuffer(&graphics->device, &graphics->uploader,
                             &transaction);
  std::memcpy(transaction.mapped.data, items.data(), transaction.size);
  RI_ResourceEndCopyBuffer(&graphics->device, &graphics->uploader,
                           &transaction);
  // Pin at the allocation/upload boundary, before any caller can take an
  // early return (including a later light-buffer allocation failure).
  graphics->graphicsDefer.push(buffer);
  return true;
}

// Legacy hpl::GetShadowMapResolution: clamp the authored quality to the cap.
static eShadowMapResolution
StandardCapShadowQuality(eShadowMapResolution wanted,
                         eShadowMapResolution cap) {
  if (cap == eShadowMapResolution_High)
    return wanted;
  if (cap == eShadowMapResolution_Medium)
    return wanted == eShadowMapResolution_High ? eShadowMapResolution_Medium
                                               : wanted;
  return eShadowMapResolution_Low;
}

// Build the per-object record the global set stores.
//
// Shared by the camera pass and the shadow gather so both produce a byte-
// identical payload for the same renderable: submitObject skips its upload when
// the record matches what the slot already holds, so the second caller costs
// only the slot lookup, and a caster visible to both occupies ONE slot rather
// than one per light.
//
// apFrustum must never be null. cRopeEntity inverts the usual convention --
// GetModelMatrix(nullptr) returns the world matrix while any non-null frustum
// returns NULL (rope vertices are already world space) -- so passing nullptr
// here would double-transform every rope.
static ObjectSubmitDesc BuildStandardObjectDesc(iRenderable *apObject,
                                                cMaterial *apMaterial,
                                                cFrustum *apFrustum,
                                                uint32_t alMaterialId) {
  ObjectSubmitDesc object;
  object.modelMatrix = apObject->GetModelMatrix(apFrustum);
  object.uvMatrix = apMaterial->GetUvMatrix();
  object.materialId = alMaterialId;
  object.dissolveAmount = apObject->GetCoverageAmount();
  object.illuminationAmount = apObject->GetIlluminationAmount();
  object.renderFlags = apObject->GetRenderFlags();
  if (apObject->IsStatic())
    object.renderFlags |= kStandardCullStaticBit;
  object.decalList =
      (static_cast<uint32_t>(apObject->GetDecalListOffset()) << 8) |
      (static_cast<uint32_t>(apObject->GetDecalListCount()) & 0xffu);
  return object;
}

// Legacy cRendererDeferred shadow distance LOD (RendererDeferred.h).
static constexpr float kStandardShadowDistanceMedium = 10.0f;
static constexpr float kStandardShadowDistanceLow = 20.0f;
static constexpr float kStandardShadowDistanceNone = 40.0f;

// Shadow atlas pages are kStandardShadowAtlasCapTiles cap-resolution tiles
// wide (4096 at a High cap). Lights that would need tiles below the minimum
// lose their shadow instead of blurring further.
static constexpr uint32_t kStandardShadowAtlasCapTiles = 4;
static constexpr uint32_t kStandardShadowMaxAtlases = 4;
static constexpr uint32_t kStandardShadowMinTile = 64;
// Cube-face frusta are widened by this many texels per side so the bilinear
// and jitter kernel of a receiver near a face edge still reads rendered depth.
static constexpr float kStandardShadowCubeBorderTexels = 2.0f;

// Emitter radius, in world units, used by the PCSS penumbra estimate.
//
// Amnesia authors no emitter size -- a light has a reach radius, which is a
// different thing entirely and would make a room light absurdly soft. This is a
// single tunable standing in for "about the size of a lamp flame or bulb".
// Larger means softer everywhere; 0 restores the old one-texel hard edge.
static constexpr float kStandardShadowLightSize = 0.12f;


// GPU shadow cull limits.
//
// kStandardShadowMaxCandidatesPerLight replaces the old
// `casters.size() > kObjectSlotCapacity` guard, which never fired in practice:
// kObjectSlotCapacity is 32768, so it was a slot-pool assertion rather than a
// per-light bound. This one is a real bound, and it is also the worst-case
// indirect reservation a single light can take out.
static constexpr uint32_t kStandardShadowMaxTiles = 1024;
static constexpr uint32_t kStandardShadowMaxCandidatesPerLight = 2048;
// Total candidate records across every light in one Draw. A caster near K
// lights appears K times; K is 1-2 in practice.
static constexpr uint32_t kStandardShadowMaxCandidates = 16384;
// StandardCull.h has to mirror these by value: it is included from Slang,
// which cannot see the C++-only headers that define them. Assert the mirrors
// here, where both are visible, so a renumber cannot silently desync the
// kernel's flag tests from the engine's.
static_assert(kStandardCullShadowCasterBit == eRenderableFlag_ShadowCaster,
              "StandardCull.h shadow-caster mirror is stale");
static_assert(kStandardCullVariabilityStatic == eObjectVariabilityFlag_Static,
              "StandardCull.h static-variability mirror is stale");
static_assert(kStandardCullVariabilityDynamic == eObjectVariabilityFlag_Dynamic,
              "StandardCull.h dynamic-variability mirror is stale");

// Camera records live one per occlusion-testing tile per frame; only the camera
// pass allocates them, so a small ring is plenty.
static constexpr uint32_t kStandardCullMaxCameras = 64;

// Draws the translucent occlusion cull can cover in one frame. Each one costs a
// 20-byte command slot and a 48-byte candidate; past this the pass falls back
// to direct draws rather than culling part of a sorted list.
static constexpr uint32_t kStandardTranslucentMaxDraws = 4096;

// Opaque draws the camera cull can cover in one frame.
static constexpr uint32_t kStandardCameraMaxDraws = 16384;
// Persistent visibility slots. A renderable maps to one by hashing its cookie,
// so two can collide: the loser is either drawn in phase 1 while hidden (depth
// rejects it) or skipped there and picked up by phase 2. Costs efficiency,
// never correctness, which is what lets this be a plain lossy table.
static constexpr uint32_t kStandardCullVisibilityKeys = 65536;

static constexpr uint32_t kStandardShadowMaxCullGroups =
    kStandardShadowMaxCandidates / kStandardCullGroupSize + kStandardShadowMaxTiles;

// The limits above are what ONE Draw may stage. Their backing store is an
// RISegmentAlloc ring, which only reclaims a segment once the frame that took
// it is RI_NUMBER_FRAMES_FLIGHT old -- so every frame still in flight holds its
// slice at the same time. These rings were sized to a single frame's limit,
// which left no room for the frames already in flight, so the host-side guards
// never fired: the allocator ran out first.
//
// What that cost depended on the allocator. It used to read a full ring as an
// empty one and hand the next frame a range the in-flight frame was still
// reading, so a heavy view staged cull tiles and candidates over the data the
// GPU had not finished with. It now correctly refuses instead -- but a refusal
// is also silent here: a light whose count/indirect request fails is dropped
// (lightFailed), and a failed tile/group publish clears `prepared` outright,
// dropping EVERY shadow for that frame. Either way the symptom is the same,
// and it is load-dependent: the staged tile count tracks how many cube faces
// face the camera, so a room full of shadowed point lights -- a row of
// candles, six faces each -- sits at the edge and crosses it as the view
// turns, and the shadows flash.
//
// Sizing the rings for all frames in flight, plus one frame of slack for the
// tail the allocator forfeits when a contiguous request does not fit before
// the end of the buffer, makes the per-frame guards above the real limit again.
static constexpr uint32_t kStandardShadowRingFrames = RI_NUMBER_FRAMES_FLIGHT + 1;
static constexpr uint32_t kStandardShadowCandidateRing =
    kStandardShadowMaxCandidates * kStandardShadowRingFrames;
static constexpr uint32_t kStandardShadowTileRing =
    kStandardShadowMaxTiles * kStandardShadowRingFrames;
static constexpr uint32_t kStandardShadowCullGroupRing =
    kStandardShadowMaxCullGroups * kStandardShadowRingFrames;
static constexpr uint32_t kStandardShadowIndirectRing =
    kObjectSlotCapacity * kStandardShadowRingFrames;

static float StandardPointShadowNear(float radius) {
  return std::max(0.05f, radius * 0.01f);
}

struct StandardShadowLightTiles {
  const iLight *light;
  uint32_t firstTile;
  uint32_t size;
};

static const StandardShadowLightTiles *
FindStandardShadowTiles(const std::vector<StandardShadowLightTiles> &tiles,
                        const iLight *light) {
  for (const StandardShadowLightTiles &entry : tiles)
    if (entry.light == light)
      return &entry;
  return nullptr;
}

// View matrix of cube face `face` (+X,-X,+Y,-Y,+Z,-Z) at `position`. HPL views
// look down -Z of their world matrix (cCamera::GetForward), so the face
// direction is the negated Z column. The basis stays right-handed so the
// shadow pipeline's winding is unchanged.
static cMatrixf StandardCubeFaceView(const cVector3f &position, uint32_t face) {
  static const cVector3f kForward[kStandardShadowCubeFaces] = {
      cVector3f(1, 0, 0),  cVector3f(-1, 0, 0), cVector3f(0, 1, 0),
      cVector3f(0, -1, 0), cVector3f(0, 0, 1),  cVector3f(0, 0, -1)};
  static const cVector3f kUp[kStandardShadowCubeFaces] = {
      cVector3f(0, 1, 0), cVector3f(0, 1, 0), cVector3f(0, 0, -1),
      cVector3f(0, 0, 1), cVector3f(0, 1, 0), cVector3f(0, 1, 0)};
  const cVector3f back = kForward[face] * -1.0f;
  const cVector3f up = kUp[face];
  const cVector3f right = cMath::Vector3Cross(up, back);
  cMatrixf world = cMatrixf::Identity;
  world.m[0][0] = right.x;
  world.m[1][0] = right.y;
  world.m[2][0] = right.z;
  world.m[0][1] = up.x;
  world.m[1][1] = up.y;
  world.m[2][1] = up.z;
  world.m[0][2] = back.x;
  world.m[1][2] = back.y;
  world.m[2][2] = back.z;
  world.SetTranslation(position);
  return cMath::MatrixInverse(world);
}

// Light records are partitioned, not filtered: every enabled light still
// reaches the buffer, but the ones whose volume touches `frustum` are written
// first and reported as `pointCount` / `spotCount`. The resolve, translucent
// and particle passes loop only over that prefix -- a light whose volume misses
// the camera frustum cannot light anything inside it. The water reflection
// pass renders a view mirrored through the water plane, so its lights are not
// a subset of the camera's; it loops over `pointCountTotal` / `spotCountTotal`
// instead. Disabled lights are dropped outright (they previously occupied a
// slot with radius 0, which the shader rejected per pixel anyway).
static bool BuildStandardLights(
    cWorld *world, cGraphics *graphics, cFrustum *frustum,
    RISharedPointer<RIBuffer> &pointBuffer,
    RISharedPointer<RIBuffer> &spotBuffer, RISharedPointer<RIBuffer> &boxBuffer,
    uint32_t &pointCount, uint32_t &spotCount, uint32_t &boxCount,
    uint32_t &pointCountTotal, uint32_t &spotCountTotal,
    uint32_t &shadowCount, eShadowMapResolution shadowResolutionCap,
    bool shadowsAvailable,
    const std::vector<StandardShadowLightTiles> &shadowTiles) {
  // Keep the resources referenced by the previous recording alive through the
  // GPU completion point. This also makes a same-size rewrite safe when the
  // uploader uses a deferred transfer queue.
  std::vector<StandardPointLightData> points;
  std::vector<StandardSpotLightData> spots;
  // Enabled but outside the camera frustum: appended after the visible ones.
  std::vector<StandardPointLightData> pointsOffscreen;
  std::vector<StandardSpotLightData> spotsOffscreen;
  std::vector<std::pair<StandardBoxLightData, cLightBoxLegacy *>>
      boxesWithLights;
  shadowCount = 0;
  // Cull on the bounding volume, never the origin: a large-radius light whose
  // centre sits behind the camera can still light the view.
  const auto touchesFrustum = [&](iLight *light) {
    return !frustum || frustum->CollideBoundingVolume(
                           light->GetBoundingVolume()) != eCollision_Outside;
  };
  if (world) {
    for (iLight *light : *world->GetLightList()) {
      if (!light)
        continue;
      const float radius = light->GetRadius();
      // A light carries both tunings and resolves the Standard one here; the
      // renderer mask, not the class, decides whether it contributes. Area
      // lights have no Standard tuning at all.
      const bool enabled = light->GetLightType() != eLightType_Area &&
                           light->GetVisibleVar() &&
                           light->IsLegacyRendererEnabled() &&
                           StandardFinite(radius) && radius > 0.0f;
      const cColor diffuse = light->GetDiffuseColor();
      if (light->GetLightType() == eLightType_Point) {
        if (!enabled)
          continue;
        StandardPointLightData data{};
        const cVector3f p = light->GetWorldPosition();
        data.position[0] = p.x;
        data.position[1] = p.y;
        data.position[2] = p.z;
        data.radius = enabled ? radius : 0.0f;
        data.color[0] = diffuse.r;
        data.color[1] = diffuse.g;
        data.color[2] = diffuse.b;
        data.specularScale = diffuse.a;
        // Legacy ABI name: this field carries the complete authored lightWorld
        // matrix. The deferred reference uploads all four rows, including its
        // translation, for the homogeneous point-gobo lookup.
        const ml::float4x4 lightWorld =
            cMath::ToFloatTranspose4x4(light->GetWorldMatrix());
        std::memcpy(data.invViewRotation, lightWorld.a,
                    sizeof(data.invViewRotation));
        data.falloffTexture = StandardTextureSlot(light->GetFalloffImage());
        data.goboTexture = StandardTextureSlot(light->GetGoboImage());
        // First of the six cube-face tiles rendered for this light this Draw.
        data.shadowIndex = kStandardInvalidShadow;
        const StandardShadowLightTiles *pointTiles =
            FindStandardShadowTiles(shadowTiles, light);
        if (shadowsAvailable && enabled && light->GetCastShadows() &&
            light->GetShadowCastersAffected() != 0 && pointTiles) {
          data.shadowIndex = pointTiles->firstTile;
          ++shadowCount;
        }
        data.config =
            (enabled ? kStandardLightEnabled : 0u) |
            (data.goboTexture != kStandardInvalidTexture ? kStandardLightHasGobo
                                                         : 0u) |
            (data.shadowIndex != kStandardInvalidShadow
                 ? kStandardLightHasShadow
                 : 0u);
        (touchesFrustum(light) ? points : pointsOffscreen).push_back(data);
      } else if (light->GetLightType() == eLightType_Spot) {
        if (!enabled)
          continue;
        iLightSpot *spot = static_cast<iLightSpot *>(light);
        StandardSpotLightData data{};
        const cVector3f p = spot->GetWorldPosition();
        const cMatrixf &worldMatrix = spot->GetWorldMatrix();
        data.position[0] = p.x;
        data.position[1] = p.y;
        data.position[2] = p.z;
        const float fov = spot->GetFOV();
        const float aspect = spot->GetAspect();
        const float nearClip = spot->GetNearClipPlane();
        const bool validProjection =
            StandardFinite(radius) && radius > nearClip &&
            StandardFinite(fov) && fov > 0.0f &&
            fov < 3.14159265358979323846f && StandardFinite(aspect) &&
            aspect > 0.0f && StandardFinite(nearClip) && nearClip > 0.0f;
        const bool spotEnabled = enabled && validProjection;
        data.radius = spotEnabled ? radius : 0.0f;
        data.direction[0] = worldMatrix.m[0][2];
        data.direction[1] = worldMatrix.m[1][2];
        data.direction[2] = worldMatrix.m[2][2];
        data.oneMinusCosHalfFov =
            validProjection ? 1.0f - std::cos(fov * 0.5f) : 0.0f;
        data.color[0] = diffuse.r;
        data.color[1] = diffuse.g;
        data.color[2] = diffuse.b;
        data.specularScale = diffuse.a;
        // The Standard light contract uses the authored legacy radius.  Do
        // not call GetViewProjMatrix here: its projection reach is selected
        // by the light evaluation mode and can therefore disagree with the
        // radial light volume above.
        if (validProjection) {
          const cMatrixf projection = cMath::MatrixPerspectiveProjection(
              nearClip, radius, fov, aspect, false);
          const cMatrixf viewProjection =
              cMath::MatrixMul(projection, spot->GetViewMatrix());
          const ml::float4x4 vp = cMath::ToFloatTranspose4x4(viewProjection);
          std::memcpy(data.spotViewProjection, vp.a,
                      sizeof(data.spotViewProjection));
        }
        data.radialFalloffTexture =
            StandardTextureSlot(light->GetFalloffImage());
        data.coneFalloffTexture =
            StandardTextureSlot(spot->GetSpotFalloffImage());
        data.goboTexture = StandardTextureSlot(light->GetGoboImage());
        data.shadowIndex = kStandardInvalidShadow;
        const float authoredBias = spot->GetShadowMapBiasMul();
        data.shadowBias = (StandardFinite(authoredBias) && authoredBias >= 0.0f)
                              ? 0.0005f * authoredBias
                              : 0.0f;
        const uint32_t authoredResolution =
            StandardShadowResolution(spot->GetShadowMapResolution());
        const uint32_t resolutionCap =
            StandardShadowResolution(shadowResolutionCap);
        data.shadowResolution = std::min(authoredResolution, resolutionCap);
        // The light's atlas tile, if one was packed and rendered this Draw.
        const StandardShadowLightTiles *spotTiles =
            FindStandardShadowTiles(shadowTiles, spot);
        if (shadowsAvailable && spotEnabled && spot->GetCastShadows() &&
            spot->GetShadowCastersAffected() != 0 && spotTiles) {
          data.shadowIndex = spotTiles->firstTile;
          data.shadowResolution = std::min(spotTiles->size, resolutionCap);
          ++shadowCount;
        }
        data.config =
            (spotEnabled ? kStandardLightEnabled : 0u) |
            (data.goboTexture != kStandardInvalidTexture ? kStandardLightHasGobo
                                                         : 0u) |
            (data.shadowIndex != kStandardInvalidShadow
                 ? kStandardLightHasShadow
                 : 0u);
        if (!spotEnabled)
          continue;
        (touchesFrustum(light) ? spots : spotsOffscreen).push_back(data);
      } else if (light->GetLightType() == eLightType_Box) {
        cLightBoxLegacy *box = static_cast<cLightBoxLegacy *>(light);
        const cVector3f size = box->GetSize();
        const bool boxEnabled =
            light->GetVisibleVar() && light->IsLegacyRendererEnabled() &&
            StandardFinite(size.x) && size.x > 0.0f && StandardFinite(size.y) &&
            size.y > 0.0f && StandardFinite(size.z) && size.z > 0.0f;
        if (!boxEnabled)
          continue;
        StandardBoxLightData data{};
        const cVector3f p = light->GetWorldPosition();
        const cVector3f halfSize = size * 0.5f;
        const cVector3f boxMin = p - halfSize;
        const cVector3f boxMax = p + halfSize;
        data.boxMin[0] = boxMin.x;
        data.boxMin[1] = boxMin.y;
        data.boxMin[2] = boxMin.z;
        data.boxMax[0] = boxMax.x;
        data.boxMax[1] = boxMax.y;
        data.boxMax[2] = boxMax.z;
        data.color[0] = diffuse.r;
        data.color[1] = diffuse.g;
        data.color[2] = diffuse.b;
        data.blendFunc = static_cast<uint32_t>(box->GetBlendFunc());
        data.config = kStandardLightEnabled;
        boxesWithLights.push_back({data, box});
      }
    }
  }
  // Stable-sort box lights by priority, then by pointer for determinism. Cap at 256 lights.
  std::stable_sort(
      boxesWithLights.begin(), boxesWithLights.end(),
      [](const std::pair<StandardBoxLightData, cLightBoxLegacy *> &a,
         const std::pair<StandardBoxLightData, cLightBoxLegacy *> &b) {
        const int aPrio = a.second->GetBoxLightPrio();
        const int bPrio = b.second->GetBoxLightPrio();
        if (aPrio != bPrio)
          return aPrio < bPrio;
        return a.second < b.second;
      });
  std::vector<StandardBoxLightData> boxes;
  for (const auto &pair : boxesWithLights) {
    if (boxes.size() >= 256)
      break;
    boxes.push_back(pair.first);
  }
  // The visible prefix is what the camera-view passes loop over; the offscreen
  // remainder follows it so the mirrored reflection view can still reach it.
  pointCount = static_cast<uint32_t>(points.size());
  spotCount = static_cast<uint32_t>(spots.size());
  points.insert(points.end(), pointsOffscreen.begin(), pointsOffscreen.end());
  spots.insert(spots.end(), spotsOffscreen.begin(), spotsOffscreen.end());
  pointCountTotal = static_cast<uint32_t>(points.size());
  spotCountTotal = static_cast<uint32_t>(spots.size());
  boxCount = static_cast<uint32_t>(boxes.size());
  size_t pointCapacity = 0, spotCapacity = 0, boxCapacity = 0;
  const bool pointUpload =
      UploadStandardLights(graphics, points, pointBuffer, pointCapacity);
  const bool spotUpload =
      UploadStandardLights(graphics, spots, spotBuffer, spotCapacity);
  const bool boxUpload =
      UploadStandardLights(graphics, boxes, boxBuffer, boxCapacity);
  if (!pointUpload || !spotUpload || !boxUpload) {
    pointCount = spotCount = boxCount = 0;
    pointCountTotal = spotCountTotal = 0;
    return false;
  }
  return true;
}

} // namespace

cStandardRenderer::cStandardRenderer(cGraphics *apGraphics,
                                     cResources *apResources)
    : iRenderer("Standard", apGraphics, apResources), m_visibility(nullptr),
      m_fallback(nullptr), m_reconstruct(nullptr), m_lighting(nullptr) {
  m_environment =
      std::make_unique<cStandardEnvironmentPass>(mpGraphics, apResources);
  m_particles =
      std::make_unique<cStandardParticlePass>(mpGraphics, apResources);
  m_decals = std::make_unique<cStandardDecalPass>(mpGraphics, apResources);
  m_shadow = std::make_unique<cStandardShadowPass>(mpGraphics, apResources);
  m_shadowCull =
      std::make_unique<cStandardShadowCullPass>(mpGraphics, apResources);
  m_hiZ = std::make_unique<cStandardHiZPass>(mpGraphics, apResources);
  m_halo = std::make_unique<cStandardHaloPass>(mpGraphics, apResources);
  m_translucent =
      std::make_unique<cStandardTranslucentPass>(mpGraphics, apResources);
  m_water = std::make_unique<cStandardWaterPass>(mpGraphics, apResources);
  m_ambientOcclusion =
      std::make_unique<cStandardAmbientOcclusionPass>(mpGraphics, apResources);
  m_forceFallback = std::getenv("HPL_STANDARD_FORCE_FALLBACK") != nullptr;
  RISegmentAllocDesc desc = {};
  desc.numSegments = RI_NUMBER_FRAMES_FLIGHT;
  desc.elementStride = sizeof(VkDrawIndirectCommand);
  desc.maxElements = kObjectSlotCapacity;
  m_indirectSegment = RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS>(&desc);
  // Host-built: the two-phase camera cull only rewrites each command's
  // instanceCount word.
  m_indirectDrawBuffer.Create(&mpGraphics->device, kObjectSlotCapacity,
                              sizeof(VkDrawIndirectCommand),
                              /*hostWritten*/ true,
                              "StandardRenderer.indirectDraw");
  m_indirectDrawFirstUse = true;
  RISegmentAllocDesc shadowDesc = {};
  shadowDesc.numSegments = RI_NUMBER_FRAMES_FLIGHT;
  shadowDesc.elementStride = sizeof(VkDrawIndirectCommand);
  shadowDesc.maxElements = kStandardShadowIndirectRing;
  m_shadowIndirectSegment =
      RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS>(&shadowDesc);
  // Kernel-authored: both shadow cull modes write whole commands, so the host
  // never maps this one.
  m_shadowIndirectBuffer.Create(&mpGraphics->device, kStandardShadowIndirectRing,
                                sizeof(VkDrawIndirectCommand),
                                /*hostWritten*/ false,
                                "StandardRenderer.shadowIndirect");

  CreateCullBuffers();
  // Like cRendererSimple: cGraphics::Init no longer loads renderers, and Draw
  // returns immediately until the fallback and lighting programs exist.
  if (!LoadData())
    Error("Standard renderer failed to load its shaders; nothing will render. "
          "See log.\n");
}

// GPU cull buffers. Created here rather than in LoadData's body so both the
// constructor and a shader hot-reload land on the same code, and idempotent so
// LoadData can call it after DestroyData has handed the old ones back.
//
// The inputs stay host-mapped: the host writes each one linearly once per Draw
// and the GPU only reads them, so a staged device-local copy would add an
// upload for no benefit.
void cStandardRenderer::CreateCullBuffers() {
  // Buffer classification matters here, because D3D12 upload heaps cannot carry
  // UAV flags (RID3D12Buffer.cpp rejects HOST_UPLOAD + SHADER_RESOURCE_STORAGE
  // outright). The cull kernel splits its set cleanly:
  //
  //   read-only  (StructuredBuffer,   t#) -- candidates, tiles, groups, cameras
  //   read/write (RWStructuredBuffer, u#) -- indirect commands, counts, visibility
  //
  // The read-only ones stay host-mapped and ask only for SHADER_RESOURCE; both
  // that and SHADER_RESOURCE_STORAGE map to VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
  // on Vulkan (RIVK.h), so this is a no-op there, and RIProgram picks SRV vs UAV
  // from the reflected register class rather than the descriptor type, so the
  // binding side is unchanged on both backends.
  //
  // The read/write ones must be device-local; they are seeded through the
  // uploader / staged copies instead of direct host writes.
  const auto makeCullBuffer = [&](RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS> *segment,
                                  struct RIBuffer *buffer, uint32_t elements,
                                  uint32_t stride, uint32_t usage,
                                  bool deviceLocal, const char *debugName) {
    if (!buffer->isEmpty())
      return;
    RISegmentAllocDesc segmentDesc = {};
    segmentDesc.numSegments = RI_NUMBER_FRAMES_FLIGHT;
    segmentDesc.elementStride = stride;
    segmentDesc.maxElements = elements;
    *segment = RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS>(&segmentDesc);
    *buffer = detail::CreateBindlessSlotBuffer(
        &mpGraphics->device, elements, stride, usage, deviceLocal, debugName);
  };
  // Ring sizes, not per-Draw limits: see kStandardShadowRingFrames.
  makeCullBuffer(&m_shadowCandidateSegment, &m_shadowCandidateBuffer,
                 kStandardShadowCandidateRing, sizeof(StandardCullCandidate),
                 RI_BUFFER_USAGE_SHADER_RESOURCE, false,
                 "StandardRenderer.shadowCullCandidates");
  makeCullBuffer(&m_shadowCullTileSegment, &m_shadowCullTileBuffer,
                 kStandardShadowTileRing, sizeof(StandardCullTile),
                 RI_BUFFER_USAGE_SHADER_RESOURCE, false,
                 "StandardRenderer.shadowCullTiles");
  makeCullBuffer(&m_shadowCullGroupSegment, &m_shadowCullGroupBuffer,
                 kStandardShadowCullGroupRing, sizeof(StandardCullGroup),
                 RI_BUFFER_USAGE_SHADER_RESOURCE, false,
                 "StandardRenderer.shadowCullGroups");
  makeCullBuffer(&m_cullCameraSegment, &m_cullCameraBuffer,
                 kStandardCullMaxCameras, sizeof(StandardCullCamera),
                 RI_BUFFER_USAGE_SHADER_RESOURCE, false,
                 "StandardRenderer.cullCameras");
  // Written by the host and rewritten by the kernel, so it takes the staged
  // path rather than makeCullBuffer's single host-mapped allocation.
  {
    RISegmentAllocDesc segmentDesc = {};
    segmentDesc.numSegments = RI_NUMBER_FRAMES_FLIGHT;
    segmentDesc.elementStride = sizeof(VkDrawIndexedIndirectCommand);
    segmentDesc.maxElements = kStandardTranslucentMaxDraws;
    m_translucentCommandSegment =
        RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS>(&segmentDesc);
    // Host-built: instance-mask mode rewrites one word of a command the host
    // already sorted and wrote.
    m_translucentCommandBuffer.Create(
        &mpGraphics->device, kStandardTranslucentMaxDraws,
        sizeof(VkDrawIndexedIndirectCommand), /*hostWritten*/ true,
        "StandardRenderer.translucentCommands");
    m_translucentCommandFirstUse = true;
  }
  makeCullBuffer(&m_translucentCandidateSegment, &m_translucentCandidateBuffer,
                 kStandardTranslucentMaxDraws, sizeof(StandardCullCandidate),
                 RI_BUFFER_USAGE_SHADER_RESOURCE, false,
                 "StandardRenderer.translucentCandidates");
  makeCullBuffer(&m_cameraCandidateSegment, &m_cameraCandidateBuffer,
                 kStandardCameraMaxDraws, sizeof(StandardCullCandidate),
                 RI_BUFFER_USAGE_SHADER_RESOURCE, false,
                 "StandardRenderer.cameraCullCandidates");
  // Device-local: the counts are written by compute and consumed by
  // vkCmdDrawIndirectCount without ever being read back on the host.
  makeCullBuffer(&m_shadowDrawCountSegment, &m_shadowDrawCountBuffer,
                 kStandardShadowTileRing, sizeof(uint32_t),
                 RI_BUFFER_USAGE_INDIRECT |
                     RI_BUFFER_USAGE_SHADER_RESOURCE_STORAGE,
                 true,
                 "StandardRenderer.shadowDrawCounts");
  // Persistent across frames, so no segment allocator. The kernel both reads
  // and writes it (gCullVisibility is an RWStructuredBuffer), so it has to be
  // device-local -- a host-mapped UAV is exactly what D3D12 upload heaps
  // cannot express. Every entry must start "not visible", which makes the
  // first frame after a (re)create draw everything in phase 2 and nothing in
  // phase 1; the RI layer has no fillBuffer, so the seed is staged through the
  // uploader the way cGpuParticleSystem::ClearSlice does.
  if (m_cullVisibilityBuffer.isEmpty()) {
    m_cullVisibilityBuffer = detail::CreateBindlessSlotBuffer(
        &mpGraphics->device, kStandardCullVisibilityKeys, sizeof(uint32_t),
        RI_BUFFER_USAGE_SHADER_RESOURCE_STORAGE |
            RI_BUFFER_USAGE_TRANSFER_DST,
        /*deviceLocalOnly*/ true, "StandardRenderer.cullVisibility");
    ZeroCullVisibility();
  }
}

// Whether this device can express the Hi-Z build's combined read+write state.
// Vulkan always can (VK_IMAGE_LAYOUT_GENERAL); D3D12 needs the enhanced-barrier
// path, which pins a simultaneous-access texture's layout to COMMON.
static bool HiZBarriersUsable(const RIDevice &device) {
#if (DEVICE_IMPL_D3D12)
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12))
    return device.physicalAdapter.isEnchancedBarrierSupported != 0;
#endif
  (void)device;
  return true;
}

bool StagedIndirectBuffer::isEmpty() const {
  if (staged)
    return host.isEmpty() || host.mappedAddress == nullptr || device.isEmpty();
  return host.isEmpty() && device.isEmpty();
}

bool StagedIndirectBuffer::Create(RIDevice *dev, uint64_t elements,
                                  size_t stride, bool hostWritten,
                                  const char *debugName) {
  if (!isEmpty())
    return true;
  const uint32_t commandUsage = RI_BUFFER_USAGE_INDIRECT |
                                RI_BUFFER_USAGE_SHADER_RESOURCE_STORAGE |
                                RI_BUFFER_USAGE_TRANSFER_DST;

  if (!hostWritten) {
    // Kernel-authored commands: nothing maps it, so one device-local buffer
    // serves both backends.
    device = detail::CreateBindlessSlotBuffer(dev, elements, stride,
                                              commandUsage,
                                              /*deviceLocalOnly*/ true,
                                              debugName);
    staged = false;
    return !isEmpty();
  }

  // Only D3D12 needs the split; Vulkan keeps the single host-mapped buffer the
  // renderer has always used, so the shipping backend is untouched.
  staged = false;
#if (DEVICE_IMPL_D3D12)
  staged = RIIsTargetSelected(RI_DEVICE_API_D3D12);
#endif
  // When staged the host side is pure staging: transfer source only, which an
  // upload heap will accept alongside a mapping.
  host = detail::CreateBindlessSlotBuffer(
      dev, elements, stride,
      staged ? RI_BUFFER_USAGE_TRANSFER_SRC : commandUsage,
      /*deviceLocalOnly*/ false, debugName);
  if (host.isEmpty())
    return false;
  if (staged)
    device = detail::CreateBindlessSlotBuffer(dev, elements, stride,
                                              commandUsage,
                                              /*deviceLocalOnly*/ true,
                                              debugName);
  return !isEmpty();
}

void StagedIndirectBuffer::Defer(cGraphics *graphics) {
  const auto retire = [&](RIBuffer *buffer) {
    if (buffer->isEmpty())
      return;
    graphics->graphicsDefer.push(std::function<void()>(
        [owned = std::move(*buffer), dev = &graphics->device]() mutable {
          owned.dispose(dev);
        }));
    *buffer = {};
  };
  retire(&host);
  retire(&device);
  staged = false;
}

void StagedIndirectBuffer::Flush(RIDevice *dev, RICmd *cmd, uint64_t byteOffset,
                                 uint64_t byteSize, bool firstUse,
                                 bool cullFollows) {
  if (!staged || byteSize == 0 || isEmpty())
    return;
  // A previous frame's draw left the buffer as draw arguments; a freshly
  // created one has no contents worth preserving.
  const uint32_t before = firstUse ? RI_RESOURCE_STATE_UNDEFINED
                                   : RI_RESOURCE_STATE_INDIRECT_ARGUMENT;
  const uint32_t beforeStage =
      firstUse ? RI_STAGE_NONE : RI_STAGE_DRAW_INDIRECT;
  cmd->vk_d3d12_bufferBarrier(RIBufferBarrier(&device, before,
                                              RI_RESOURCE_STATE_COPY_DST,
                                              beforeStage, RI_STAGE_COPY));
  cmd->copyBuffer(dev, &host, byteOffset, &device, byteOffset, byteSize);
  // A cull dispatch rewrites instanceCount next and its own closing barrier
  // declares STORAGE_WRITE as the before-state; without one the draw reads the
  // commands straight from here, so they have to land as arguments instead.
  cmd->vk_d3d12_bufferBarrier(RIBufferBarrier(
      &device, RI_RESOURCE_STATE_COPY_DST,
      cullFollows ? RI_RESOURCE_STATE_STORAGE_WRITE
                  : RI_RESOURCE_STATE_INDIRECT_ARGUMENT,
      RI_STAGE_COPY,
      cullFollows ? RI_STAGE_COMPUTE : RI_STAGE_DRAW_INDIRECT));
}

// Seeds the whole visibility buffer to zero. Separate from CreateCullBuffers so
// the same path covers a fresh create and any later reset.
void cStandardRenderer::ZeroCullVisibility() {
  if (m_cullVisibilityBuffer.isEmpty())
    return;
  const size_t bytes =
      static_cast<size_t>(kStandardCullVisibilityKeys) * sizeof(uint32_t);

  RIResourceBufferTransaction transaction = {};
  transaction.target = m_cullVisibilityBuffer;
  transaction.size = bytes;
  transaction.offset = 0;
  // Freshly created (or about to be reinterpreted wholesale), so nothing in it
  // needs to survive -- UNDEFINED lets the backend discard rather than preserve.
  transaction.currentState = RI_RESOURCE_STATE_UNDEFINED;
  transaction.currentStages = RI_STAGE_NONE;
  transaction.postState = RI_RESOURCE_STATE_UNORDERED_ACCESS;
  transaction.postStages = RI_STAGE_COMPUTE;
  RI_ResourceBeginCopyBuffer(&mpGraphics->device, &mpGraphics->uploader,
                             &transaction);
  if (!transaction.mapped.data) {
    Error("Standard renderer: could not stage the cull visibility seed (%zu "
          "bytes); occlusion culling is off this run\n",
          bytes);
    return;
  }
  std::memset(transaction.mapped.data, 0, bytes);
  RI_ResourceEndCopyBuffer(&mpGraphics->device, &mpGraphics->uploader,
                           &transaction);
}

// Hands every cull buffer back through the deferral queue. Without this they
// outlive the device: DestroyData used to free only the two indirect-draw
// buffers, so the rest showed up as live VMA allocations at teardown.
void cStandardRenderer::DisposeCullBuffers() {
  if (!mpGraphics)
    return;
  const auto retire = [&](struct RIBuffer *buffer) {
    if (buffer->isEmpty())
      return;
    mpGraphics->graphicsDefer.push(std::function<void()>(
        [owned = std::move(*buffer),
         device = &mpGraphics->device]() mutable { owned.dispose(device); }));
    *buffer = {};
  };
  retire(&m_shadowCandidateBuffer);
  retire(&m_shadowCullTileBuffer);
  retire(&m_shadowCullGroupBuffer);
  retire(&m_shadowDrawCountBuffer);
  retire(&m_cullCameraBuffer);
  m_translucentCommandBuffer.Defer(mpGraphics);
  retire(&m_translucentCandidateBuffer);
  retire(&m_cameraCandidateBuffer);
  retire(&m_cullVisibilityBuffer);
}

cStandardRenderer::~cStandardRenderer() { DestroyData(); }

bool cStandardRenderer::LoadData() {
  if (!mpResources || !mpGraphics->globalset) {
    Error(
        "Standard renderer: resources or global managed sets are not ready\n");
    return false;
  }
  auto loadPass = [](bool loaded, const char *name) {
    if (!loaded)
      Error("Standard renderer: %s pass failed to load\n", name);
    return loaded;
  };
  if (!loadPass(m_environment && m_environment->LoadData(), "environment"))
    return false;
  if (!loadPass(m_particles && m_particles->LoadData(), "particle"))
    return false;
  if (!loadPass(m_decals && m_decals->LoadData(), "decal"))
    return false;
  if (!loadPass(m_translucent && m_translucent->LoadData(), "translucent"))
    return false;
  if (!loadPass(m_water && m_water->LoadData(), "water"))
    return false;
  if (!loadPass(m_shadow && m_shadow->LoadData(), "shadow"))
    return false;
  // The cull kernel is not optional: without it the shadow indirect range is
  // never written, so shadows are dropped rather than drawn from stale data.
  if (!loadPass(m_shadowCull && m_shadowCull->LoadData(), "shadow cull"))
    return false;
  // The pyramid is held in a combined SHADER_RESOURCE + UNORDERED_ACCESS state
  // for the whole build, which only a COMMON-pinned simultaneous-access texture
  // admits -- and only the enhanced-barrier path pins it (RID3D12Barrier.cpp).
  // The legacy ResourceBarrier fallback has no equivalent, so the same build
  // would be a genuine state conflict there. Camera occlusion culling degrades
  // to frustum-only rather than reading a corrupt pyramid.
  m_hiZLoaded = m_hiZ && HiZBarriersUsable(mpGraphics->device) &&
                m_hiZ->LoadData();
  if (!m_hiZLoaded)
    Warning("Standard renderer: HiZ pass unavailable; camera occlusion "
            "culling is off this run\n");
  // AO is optional: without it the light pass reads a cleared fallback.
  m_ambientOcclusionLoaded =
      m_ambientOcclusion && m_ambientOcclusion->LoadData();
  if (!m_ambientOcclusionLoaded)
    Warning("Standard renderer: ambient occlusion pass failed to load; AO "
            "disabled\n");
  if (m_indirectDrawBuffer.isEmpty()) {
    RISegmentAllocDesc desc = {};
    desc.numSegments = RI_NUMBER_FRAMES_FLIGHT;
    desc.elementStride = sizeof(VkDrawIndirectCommand);
    desc.maxElements = kObjectSlotCapacity;
    m_indirectSegment = RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS>(&desc);
    m_indirectDrawBuffer.Create(&mpGraphics->device, kObjectSlotCapacity,
                                sizeof(VkDrawIndirectCommand),
                                /*hostWritten*/ true,
                                "StandardRenderer.indirectDraw");
    m_indirectDrawFirstUse = true;
  }
  if (m_shadowIndirectBuffer.isEmpty()) {
    RISegmentAllocDesc desc = {};
    desc.numSegments = RI_NUMBER_FRAMES_FLIGHT;
    desc.elementStride = sizeof(VkDrawIndirectCommand);
    desc.maxElements = kStandardShadowIndirectRing;
    m_shadowIndirectSegment = RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS>(&desc);
    m_shadowIndirectBuffer.Create(&mpGraphics->device,
                                  kStandardShadowIndirectRing,
                                  sizeof(VkDrawIndirectCommand),
                                  /*hostWritten*/ false,
                                  "StandardRenderer.shadowIndirect");
  }
  // DestroyData hands these back, so a hot-reload has to rebuild them before
  // any pass binds them again.
  CreateCullBuffers();
  const RIBindlessLayout external[] = {
      mpGraphics->globalset->m_bindlessSet.layout()};
  auto load = [&](std::shared_ptr<RIProgram> &program, const char *file,
                  const char *name) -> bool {
    auto vertBin = RIProgram::loadShaderStage(mpResources->GetFileSearcher(),
                                              file, "vsMain");
    auto fragBin = RIProgram::loadShaderStage(mpResources->GetFileSearcher(),
                                              file, "psMain");
    if (vertBin.empty() || fragBin.empty())
      return false;
    auto replacement = std::make_shared<RIProgram>();
    std::array<RIProgram::ModuleStage, 2> stages = {
        RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, vertBin, "vsMain"},
        RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, fragBin,
                               "psMain"}};
    replacement->initialize(&mpGraphics->device, stages, external, name);
    auto old = std::move(program);
    program = std::move(replacement);
    if (old) {
      mpGraphics->graphicsDefer.push(std::function<void()>(
          [old = std::move(old), device = &mpGraphics->device]() mutable {
            old->dispose(device);
          }));
    }
    // RIProgram::initialize has no result; shader presence is the established
    // success contract for these prevalidated renderer modules.
    return true;
  };
  auto retire = [&](std::shared_ptr<RIProgram> &program) {
    auto old = std::move(program);
    if (old)
      mpGraphics->graphicsDefer.push(std::function<void()>(
          [old = std::move(old), device = &mpGraphics->device]() mutable {
            old->dispose(device);
          }));
  };
  const bool usePacked = !m_forceFallback &&
                         mpGraphics->device.fragmentShaderBarycentricEnabled &&
                         mpGraphics->device.shaderInt16Enabled &&
                         mpGraphics->device.shaderFloat16Enabled &&
                         mpGraphics->device.geometryShaderEnabled;
  if (usePacked) {
    if (!m_visibilityLoaded)
      m_visibilityLoaded = load(m_visibility, "Standard.visibility.3d",
                                "Standard.visibility");
    if (!m_reconstructLoaded)
      m_reconstructLoaded = load(m_reconstruct, "Standard.reconstruct.3d",
                                 "Standard.reconstruct");
  } else {
    if (m_visibility)
      retire(m_visibility);
    m_visibilityLoaded = false;
    if (m_reconstruct)
      retire(m_reconstruct);
    m_reconstructLoaded = false;
  }
  if (!m_fallbackLoaded)
    m_fallbackLoaded =
        load(m_fallback, "Standard.fallback.3d", "Standard.fallback");
  if (!m_lightingLoaded)
    m_lightingLoaded =
        load(m_lighting, "Standard.light.3d", "Standard.light");
  // Type="Decal" meshes are optional: without the program the accumulators
  // still clear to identity and the resolve is unchanged.
  if (!m_meshDecalLoaded) {
    // Decal.vert/frag each carry two entry points (vsMain/psMain for this pass,
    // vsOcclusion/psOcclusion for cStandardHaloPass), so the entry name is
    // required: on D3D12 a multi-entry source compiles to a lib_6_8 library that
    // no graphics PSO can consume, and loadShaderStage uses the name to pick the
    // per-entry executable. Vulkan resolves to the same .spv either way.
    auto vert = RIProgram::loadShaderStage(mpResources->GetFileSearcher(),
                                           "Decal.vert", "vsMain");
    auto frag = RIProgram::loadShaderStage(mpResources->GetFileSearcher(),
                                           "Decal.frag", "psMain");
    if (!vert.empty() && !frag.empty()) {
      auto replacement = std::make_shared<RIProgram>();
      std::array<RIProgram::ModuleStage, 2> stages = {
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, vert,
                                 "vsMain"},
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, frag,
                                 "psMain"}};
      replacement->initialize(&mpGraphics->device, stages, external,
                              "Standard.meshDecal");
      if (m_meshDecal)
        retire(m_meshDecal);
      m_meshDecal = std::move(replacement);
      m_meshDecalLoaded = true;
    } else if (!m_meshDecalWarned) {
      Warning("Standard renderer: Decal.vert/frag missing; Type=\"Decal\" "
              "meshes disabled\n");
      m_meshDecalWarned = true;
    }
  }
  m_shadowLoaded = true;
  return m_fallback && m_fallbackLoaded && m_lighting && m_lightingLoaded &&
         m_shadowLoaded;
}

void cStandardRenderer::DestroyData() {
  if (m_hiZ)
    m_hiZ->DestroyData();
  m_hiZLoaded = false;
  auto visibility = std::move(m_visibility);
  auto fallback = std::move(m_fallback);
  auto reconstruct = std::move(m_reconstruct);
  auto lighting = std::move(m_lighting);
  auto environment = std::move(m_environment);
  auto particles = std::move(m_particles);
  auto decals = std::move(m_decals);
  auto shadow = std::move(m_shadow);
  auto translucent = std::move(m_translucent);
  auto water = std::move(m_water);
  auto meshDecal = std::move(m_meshDecal);
  m_shadowJitter = {};
  m_shadowJitterQuality = -1;
  if (!m_shadowAtlas.isEmpty())
    mpGraphics->graphicsDefer.push(m_shadowAtlas);
  m_shadowAtlas = {};
  m_shadowAtlasSize = 0;
  m_shadowAtlasLayers = 0;
  if (!m_shadowFallbackView.isEmpty())
    mpGraphics->graphicsDefer.push(m_shadowFallbackView);
  if (!m_shadowFallback.isEmpty())
    mpGraphics->graphicsDefer.push(m_shadowFallback);
  m_shadowFallbackView = {};
  m_shadowFallback = {};
  if (m_halo)
    m_halo->DestroyData();
  m_meshDecalLoaded = false;
  if (meshDecal)
    mpGraphics->graphicsDefer.push(
        std::function<void()>([meshDecal = std::move(meshDecal),
                               device = &mpGraphics->device]() mutable {
          meshDecal->dispose(device);
        }));
  if (shadow)
    shadow->DestroyData();
  m_shadow = std::move(shadow);
  if (visibility || fallback || reconstruct || lighting)
    mpGraphics->graphicsDefer.push(std::function<void()>(
        [visibility = std::move(visibility), fallback = std::move(fallback),
         reconstruct = std::move(reconstruct), lighting = std::move(lighting),
         device = &mpGraphics->device]() mutable {
          if (visibility)
            visibility->dispose(device);
          if (fallback)
            fallback->dispose(device);
          if (reconstruct)
            reconstruct->dispose(device);
          if (lighting)
            lighting->dispose(device);
        }));
  m_indirectDrawBuffer.Defer(mpGraphics);
  m_shadowIndirectBuffer.Defer(mpGraphics);
  DisposeCullBuffers();
  m_visibilityLoaded = false;
  m_fallbackLoaded = false;
  m_reconstructLoaded = false;
  m_lightingLoaded = false;
  m_shadowLoaded = false;
  if (environment)
    environment->DestroyData();
  m_environment = std::move(environment);
  if (particles)
    particles->DestroyData();
  m_particles = std::move(particles);
  if (decals)
    decals->DestroyData();
  m_decals = std::move(decals);
  if (translucent)
    translucent->DestroyData();
  m_translucent = std::move(translucent);
  if (water)
    water->DestroyData();
  m_water = std::move(water);
}

cViewport::StandardViewportState::~StandardViewportState() {
  cGraphics *graphics = Interface<cGraphics>::Get();
  if (!graphics)
    return;
  for (uint32_t i = 0; i < RI_MAX_SWAPCHAIN_IMAGES; ++i) {
    graphics->graphicsDefer.push(renderTarget[i]);
    graphics->graphicsDefer.push(renderTargetView[i]);
    graphics->graphicsDefer.push(renderTargetAttachmentView[i]);
    graphics->graphicsDefer.push(depthTextures[i]);
    graphics->graphicsDefer.push(depthView[i]);
    graphics->graphicsDefer.push(visibilityTexture[i]);
    graphics->graphicsDefer.push(visibilityView[i]);
    graphics->graphicsDefer.push(visibilityAttachmentView[i]);
    graphics->graphicsDefer.push(positionTexture[i]);
    graphics->graphicsDefer.push(positionView[i]);
    graphics->graphicsDefer.push(positionAttachmentView[i]);
    graphics->graphicsDefer.push(normalTexture[i]);
    graphics->graphicsDefer.push(normalView[i]);
    graphics->graphicsDefer.push(normalAttachmentView[i]);
    graphics->graphicsDefer.push(shadingNormalTexture[i]);
    graphics->graphicsDefer.push(shadingNormalView[i]);
    graphics->graphicsDefer.push(shadingNormalAttachmentView[i]);
    graphics->graphicsDefer.push(surfaceTexture[i]);
    graphics->graphicsDefer.push(surfaceView[i]);
    graphics->graphicsDefer.push(surfaceAttachmentView[i]);
    graphics->graphicsDefer.push(materialColorTexture[i]);
    graphics->graphicsDefer.push(materialColorView[i]);
    graphics->graphicsDefer.push(materialColorAttachmentView[i]);
    graphics->graphicsDefer.push(decalColorTexture[i]);
    graphics->graphicsDefer.push(decalColorView[i]);
    graphics->graphicsDefer.push(decalColorAttachmentView[i]);
    graphics->graphicsDefer.push(decalMulTexture[i]);
    graphics->graphicsDefer.push(decalMulView[i]);
    graphics->graphicsDefer.push(decalMulAttachmentView[i]);
    graphics->graphicsDefer.push(decalAddTexture[i]);
    graphics->graphicsDefer.push(decalAddView[i]);
    graphics->graphicsDefer.push(decalAddAttachmentView[i]);
    graphics->graphicsDefer.push(environmentTexture[i]);
    graphics->graphicsDefer.push(environmentView[i]);
    graphics->graphicsDefer.push(environmentAttachmentView[i]);
    graphics->graphicsDefer.push(translucentSceneCopy[i]);
    graphics->graphicsDefer.push(translucentSceneCopyView[i]);
    graphics->graphicsDefer.push(waterReflectionTexture[i]);
    graphics->graphicsDefer.push(waterReflectionView[i]);
    graphics->graphicsDefer.push(waterReflectionAttachmentView[i]);
    graphics->graphicsDefer.push(waterReflectionPositionTexture[i]);
    graphics->graphicsDefer.push(waterReflectionPositionView[i]);
    graphics->graphicsDefer.push(waterReflectionPositionAttachmentView[i]);
    graphics->graphicsDefer.push(waterReflectionOpaqueTexture[i]);
    graphics->graphicsDefer.push(waterReflectionOpaqueView[i]);
    graphics->graphicsDefer.push(waterReflectionOpaqueAttachmentView[i]);
    graphics->graphicsDefer.push(waterReflectionDepthTexture[i]);
    graphics->graphicsDefer.push(waterReflectionDepthView[i]);
    graphics->graphicsDefer.push(waterReflectionDepthSampleView[i]);
    graphics->graphicsDefer.push(waterReflectionDepthAttachmentView[i]);
    graphics->graphicsDefer.push(waterSceneCopy[i]);
    graphics->graphicsDefer.push(waterSceneCopyView[i]);
    graphics->graphicsDefer.push(waterReflectionSceneCopy[i]);
    graphics->graphicsDefer.push(waterReflectionSceneCopyView[i]);
    graphics->graphicsDefer.push(aoPreparedDepthTexture[i]);
    graphics->graphicsDefer.push(aoPreparedDepthStorageView[i]);
    graphics->graphicsDefer.push(aoQuarterTexture[i]);
    graphics->graphicsDefer.push(aoQuarterStorageView[i]);
    graphics->graphicsDefer.push(aoTexture[i]);
    graphics->graphicsDefer.push(aoStorageView[i]);
    graphics->graphicsDefer.push(aoView[i]);
  }
  for (uint32_t i = 0; i < RI_MAX_SWAPCHAIN_IMAGES; ++i) {
    if (!velocityTexture[i].isEmpty())
      graphics->graphicsDefer.push(velocityTexture[i]);
    if (!velocityView[i].isEmpty())
      graphics->graphicsDefer.push(velocityView[i]);
    if (!velocityAttachmentView[i].isEmpty())
      graphics->graphicsDefer.push(velocityAttachmentView[i]);
  }
  DeferDepthSampleViews(graphics, *this);
  hiZ.Defer(graphics);
}

cViewport::StandardViewportState::StandardViewportState(
    StandardViewportState &&rhs) noexcept
    : width(rhs.width), height(rhs.height), hiZ(std::move(rhs.hiZ)) {
  for (uint32_t i = 0; i < RI_MAX_SWAPCHAIN_IMAGES; ++i) {
    renderTarget[i] = std::move(rhs.renderTarget[i]);
    renderTargetView[i] = std::move(rhs.renderTargetView[i]);
    renderTargetAttachmentView[i] =
        std::move(rhs.renderTargetAttachmentView[i]);
    depthTextures[i] = std::move(rhs.depthTextures[i]);
    depthView[i] = std::move(rhs.depthView[i]);
    visibilityTexture[i] = std::move(rhs.visibilityTexture[i]);
    visibilityView[i] = std::move(rhs.visibilityView[i]);
    visibilityAttachmentView[i] = std::move(rhs.visibilityAttachmentView[i]);
    positionTexture[i] = std::move(rhs.positionTexture[i]);
    positionView[i] = std::move(rhs.positionView[i]);
    positionAttachmentView[i] = std::move(rhs.positionAttachmentView[i]);
    normalTexture[i] = std::move(rhs.normalTexture[i]);
    normalView[i] = std::move(rhs.normalView[i]);
    normalAttachmentView[i] = std::move(rhs.normalAttachmentView[i]);
    shadingNormalTexture[i] = std::move(rhs.shadingNormalTexture[i]);
    shadingNormalView[i] = std::move(rhs.shadingNormalView[i]);
    shadingNormalAttachmentView[i] =
        std::move(rhs.shadingNormalAttachmentView[i]);
    surfaceTexture[i] = std::move(rhs.surfaceTexture[i]);
    surfaceView[i] = std::move(rhs.surfaceView[i]);
    surfaceAttachmentView[i] = std::move(rhs.surfaceAttachmentView[i]);
    materialColorTexture[i] = std::move(rhs.materialColorTexture[i]);
    materialColorView[i] = std::move(rhs.materialColorView[i]);
    materialColorAttachmentView[i] =
        std::move(rhs.materialColorAttachmentView[i]);
    decalColorTexture[i] = std::move(rhs.decalColorTexture[i]);
    decalColorView[i] = std::move(rhs.decalColorView[i]);
    decalColorAttachmentView[i] = std::move(rhs.decalColorAttachmentView[i]);
    decalColorInitialized[i] = rhs.decalColorInitialized[i];
    rhs.decalColorInitialized[i] = false;
    decalMulTexture[i] = std::move(rhs.decalMulTexture[i]);
    decalMulView[i] = std::move(rhs.decalMulView[i]);
    decalMulAttachmentView[i] = std::move(rhs.decalMulAttachmentView[i]);
    decalAddTexture[i] = std::move(rhs.decalAddTexture[i]);
    decalAddView[i] = std::move(rhs.decalAddView[i]);
    decalAddAttachmentView[i] = std::move(rhs.decalAddAttachmentView[i]);
    environmentTexture[i] = std::move(rhs.environmentTexture[i]);
    environmentView[i] = std::move(rhs.environmentView[i]);
    environmentAttachmentView[i] = std::move(rhs.environmentAttachmentView[i]);
    translucentSceneCopy[i] = std::move(rhs.translucentSceneCopy[i]);
    translucentSceneCopyView[i] = std::move(rhs.translucentSceneCopyView[i]);
    translucentSceneCopyInitialized[i] = rhs.translucentSceneCopyInitialized[i];
    rhs.translucentSceneCopyInitialized[i] = false;
    waterReflectionTexture[i] = std::move(rhs.waterReflectionTexture[i]);
    waterReflectionView[i] = std::move(rhs.waterReflectionView[i]);
    waterReflectionAttachmentView[i] =
        std::move(rhs.waterReflectionAttachmentView[i]);
    waterReflectionPositionTexture[i] =
        std::move(rhs.waterReflectionPositionTexture[i]);
    waterReflectionPositionView[i] =
        std::move(rhs.waterReflectionPositionView[i]);
    waterReflectionPositionAttachmentView[i] =
        std::move(rhs.waterReflectionPositionAttachmentView[i]);
    waterReflectionOpaqueTexture[i] =
        std::move(rhs.waterReflectionOpaqueTexture[i]);
    waterReflectionOpaqueView[i] = std::move(rhs.waterReflectionOpaqueView[i]);
    waterReflectionOpaqueAttachmentView[i] =
        std::move(rhs.waterReflectionOpaqueAttachmentView[i]);
    waterReflectionDepthTexture[i] =
        std::move(rhs.waterReflectionDepthTexture[i]);
    waterReflectionDepthView[i] = std::move(rhs.waterReflectionDepthView[i]);
    waterReflectionDepthSampleView[i] =
        std::move(rhs.waterReflectionDepthSampleView[i]);
    waterReflectionDepthAttachmentView[i] =
        std::move(rhs.waterReflectionDepthAttachmentView[i]);
    waterSceneCopy[i] = std::move(rhs.waterSceneCopy[i]);
    waterSceneCopyView[i] = std::move(rhs.waterSceneCopyView[i]);
    waterReflectionSceneCopy[i] = std::move(rhs.waterReflectionSceneCopy[i]);
    waterReflectionSceneCopyView[i] =
        std::move(rhs.waterReflectionSceneCopyView[i]);
    waterReflectionInitialized[i] = rhs.waterReflectionInitialized[i];
    waterSceneCopyInitialized[i] = rhs.waterSceneCopyInitialized[i];
    waterReflectionSceneCopyInitialized[i] =
        rhs.waterReflectionSceneCopyInitialized[i];
    rhs.waterReflectionInitialized[i] = rhs.waterSceneCopyInitialized[i] =
        rhs.waterReflectionSceneCopyInitialized[i] = false;
    environmentInitialized[i] = rhs.environmentInitialized[i];
    rhs.environmentInitialized[i] = false;
    aoPreparedDepthTexture[i] = std::move(rhs.aoPreparedDepthTexture[i]);
    aoPreparedDepthStorageView[i] =
        std::move(rhs.aoPreparedDepthStorageView[i]);
    aoQuarterTexture[i] = std::move(rhs.aoQuarterTexture[i]);
    aoQuarterStorageView[i] = std::move(rhs.aoQuarterStorageView[i]);
    aoTexture[i] = std::move(rhs.aoTexture[i]);
    aoStorageView[i] = std::move(rhs.aoStorageView[i]);
    aoView[i] = std::move(rhs.aoView[i]);
    aoInitialized[i] = rhs.aoInitialized[i];
    rhs.aoInitialized[i] = false;
  }
  for (uint32_t i = 0; i < RI_MAX_SWAPCHAIN_IMAGES; ++i) {
    velocityTexture[i] = std::move(rhs.velocityTexture[i]);
    velocityView[i] = std::move(rhs.velocityView[i]);
    velocityAttachmentView[i] = std::move(rhs.velocityAttachmentView[i]);
  }
  waterReflection = std::move(rhs.waterReflection);
  haloQueries = std::move(rhs.haloQueries);
  MoveDepthSampleViews(*this, rhs);
  rhs.width = 0;
  rhs.height = 0;
}

cViewport::StandardViewportState &cViewport::StandardViewportState::operator=(
    StandardViewportState &&rhs) noexcept {
  if (this == &rhs)
    return *this;
  this->~StandardViewportState();
  new (this) StandardViewportState(std::move(rhs));
  return *this;
}

void cViewport::StandardViewportState::Update(cGraphics::FrameContext *cntx,
                                              cVector2l size) {
  (void)cntx;
  cGraphics *graphics = Interface<cGraphics>::Get();
  if (!graphics)
    return;

  if (size.x <= 0 || size.y <= 0) {
    *this = StandardViewportState{};
    return;
  }

  const uint32_t widthValue = static_cast<uint32_t>(size.x);
  const uint32_t heightValue = static_cast<uint32_t>(size.y);
  const uint32_t imageCount =
      graphics->swapchain ? graphics->swapchain->imageCount : 0;
  if (imageCount == 0 || imageCount > RI_MAX_SWAPCHAIN_IMAGES) {
    *this = StandardViewportState{};
    return;
  }
  if (width == widthValue && height == heightValue &&
      HasTargets(*this, imageCount))
    return;

  // Build off to the side. A failed image/view creation resets the old state
  // as well, so stale targets at the previous extent cannot be published as
  // the current render extent.
  StandardViewportState replacement;
  replacement.width = widthValue;
  replacement.height = heightValue;
  bool success = true;
  for (uint32_t i = 0; i < imageCount && success; ++i) {
    auto makeAttachmentView = [&](RISharedPointer<RITexture> &texture,
                                  RI_Format_e format,
                                  RISharedPointer<RITextureView> &out) {
      RITextureViewDesc vd = {};
      vd.viewType = RI_VIEWTYPE_COLOR_ATTACHMENT;
      vd.format = format;
      vd.mipNum = 1;
      vd.layerNum = 1;
      RITextureView av =
          RITextureView::create(&graphics->device, texture.Get(), vd);
      out = RISharedPointer<RITextureView>(&graphics->device, av);
      return !av.isEmpty();
    };
    success = CreateViewportColorTexture(
        &graphics->device, widthValue, heightValue, cGraphics::PogoColorFormat,
        RI_USAGE_COLOR_ATTACHMENT | RI_USAGE_SHADER_RESOURCE |
            RI_USAGE_TRANSFER_SRC |
            static_cast<decltype(RI_USAGE_TRANSFER_SRC)>(0x40),
        &replacement.renderTarget[i], &replacement.renderTargetView[i],
        "StandardViewportState.renderTarget");
    if (success)
      success = makeAttachmentView(replacement.renderTarget[i],
                                   cGraphics::PogoColorFormat,
                                   replacement.renderTargetAttachmentView[i]);
    success =
        success &&
        CreateViewportAttachmentTexture(
            &graphics->device, widthValue, heightValue, cGraphics::DepthFormat,
            RI_USAGE_DEPTH_STENCIL_ATTACHMENT | RI_USAGE_SHADER_RESOURCE,
            RI_VIEWTYPE_DEPTH_STENCIL_ATTACHMENT, &replacement.depthTextures[i],
            &replacement.depthView[i], "StandardViewportState.depth");
    if (success)
      success = CreateDepthSampleView(graphics, i, replacement);
    if (success)
      success = replacement.hiZ.Create(graphics, i, widthValue, heightValue);
    // Five full-resolution material MRT outputs. renderTarget is albedo/final
    // HDR; the other four remain available to lighting, post effects, and
    // diagnostics.
    if (graphics->device.physicalAdapter.colorAttachmentMaxNum < 6 ||
        widthValue > 32767 || heightValue > 32767)
      success = false;
    success =
        success &&
        CreateViewportColorTexture(
            &graphics->device, widthValue, heightValue, RI_FORMAT_RGBA32_SFLOAT,
            RI_USAGE_COLOR_ATTACHMENT | RI_USAGE_SHADER_RESOURCE,
            &replacement.positionTexture[i], &replacement.positionView[i],
            "StandardViewportState.position");
    success =
        success && makeAttachmentView(replacement.positionTexture[i],
                                      RI_FORMAT_RGBA32_SFLOAT,
                                      replacement.positionAttachmentView[i]);
    success =
        success &&
        CreateViewportColorTexture(
            &graphics->device, widthValue, heightValue, RI_FORMAT_RGBA32_SFLOAT,
            RI_USAGE_COLOR_ATTACHMENT | RI_USAGE_SHADER_RESOURCE,
            &replacement.normalTexture[i], &replacement.normalView[i],
            "StandardViewportState.normal");
    success =
        success && makeAttachmentView(replacement.normalTexture[i],
                                      RI_FORMAT_RGBA32_SFLOAT,
                                      replacement.normalAttachmentView[i]);
    success = success &&
              CreateViewportColorTexture(&graphics->device, widthValue,
                                         heightValue, RI_FORMAT_RGBA32_SFLOAT,
                                         RI_USAGE_COLOR_ATTACHMENT |
                                             RI_USAGE_SHADER_RESOURCE,
                                         &replacement.shadingNormalTexture[i],
                                         &replacement.shadingNormalView[i],
                                         "StandardViewportState.shadingNormal");
    success = success &&
              makeAttachmentView(replacement.shadingNormalTexture[i],
                                 RI_FORMAT_RGBA32_SFLOAT,
                                 replacement.shadingNormalAttachmentView[i]);
    success = success &&
              CreateViewportColorTexture(
                  &graphics->device, widthValue, heightValue,
                  cGraphics::VisibilityFormat,
                  RI_USAGE_COLOR_ATTACHMENT | RI_USAGE_SHADER_RESOURCE,
                  &replacement.surfaceTexture[i], &replacement.surfaceView[i],
                  "StandardViewportState.surface");
    success =
        success && makeAttachmentView(replacement.surfaceTexture[i],
                                      cGraphics::VisibilityFormat,
                                      replacement.surfaceAttachmentView[i]);
    success = success &&
              CreateViewportColorTexture(
                  &graphics->device, widthValue, heightValue,
                  cGraphics::VelocityFormat,
                  RI_USAGE_COLOR_ATTACHMENT | RI_USAGE_SHADER_RESOURCE,
                  &replacement.velocityTexture[i], &replacement.velocityView[i],
                  "StandardViewportState.velocity");
    success =
        success && makeAttachmentView(replacement.velocityTexture[i],
                                      cGraphics::VelocityFormat,
                                      replacement.velocityAttachmentView[i]);

    success =
        success && CreateViewportColorTexture(
                       &graphics->device, widthValue, heightValue,
                       cGraphics::PogoColorFormat,
                       RI_USAGE_COLOR_ATTACHMENT | RI_USAGE_SHADER_RESOURCE,
                       &replacement.materialColorTexture[i],
                       &replacement.materialColorView[i],
                       "StandardViewportState.materialColor");
    success = success &&
              makeAttachmentView(replacement.materialColorTexture[i],
                                 cGraphics::PogoColorFormat,
                                 replacement.materialColorAttachmentView[i]);
    success =
        success &&
        CreateViewportColorTexture(
            &graphics->device, widthValue, heightValue,
            cGraphics::PogoColorFormat,
            RI_USAGE_COLOR_ATTACHMENT | RI_USAGE_SHADER_RESOURCE,
            &replacement.decalColorTexture[i], &replacement.decalColorView[i],
            "StandardViewportState.decalColor");
    success =
        success && makeAttachmentView(replacement.decalColorTexture[i],
                                      cGraphics::PogoColorFormat,
                                      replacement.decalColorAttachmentView[i]);
    for (auto target : {std::make_tuple(&replacement.decalMulTexture[i],
                                        &replacement.decalMulView[i],
                                        &replacement.decalMulAttachmentView[i],
                                        "StandardViewportState.decalMul"),
                        std::make_tuple(&replacement.decalAddTexture[i],
                                        &replacement.decalAddView[i],
                                        &replacement.decalAddAttachmentView[i],
                                        "StandardViewportState.decalAdd")}) {
      success =
          success &&
          CreateViewportColorTexture(
              &graphics->device, widthValue, heightValue,
              cGraphics::PogoColorFormat,
              RI_USAGE_COLOR_ATTACHMENT | RI_USAGE_SHADER_RESOURCE,
              std::get<0>(target), std::get<1>(target), std::get<3>(target));
      success = success && makeAttachmentView(*std::get<0>(target),
                                              cGraphics::PogoColorFormat,
                                              *std::get<2>(target));
    }
    replacement.aoInitialized[i] = false;
    success =
        success &&
        CreateViewportColorTexture(
            &graphics->device, widthValue, heightValue,
            cGraphics::PogoColorFormat,
            RI_USAGE_COLOR_ATTACHMENT | RI_USAGE_SHADER_RESOURCE |
                RI_USAGE_TRANSFER_SRC |
                static_cast<decltype(RI_USAGE_TRANSFER_SRC)>(0x40),
            &replacement.environmentTexture[i], &replacement.environmentView[i],
            "StandardViewportState.environment");
    if (success) {
      RITextureViewDesc vd = {};
      vd.viewType = RI_VIEWTYPE_COLOR_ATTACHMENT;
      vd.format = cGraphics::PogoColorFormat;
      vd.mipNum = 1;
      vd.layerNum = 1;
      RITextureView av = RITextureView::create(
          &graphics->device, replacement.environmentTexture[i].Get(), vd);
      replacement.environmentAttachmentView[i] =
          RISharedPointer<RITextureView>(&graphics->device, av);
      success = !av.isEmpty();
    }
    success =
        success && CreateViewportColorTexture(
                       &graphics->device, widthValue, heightValue,
                       cGraphics::PogoColorFormat,
                       RI_USAGE_SHADER_RESOURCE | RI_USAGE_TRANSFER_DST |
                           static_cast<decltype(RI_USAGE_TRANSFER_DST)>(0x40),
                       &replacement.translucentSceneCopy[i],
                       &replacement.translucentSceneCopyView[i],
                       "StandardViewportState.translucentSceneCopy");

    const uint32_t quarterWidth = (widthValue + 3) / 4;
    const uint32_t quarterHeight = (heightValue + 3) / 4;

    // Ambient occlusion textures are optional. Allocation failure clears only AO
    // members without invalidating the viewport.
    {
      bool aoSuccess = true;

      // Allocate AO resources: aoPreparedDepthTexture, aoQuarterTexture, aoTexture.
      // The two quarter-resolution arrays hold one slice per full-resolution
      // offset in a 4x4 block and are only ever read as storage images.
      static constexpr uint32_t kAOSlices = 16;
      aoSuccess = aoSuccess &&
                  CreateViewportAttachmentTexture(
                      &graphics->device, quarterWidth, quarterHeight,
                      RI_FORMAT_R16_SFLOAT, RI_USAGE_SHADER_RESOURCE_STORAGE,
                      RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_2D_ARRAY,
                      &replacement.aoPreparedDepthTexture[i],
                      &replacement.aoPreparedDepthStorageView[i],
                      "StandardViewportState.aoPreparedDepth", kAOSlices);
      aoSuccess = aoSuccess &&
                  CreateViewportAttachmentTexture(
                      &graphics->device, quarterWidth, quarterHeight,
                      RI_FORMAT_R16_SFLOAT, RI_USAGE_SHADER_RESOURCE_STORAGE,
                      RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_2D_ARRAY,
                      &replacement.aoQuarterTexture[i],
                      &replacement.aoQuarterStorageView[i],
                      "StandardViewportState.aoQuarter", kAOSlices);
      // Written by reinterleave, cleared to 1 first, sampled by the light pass.
      aoSuccess =
          aoSuccess &&
          CreateViewportAttachmentTexture(
              &graphics->device, widthValue, heightValue, RI_FORMAT_R16_SFLOAT,
              RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_SHADER_RESOURCE |
                  RI_USAGE_TRANSFER_DST,
              RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_2D, &replacement.aoTexture[i],
              &replacement.aoStorageView[i], "StandardViewportState.ao");
      // Create additional shader resource view for aoTexture (optional, doesn't fail AO)
      if (!replacement.aoTexture[i].isEmpty()) {
        RITextureViewDesc aoShaderView = {};
        aoShaderView.viewType = RI_VIEWTYPE_SHADER_RESOURCE_2D;
        aoShaderView.format = RI_FORMAT_R16_SFLOAT;
        aoShaderView.mipNum = 1;
        aoShaderView.layerNum = 1;
        RITextureView aoShaderViewObj = RITextureView::create(
            &graphics->device, replacement.aoTexture[i].Get(), aoShaderView);
        if (!aoShaderViewObj.isEmpty()) {
          replacement.aoView[i] = RISharedPointer<RITextureView>(
              &graphics->device, aoShaderViewObj);
        }
      }

      // If any AO allocation failed, clear all AO members but keep success intact
      if (!aoSuccess) {
        replacement.aoPreparedDepthTexture[i] = {};
        replacement.aoPreparedDepthStorageView[i] = {};
        replacement.aoQuarterTexture[i] = {};
        replacement.aoQuarterStorageView[i] = {};
        replacement.aoTexture[i] = {};
        replacement.aoStorageView[i] = {};
        replacement.aoView[i] = {};
      }
    }

    const uint32_t reflectionWidth = std::max(1u, widthValue / 2u);
    const uint32_t reflectionHeight = std::max(1u, heightValue / 2u);
    success =
        success && CreateViewportColorTexture(
                       &graphics->device, reflectionWidth, reflectionHeight,
                       cGraphics::PogoColorFormat,
                       RI_USAGE_COLOR_ATTACHMENT | RI_USAGE_SHADER_RESOURCE |
                           RI_USAGE_TRANSFER_SRC,
                       &replacement.waterReflectionTexture[i],
                       &replacement.waterReflectionView[i],
                       "StandardViewportState.waterReflection");
    // Refraction source for translucents drawn inside the capture. Failure is
    // survivable: the capture then draws them without refraction.
    if (success)
      CreateViewportColorTexture(
          &graphics->device, reflectionWidth, reflectionHeight,
          cGraphics::PogoColorFormat,
          RI_USAGE_SHADER_RESOURCE | RI_USAGE_TRANSFER_DST,
          &replacement.waterReflectionSceneCopy[i],
          &replacement.waterReflectionSceneCopyView[i],
          "StandardViewportState.waterReflectionSceneCopy");
    success = success &&
              makeAttachmentView(replacement.waterReflectionTexture[i],
                                 cGraphics::PogoColorFormat,
                                 replacement.waterReflectionAttachmentView[i]);
    success =
        success && CreateViewportColorTexture(
                       &graphics->device, reflectionWidth, reflectionHeight,
                       RI_FORMAT_RGBA32_SFLOAT,
                       RI_USAGE_COLOR_ATTACHMENT | RI_USAGE_SHADER_RESOURCE,
                       &replacement.waterReflectionPositionTexture[i],
                       &replacement.waterReflectionPositionView[i],
                       "StandardViewportState.waterReflectionPosition");
    success =
        success && makeAttachmentView(
                       replacement.waterReflectionPositionTexture[i],
                       RI_FORMAT_RGBA32_SFLOAT,
                       replacement.waterReflectionPositionAttachmentView[i]);
    success =
        success && CreateViewportColorTexture(
                       &graphics->device, reflectionWidth, reflectionHeight,
                       cGraphics::PogoColorFormat,
                       RI_USAGE_COLOR_ATTACHMENT | RI_USAGE_SHADER_RESOURCE,
                       &replacement.waterReflectionOpaqueTexture[i],
                       &replacement.waterReflectionOpaqueView[i],
                       "StandardViewportState.waterReflectionOpaque");
    success =
        success &&
        makeAttachmentView(replacement.waterReflectionOpaqueTexture[i],
                           cGraphics::PogoColorFormat,
                           replacement.waterReflectionOpaqueAttachmentView[i]);
    success = success &&
              CreateViewportAttachmentTexture(
                  &graphics->device, reflectionWidth, reflectionHeight,
                  cGraphics::DepthFormat,
                  RI_USAGE_DEPTH_STENCIL_ATTACHMENT | RI_USAGE_SHADER_RESOURCE,
                  RI_VIEWTYPE_DEPTH_STENCIL_ATTACHMENT,
                  &replacement.waterReflectionDepthTexture[i],
                  &replacement.waterReflectionDepthView[i],
                  "StandardViewportState.waterReflectionDepth");

    if (success) {
      RITextureViewDesc depthSample = {};
      depthSample.viewType = RI_VIEWTYPE_SHADER_RESOURCE_2D;
      depthSample.format = cGraphics::DepthFormat;
      depthSample.mipNum = depthSample.layerNum = 1;
      RITextureView sample = RITextureView::create(
          &graphics->device, replacement.waterReflectionDepthTexture[i].Get(),
          depthSample);
      replacement.waterReflectionDepthSampleView[i] =
          RISharedPointer<RITextureView>(&graphics->device, sample);
      success = !sample.isEmpty();
    }
    if (success) {
      RITextureViewDesc depthAttachment = {};
      depthAttachment.viewType = RI_VIEWTYPE_DEPTH_STENCIL_ATTACHMENT;
      depthAttachment.format = cGraphics::DepthFormat;
      depthAttachment.mipNum = depthAttachment.layerNum = 1;
      RITextureView view = RITextureView::create(
          &graphics->device, replacement.waterReflectionDepthTexture[i].Get(),
          depthAttachment);
      replacement.waterReflectionDepthAttachmentView[i] =
          RISharedPointer<RITextureView>(&graphics->device, view);
      success = !view.isEmpty();
    }
    success =
        success &&
        CreateViewportColorTexture(
            &graphics->device, widthValue, heightValue,
            cGraphics::PogoColorFormat,
            RI_USAGE_SHADER_RESOURCE | RI_USAGE_TRANSFER_DST |
                static_cast<decltype(RI_USAGE_TRANSFER_DST)>(0x40),
            &replacement.waterSceneCopy[i], &replacement.waterSceneCopyView[i],
            "StandardViewportState.waterSceneCopy");
    // Packed visibility is an enhancement, not a prerequisite for the
    // conventional renderer. Devices with only one color attachment, or
    // where the integer attachment cannot be allocated, retain the color and
    // depth targets and use the conventional material path instead.
    if (success && graphics->device.fragmentShaderBarycentricEnabled) {
      if (!CreateViewportAttachmentTexture(
              &graphics->device, widthValue, heightValue,
              cGraphics::VisibilityFormat,
              RI_USAGE_COLOR_ATTACHMENT | RI_USAGE_SHADER_RESOURCE,
              RI_VIEWTYPE_SHADER_RESOURCE_2D, &replacement.visibilityTexture[i],
              &replacement.visibilityView[i],
              "StandardViewportState.visibility")) {
        replacement.visibilityTexture[i] = {};
        replacement.visibilityView[i] = {};
      } else {
        RITextureViewDesc vd = {};
        vd.viewType = RI_VIEWTYPE_COLOR_ATTACHMENT;
        vd.format = cGraphics::VisibilityFormat;
        vd.mipNum = 1;
        vd.layerNum = 1;
        RITextureView av = RITextureView::create(
            &graphics->device, replacement.visibilityTexture[i].Get(), vd);
        replacement.visibilityAttachmentView[i] =
            RISharedPointer<RITextureView>(&graphics->device, av);
        if (av.isEmpty())
          success = false;
      }
    }
  }

  replacement.temporal.Reset();
  if (!success) {
    *this = StandardViewportState{};
    return;
  }
  *this = std::move(replacement);
}

void cStandardRenderer::Draw(cGraphics::FrameContext *cntx, cViewport *viewport,
                             float afFrameTime, cFrustum *apFrustum,
                             cWorld *apWorld, cRenderSettings *apSettings,
                             bool abSendFrameBufferToPostEffects) {
  (void)abSendFrameBufferToPostEffects;
  if (!m_fallback || !m_fallbackLoaded || !m_lighting || !m_lightingLoaded) {
    static bool sLoggedNotLoaded = false;
    if (!sLoggedNotLoaded) {
      Error("Standard renderer: Draw skipped because its shaders are not "
            "loaded\n");
      sLoggedNotLoaded = true;
    }
    return;
  }
  if (!mpGraphics || !apWorld || !viewport || !apFrustum ||
      m_indirectDrawBuffer.isEmpty())
    return;
  // Standard keeps a separate legacy ABI: enhanced-world buffers are not
  // reusable because their radius/intensity and colour transfer functions are
  // intentionally different. Build before any resolve recording so texture
  // pins and the SSBO contents share this frame's lifetime.
  RISharedPointer<RIBuffer> pointLights;
  RISharedPointer<RIBuffer> spotLights;
  RISharedPointer<RIBuffer> boxLights;
  uint32_t pointLightCount = 0, spotLightCount = 0, boxLightCount = 0;
  // Full buffer extents, including the lights culled from the camera view. Only
  // the mirrored water-reflection view reads past the visible prefix.
  uint32_t pointLightCountTotal = 0, spotLightCountTotal = 0;
  uint32_t shadowCount = 0;
  const eShadowMapResolution shadowResolutionCap =
      apSettings ? apSettings->mMaxShadowMapResolution
                 : eShadowMapResolution_High;
  const bool shadowsAvailable = m_shadow && m_shadowLoaded &&
                                (!apSettings || apSettings->mbRenderShadows);
  // Legacy RendererDeferred reported the lights it drew (MapView shows this):
  // visible legacy lights touching the camera frustum.
  if (apSettings) {
    int renderedLights = 0;
    for (iLight *light : *apWorld->GetLightList()) {
      if (light && light->GetLightType() != eLightType_Area &&
          light->GetVisibleVar() && light->IsLegacyRendererEnabled() &&
          (!apFrustum || apFrustum->CollideBoundingVolume(
                             light->GetBoundingVolume()) != eCollision_Outside))
        ++renderedLights;
    }
    apSettings->mlNumberOfLightsRendered = renderedLights;
  }
  // Shadow candidates: legacy spot and point lights touching the camera
  // frustum (legacy RendererDeferred selection). The tile size starts from the
  // authored quality clamped by the cap, steps down with distance as the legacy
  // renderer did, and is then lowered to the light's projected screen size
  // before atlas packing. Nearest lights get budget priority.
  struct ShadowSelection {
    iLight *light;
    bool point;
    float distance;
    uint32_t size;
  };
  std::vector<ShadowSelection> shadowSelection;
  if (shadowsAvailable) {
    const cVector2l selectionExtent = viewport->GetRenderExtent();
    const float halfFovTan = std::tan(apFrustum->GetFOV() * 0.5f);
    for (iLight *light : *apWorld->GetLightList()) {
      if (!light || (light->GetLightType() != eLightType_Spot &&
                     light->GetLightType() != eLightType_Point))
        continue;
      const bool point = light->GetLightType() == eLightType_Point;
      const float radius = light->GetRadius();
      if (light->GetLightType() == eLightType_Area ||
          !light->GetVisibleVar() || !light->IsLegacyRendererEnabled() ||
          !light->GetCastShadows() || light->GetShadowCastersAffected() == 0 ||
          !StandardFinite(radius) || radius <= 0.0f)
        continue;
      // Malformed authored slope bias must never reach the driver.
      if (!StandardFinite(light->GetShadowMapSlopeScaleBiasMul()))
        continue;
      if (apFrustum->CollideBoundingVolume(light->GetBoundingVolume()) ==
          eCollision_Outside)
        continue;
      float distance = 0.0f;
      if (point) {
        if (radius <= StandardPointShadowNear(radius))
          continue;
        distance = std::max(cMath::Vector3Dist(apFrustum->GetOrigin(),
                                               light->GetWorldPosition()) -
                                radius,
                            0.0f);
      } else {
        iLightSpot *spot = static_cast<iLightSpot *>(light);
        const float fov = spot->GetFOV(), aspect = spot->GetAspect();
        const float nearClip = spot->GetNearClipPlane();
        if (radius <= nearClip || !StandardFinite(fov) || fov <= 0.0f ||
            fov >= 3.14159265358979323846f || !StandardFinite(aspect) ||
            aspect <= 0.0f || !StandardFinite(nearClip) || nearClip <= 0.0f)
          continue;
        // Distance from the camera to the light frustum.
        if (!apFrustum->CheckFrustumNearPlaneIntersection(spot->GetFrustum())) {
          cVector3f intersection = spot->GetFrustum()->GetOrigin();
          spot->GetFrustum()->CheckLineIntersection(
              apFrustum->GetOrigin(),
              spot->GetBoundingVolume()->GetWorldCenter(), intersection);
          distance = cMath::Vector3Dist(apFrustum->GetOrigin(), intersection);
        }
      }
      if (!StandardFinite(distance) || distance > kStandardShadowDistanceNone)
        continue;
      eShadowMapResolution quality = StandardCapShadowQuality(
          light->GetShadowMapResolution(), shadowResolutionCap);
      if (distance > kStandardShadowDistanceLow) {
        if (quality == eShadowMapResolution_Low)
          continue;
        quality = eShadowMapResolution_Low;
      } else if (distance > kStandardShadowDistanceMedium) {
        quality = quality == eShadowMapResolution_High
                      ? eShadowMapResolution_Medium
                      : eShadowMapResolution_Low;
      }
      // Projected bounding-sphere diameter in pixels. A camera inside the
      // sphere passes 0, which keeps the legacy step.
      cBoundingVolume *bounds = light->GetBoundingVolume();
      const float boundsRadius = bounds->GetRadius();
      const float centreDistance =
          cMath::Vector3Dist(apFrustum->GetOrigin(), bounds->GetWorldCenter());
      float projectedDiameter = 0.0f;
      if (StandardFinite(boundsRadius) && centreDistance > boundsRadius &&
          halfFovTan > 0.0f)
        projectedDiameter = boundsRadius / (centreDistance * halfFovTan) *
                            static_cast<float>(selectionExtent.y);
      const uint32_t size =
          StandardShadowTileSize(StandardShadowResolution(quality),
                                 projectedDiameter, kStandardShadowMinTile);
      if (size == 0)
        continue;
      shadowSelection.push_back({light, point, distance, size});
    }
    std::stable_sort(shadowSelection.begin(), shadowSelection.end(),
                     [](const ShadowSelection &a, const ShadowSelection &b) {
                       return a.distance < b.distance;
                     });
  }
  m_rendererList.BeginAndReset(afFrameTime, apFrustum);
  auto *dynamicContainer =
      apWorld->GetRenderableSet(eWorldContainerType_Dynamic);
  auto *staticContainer = apWorld->GetRenderableSet(eWorldContainerType_Static);
  if (!dynamicContainer || !staticContainer)
    return;
  dynamicContainer->UpdateBeforeRendering();
  staticContainer->UpdateBeforeRendering();
  auto add = [&](iRenderable *o) {
    if (o && rendering::IsObjectIsVisible(
                 o, eRenderableFlag_VisibleInNonReflection, {}))
      m_rendererList.AddObject(o);
  };
  rendering::WalkAndPrepareRenderList(dynamicContainer, apFrustum, add,
                                      eRenderableFlag_VisibleInNonReflection);
  rendering::WalkAndPrepareRenderList(staticContainer, apFrustum, add,
                                      eRenderableFlag_VisibleInNonReflection);
  m_rendererList.End(
      eRenderListCompileFlag_Diffuse | eRenderListCompileFlag_FogArea |
      eRenderListCompileFlag_Translucent | eRenderListCompileFlag_Decal);
  auto solids = m_rendererList.GetSolidObjects();
  for (iRenderable *o : solids) {
    if (o && o->GetVertexBuffer())
      static_cast<cVertexBuffer *>(o->GetVertexBuffer())
          ->SubmitToGPU(&mpGraphics->device);
  }
  const uint32_t index = mpGraphics->swapchainIndex;
  const uint32_t imageCount =
      mpGraphics->swapchain ? mpGraphics->swapchain->imageCount : 0;
  if (index >= RI_MAX_SWAPCHAIN_IMAGES || index >= imageCount)
    return;

  const cVector2l extent = viewport->GetRenderExtent();
  if (extent.x <= 0 || extent.y <= 0)
    return;
  cViewport::StandardViewportState *state =
      viewport->PrepareToRender<cViewport::StandardViewportState>(cntx, extent);
  if (!state || state->width == 0 || state->height == 0 ||
      state->renderTarget[index].isEmpty() ||
      state->renderTargetView[index].isEmpty() ||
      state->depthView[index].isEmpty())
    return;
  static bool sLoggedD3D12MrtVerification = false;
  if (!sLoggedD3D12MrtVerification) {
    sLoggedD3D12MrtVerification = true;
  }

  // Publish exactly the matrices used by the raster pass.
  const uint32_t jitterPhaseCount = viewport->GetTemporalJitterPhaseCount();
  const hpl::TemporalJitter pendingJitter =
      jitterPhaseCount == 0
          ? hpl::TemporalJitter{}
          : hpl::TemporalPendingJitter(state->temporal, jitterPhaseCount);

  // GetViewMat/GetProjectionMat return by value; TemporalFrameDesc stores raw
  // pointers, so the matrices must outlive TemporalBeginFrame (as in Hybrid).
  const ml::float4x4 frustumViewMat = apFrustum->GetViewMat();
  const ml::float4x4 frustumProjMat = apFrustum->GetProjectionMat();

  hpl::TemporalFrameDesc temporalDesc = {};
  temporalDesc.viewMat = frustumViewMat.a;
  temporalDesc.unjitteredProjMat = frustumProjMat.a;
  temporalDesc.renderWidth = state->width;
  temporalDesc.renderHeight = state->height;
  temporalDesc.jitterPixels[0] = pendingJitter.x;
  temporalDesc.jitterPixels[1] = pendingJitter.y;
  // Camera cuts / teleports request a history reset on the viewport; Hybrid
  // consumes the same flag.
  temporalDesc.forceHistoryReset = viewport->ConsumeTemporalHistoryReset();

  const hpl::TemporalFrameSnapshot temporalFrame =
      hpl::TemporalBeginFrame(state->temporal, temporalDesc);

  viewport->PublishRasterCamera(temporalFrame.viewMat, temporalFrame.projMat);
  viewport->PublishRasterTemporalFrame(temporalFrame, afFrameTime * 1000.0f);

  // Hi-Z contains jittered raster depth. Convert the temporal projection to
  // row-major for occlusion while retaining the original frustum planes.
  cMatrixf rasterProjection;
  rasterProjection.FromTranspose(temporalFrame.projMat);
  const cMatrixf occlusionViewProjection =
      cMath::MatrixMul(rasterProjection, apFrustum->GetViewMatrix());

  uint32_t drawCount = 0;
  RISegmentReq req = {};
  VkDrawIndirectCommand *indirect = nullptr;
  if (!solids.empty() &&
      m_indirectSegment.request(mpGraphics->frameIndex, solids.size(), &req)) {
    indirect = reinterpret_cast<VkDrawIndirectCommand *>(
        static_cast<uint8_t *>(m_indirectDrawBuffer.mapped()) +
        req.elementOffset * sizeof(VkDrawIndirectCommand));
  }

  // Two-phase camera cull. Phase 1 replays what was visible last frame so the
  // pyramid has something to be built from; phase 2 tests everything against
  // that pyramid and draws whatever phase 1 missed. Both phases draw the SAME
  // command list geometry, from two ranges whose instanceCount the kernel
  // owns, so the host fills both identically here and never has to know the
  // answer.
  RISegmentReq phaseTwoReq = {};
  RISegmentReq cameraCandidateReq = {};
  VkDrawIndirectCommand *phaseTwoIndirect = nullptr;
  StandardCullCandidate *cameraCandidates = nullptr;
  const bool cameraCullReady =
      indirect != nullptr && m_hiZ && m_hiZLoaded &&
      m_shadowCull && m_shadowCull->IsLoaded() && state->hiZ.IsUsable(index) &&
      solids.size() <= kStandardCameraMaxDraws &&
      m_indirectSegment.request(mpGraphics->frameIndex, solids.size(),
                                &phaseTwoReq) &&
      m_cameraCandidateSegment.request(mpGraphics->frameIndex, solids.size(),
                                       &cameraCandidateReq);
  if (cameraCullReady) {
    phaseTwoIndirect = reinterpret_cast<VkDrawIndirectCommand *>(
        static_cast<uint8_t *>(m_indirectDrawBuffer.mapped()) +
        phaseTwoReq.elementOffset * sizeof(VkDrawIndirectCommand));
    cameraCandidates = reinterpret_cast<StandardCullCandidate *>(
        static_cast<uint8_t *>(m_cameraCandidateBuffer.mappedAddress) +
        cameraCandidateReq.elementOffset * sizeof(StandardCullCandidate));
  }
  // Object slots are shared by the global set. Salt each pane so a second
  // camera cannot overwrite this pane's model/UV/material record mid-frame.
  const uint32_t paneSalt =
      hash_u64(HASH_INITIAL_VALUE, reinterpret_cast<uintptr_t>(viewport));
  // Slots claimed by the camera pass. The shadow gather reuses them instead of
  // re-submitting: a caster the camera can see needs no second upload, and its
  // record is identical because both callers build it the same way.
  std::unordered_map<iRenderable *, uint32_t> submittedSlots;
  submittedSlots.reserve(solids.size());
  for (iRenderable *o : solids) {
    if (!o || !o->GetVertexBuffer())
      continue;
    cVertexBuffer *vb = static_cast<cVertexBuffer *>(o->GetVertexBuffer());
    cMaterial *mat = o->GetMaterial();
    if (!vb || !mat || !indirect)
      continue;
    const uint32_t materialId =
        mpGraphics->globalset
            ->submitMaterial(cntx, mat,
                             static_cast<uint32_t>(mpGraphics->frameIndex))
            .materialId;
    if (materialId == UINT32_MAX)
      continue;
    ObjectSubmitDesc object =
        BuildStandardObjectDesc(o, mat, apFrustum, materialId);
    const hash_t cookie =
        hash_u32(hash_u64(HASH_INITIAL_VALUE, o->GetUniqueCookie()), paneSalt);
    uint32_t slot = mpGraphics->globalset->submitObject(
        cookie, static_cast<uint32_t>(mpGraphics->frameIndex), vb, object,
        kSubmitData | kSubmitVertex | kSubmitIndex);
    if (slot == UINT32_MAX)
      continue;
    submittedSlots.emplace(o, slot);
    const uint32_t vertexCount =
        vb->GetIndexNum() > 0 ? static_cast<uint32_t>(vb->GetIndexNum())
                              : static_cast<uint32_t>(vb->GetVertexNum());
    if (cameraCullReady) {
      cBoundingVolume *bounds = o->GetBoundingVolume();
      if (bounds) {
        const cVector3f boundsMin = bounds->GetMin();
        const cVector3f boundsMax = bounds->GetMax();
        StandardCullCandidate candidate{};
        candidate.aabbMinX = boundsMin.x;
        candidate.aabbMinY = boundsMin.y;
        candidate.aabbMinZ = boundsMin.z;
        candidate.aabbMaxX = boundsMax.x;
        candidate.aabbMaxY = boundsMax.y;
        candidate.aabbMaxZ = boundsMax.z;
        candidate.objectSlot = slot;
        candidate.vertexCount = vertexCount;
        // The shared predicate gates on the caster bit and the variability
        // mask before the frustum test; neither means anything for a camera
        // tile, so the host sets the bits that let every candidate through.
        candidate.renderFlags = kStandardCullShadowCasterBit |
                                (o->IsStatic() ? kStandardCullStaticBit : 0u);
        // NOT salted by pane: the table is persistent and shared, so a
        // renderable should land on the same slot every frame from every
        // camera. Collisions are benign by construction.
        candidate.visibilityKey =
            static_cast<uint32_t>(
                hash_u64(HASH_INITIAL_VALUE, o->GetUniqueCookie())) %
            kStandardCullVisibilityKeys;
        candidate.commandWordOffset =
            static_cast<uint32_t>(
                (req.elementOffset + drawCount) *
                (sizeof(VkDrawIndirectCommand) / sizeof(uint32_t))) +
            1u;  // instanceCount
        cameraCandidates[drawCount] = candidate;
        // Identical geometry in both ranges; only instanceCount differs, and
        // the kernel owns that.
        phaseTwoIndirect[drawCount] = {vertexCount, 1, 0, slot};
      } else {
        // No bounds means nothing to test against. Give it a box that swallows
        // any frustum and exempt it from occlusion, so the frustum test keeps
        // it and one of the two phases always draws it. A zeroed candidate
        // would fail the caster gate and be dropped by BOTH phases.
        StandardCullCandidate candidate{};
        const float huge = 3.0e38f;
        candidate.aabbMinX = candidate.aabbMinY = candidate.aabbMinZ = -huge;
        candidate.aabbMaxX = candidate.aabbMaxY = candidate.aabbMaxZ = huge;
        candidate.objectSlot = slot;
        candidate.vertexCount = vertexCount;
        candidate.renderFlags = kStandardCullShadowCasterBit |
                                (o->IsStatic() ? kStandardCullStaticBit : 0u);
        candidate.cullFlags = kStandardCullFlagNeverOcclude;
        candidate.visibilityKey =
            static_cast<uint32_t>(
                hash_u64(HASH_INITIAL_VALUE, o->GetUniqueCookie())) %
            kStandardCullVisibilityKeys;
        candidate.commandWordOffset =
            static_cast<uint32_t>(
                (req.elementOffset + drawCount) *
                (sizeof(VkDrawIndirectCommand) / sizeof(uint32_t))) +
            1u;
        cameraCandidates[drawCount] = candidate;
        phaseTwoIndirect[drawCount] = {vertexCount, 1, 0, slot};
      }
    }
    indirect[drawCount++] = {vertexCount, 1, 0, slot};
  }
  // Every command for this Draw is now written on the host. Publish them to the
  // device buffer once, here, rather than at each consumer: both the two-phase
  // cull (which rewrites instanceCount) and the plain drawIndirect below read
  // the device copy, and only one of the two paths runs. A no-op on Vulkan.
  if (drawCount > 0) {
    constexpr uint64_t kStride = sizeof(VkDrawIndirectCommand);
    const uint64_t first =
        cameraCullReady ? std::min(req.elementOffset, phaseTwoReq.elementOffset)
                        : req.elementOffset;
    const uint64_t last =
        (cameraCullReady
             ? std::max(req.elementOffset, phaseTwoReq.elementOffset)
             : req.elementOffset) +
        drawCount;
    m_indirectDrawBuffer.Flush(&mpGraphics->device,
                               &mpGraphics->primary.cmds[0], first * kStride,
                               (last - first) * kStride, m_indirectDrawFirstUse,
                               /*cullFollows*/ cameraCullReady);
    m_indirectDrawFirstUse = false;
  }
  mpGraphics->globalset->flushMirrors(&mpGraphics->device);

  // gPerFrame (set 1) is needed before any Standard program draws: the shadow
  // raster's material alpha test reads it through the bindless animated-texture
  // lookup. Building it is CPU-only, so nothing is recorded yet on failure.
  // Presented extent; equal to the render extent unless a temporal provider is
  // upscaling. Only the material mip bias consumes it, and only a prepared
  // provider earns it: DevRenderScale is a plain stretch with no jittered
  // subpixel accumulation to pay for the vendor formula's -1.0, so it reports
  // no display extent and biases nothing (same rule as Hybrid).
  const cVector2l displayExtent = viewport->GetDisplayExtent();
  const bool temporalProviderPrepared = viewport->IsTemporalProviderPrepared();
  const uint32_t biasDisplayExtentX =
      temporalProviderPrepared && displayExtent.x > 0
          ? static_cast<uint32_t>(displayExtent.x)
          : 0u;
  const uint32_t biasDisplayExtentY =
      temporalProviderPrepared && displayExtent.y > 0
          ? static_cast<uint32_t>(displayExtent.y)
          : 0u;

  RIProgram::DescriptorBinding frameBinding;
  if (!m_environment ||
      !m_environment->PrepareFrame(
          cntx, apFrustum, state->width, state->height, GetTimeCount(), apWorld,
          m_rendererList.GetFogAreas(),
          apSettings ? apSettings->mClearColor : cColor(0, 0), &frameBinding,
          &temporalFrame, biasDisplayExtentX, biasDisplayExtentY))
    return;

  // Pack this Draw's shadow tiles into atlas pages before the main render
  // begins. Each tile gets its own frustum, caster list and indirect range. A
  // light is published only when every one of its tiles was prepared and all
  // pages rendered; tile records follow publish order, so a point light's six
  // faces are contiguous.
  RISharedPointer<RITexture> standardShadowTexture;
  RISharedPointer<RITextureView> standardShadowView;
  std::vector<StandardShadowTileData> shadowTileRecords;
  std::vector<StandardShadowLightTiles> shadowLightTiles;
  if (!shadowSelection.empty() && m_shadow && m_shadowLoaded &&
      !m_shadowIndirectBuffer.isEmpty() &&
      (!apSettings || apSettings->mbRenderShadows)) {
    const StandardShadowAtlasConfig atlasConfig{
        kStandardShadowAtlasCapTiles *
            StandardShadowResolution(shadowResolutionCap),
        kStandardShadowMaxAtlases, kStandardShadowMinTile};
    std::vector<StandardShadowTileRequest> requests;
    for (uint32_t owner = 0; owner < shadowSelection.size(); ++owner) {
      const uint32_t faces =
          shadowSelection[owner].point ? kStandardShadowCubeFaces : 1u;
      for (uint32_t face = 0; face < faces; ++face)
        requests.push_back({shadowSelection[owner].size, owner, face});
    }
    StandardShadowFitBudget(atlasConfig, requests);
    uint32_t atlasCount = 0;
    const std::vector<StandardShadowTilePlacement> placements =
        StandardShadowPackAtlases(atlasConfig, requests, atlasCount);
    // One persistent page array instead of new depth images every Draw: each
    // page is barriered from UNDEFINED before it is rendered, so reuse needs no
    // layout bookkeeping. It grows when a Draw packs more pages.
    if (atlasCount > 0 && (m_shadowAtlas.isEmpty() ||
                           m_shadowAtlasSize != atlasConfig.atlasSize ||
                           m_shadowAtlasLayers < atlasCount)) {
      if (!m_shadowAtlas.isEmpty())
        mpGraphics->graphicsDefer.push(m_shadowAtlas);
      m_shadowAtlas = {};
      m_shadowAtlasSize = 0;
      m_shadowAtlasLayers = 0;
      RITextureDesc td{};
      td.type = RI_TEXTURE_2D;
      td.format = cStandardShadowPass::kAtlasFormat;
      td.width = atlasConfig.atlasSize;
      td.height = atlasConfig.atlasSize;
      td.depth = 1;
      td.layerNum = atlasCount;
      td.mipNum = 1;
      td.sampleCount = 1;
      td.usage = RI_USAGE_DEPTH_STENCIL_ATTACHMENT | RI_USAGE_SHADER_RESOURCE;
      RITexture t = RITexture::create(&mpGraphics->device, td);
      m_shadowAtlas = RISharedPointer<RITexture>(&mpGraphics->device, t);
      if (!m_shadowAtlas.isEmpty()) {
        NameStandardImage(mpGraphics, *m_shadowAtlas,
                          "StandardRenderer.shadowAtlas");
        m_shadowAtlasSize = atlasConfig.atlasSize;
        m_shadowAtlasLayers = atlasCount;
      }
    }
    if (atlasCount > 0 && !m_shadowAtlas.isEmpty()) {
      standardShadowTexture = m_shadowAtlas;
      struct PreparedTile {
        uint32_t atlas;
        cStandardShadowPass::Tile raster;
        StandardShadowTileData record;
      };
      struct PreparedLight {
        iLight *light;
        uint32_t size;
        std::vector<PreparedTile> tiles;
      };
      std::vector<PreparedLight> prepared;
      // Live prefix lengths of the cull ring buffers for this Draw, and the
      // reusable per-light candidate index list.
      // Ring-segment ranges claimed this Draw. Tiles and groups are each
      // handed out contiguously, so a base plus a count describes them; the
      // kernel must not iterate from zero, which would touch entries still in
      // use by frames in flight.
      // Tiles and groups are staged on the host and published as ONE ring
      // request each after the light loop. The cull dispatch addresses them as
      // [base, count) ranges, and RISegmentAlloc::request restarts at offset 0
      // when a request does not fit the tail of the ring -- so requesting them
      // one at a time could split a single frame's entries across the wrap,
      // which a base+count range cannot express. One request per frame makes
      // contiguity an invariant instead of an assumption. Groups carry a
      // staging-relative tileIndex until they land.
      std::vector<StandardCullTile> cullTiles;
      std::vector<StandardCullGroup> cullGroups;
      std::vector<size_t> lightCandidates;
      const float globalSlope =
          apSettings ? apSettings->mfShadowMapSlopeScaleBias : 2.0f;

      // ---------------------------------------------------------------
      // Frame-global shadow caster gather.
      //
      // Replaces the two WalkAndPrepareRenderList calls this loop used to
      // make PER TILE. Every caster is found once, submitted once, and then
      // referenced by whichever tiles need it; the per-tile frustum test moves
      // to the GPU. GetModelMatrix does not vary per tile for anything that can
      // pass the caster filter, which is what makes the shared submit sound --
      // see the renderable-type filter below for the exceptions.
      // ---------------------------------------------------------------
      struct ShadowCaster {
        iRenderable *object;
        uint32_t slot;
        uint32_t vertexCount;
        uint32_t renderFlags;
        cVector3f boundsMin;
        cVector3f boundsMax;
      };
      std::vector<ShadowCaster> shadowCasters;
      // Bounds of casters that could not be submitted. A light overlapping any
      // of these fails wholesale rather than rendering a tile with a silently
      // missing shadow -- the same guarantee the old per-tile code gave by
      // failing the light when a submit failed inside its caster list.
      std::vector<std::pair<cVector3f, cVector3f>> failedCasterBounds;
      bool shadowGatherOverflow = false;
      {
        // Union of every surviving light's reach. Requests are already limited
        // to the lights that fit the atlas budget.
        bool haveBounds = false;
        cVector3f gatherMin(0.0f), gatherMax(0.0f);
        for (const StandardShadowTileRequest &request : requests) {
          iLight *light = shadowSelection[request.owner].light;
          if (!light)
            continue;
          const cVector3f position = light->GetWorldPosition();
          const float reach = light->GetRadius();
          const cVector3f lightMin = position - cVector3f(reach);
          const cVector3f lightMax = position + cVector3f(reach);
          if (!haveBounds) {
            gatherMin = lightMin;
            gatherMax = lightMax;
            haveBounds = true;
          } else {
            gatherMin = cMath::Vector3Min(gatherMin, lightMin);
            gatherMax = cMath::Vector3Max(gatherMax, lightMax);
          }
        }

        if (haveBounds) {
          const auto gatherCaster = [&](iRenderable *o) {
            if (shadowGatherOverflow)
              return;
            if (!o || !rendering::IsObjectIsVisible(
                          o, eRenderableFlag_ShadowCaster, {}))
              return;
            cMaterial *mat = o->GetMaterial();
            if (!o->GetVertexBuffer() || !mat ||
                cMaterial::IsTranslucent(mat->GetMaterialID()))
              return;
            // Billboards, beams and particle emitters orient themselves to the
            // frustum they are given, so they cannot share one object slot
            // across tiles. They are already excluded in practice by the
            // translucency test above; rejecting them by type makes that
            // structural rather than incidental. A non-translucent one would
            // previously have cast a light-oriented shadow and now casts none.
            const eRenderableType type = o->GetRenderType();
            if (type == eRenderableType_Billboard ||
                type == eRenderableType_Beam ||
                type == eRenderableType_ParticleEmitter)
              return;

            cBoundingVolume *bv = o->GetBoundingVolume();
            if (!bv)
              return;

            cVertexBuffer *vb = static_cast<cVertexBuffer *>(o->GetVertexBuffer());
            uint32_t slot = UINT32_MAX;
            const auto existing = submittedSlots.find(o);
            if (existing != submittedSlots.end()) {
              // Already uploaded for the camera this frame.
              slot = existing->second;
            } else {
              // Shadow-only caster (typically behind the camera): the camera
              // list never saw it, so its geometry may not be resident.
              vb->SubmitToGPU(&mpGraphics->device);
              const uint32_t materialId =
                  mpGraphics->globalset
                      ->submitMaterial(
                          cntx, mat,
                          static_cast<uint32_t>(mpGraphics->frameIndex))
                      .materialId;
              if (materialId != UINT32_MAX) {
                const ObjectSubmitDesc object =
                    BuildStandardObjectDesc(o, mat, apFrustum, materialId);
                const hash_t cookie = hash_u32(
                    hash_u64(HASH_INITIAL_VALUE, o->GetUniqueCookie()), paneSalt);
                slot = mpGraphics->globalset->submitObject(
                    cookie, static_cast<uint32_t>(mpGraphics->frameIndex), vb,
                    object, kSubmitData | kSubmitVertex | kSubmitIndex);
              }
              if (slot == UINT32_MAX) {
                failedCasterBounds.emplace_back(bv->GetMin(), bv->GetMax());
                return;
              }
              submittedSlots.emplace(o, slot);
            }

            if (shadowCasters.size() >= kStandardShadowMaxCandidates) {
              shadowGatherOverflow = true;
              return;
            }
            ShadowCaster caster{};
            caster.object = o;
            caster.slot = slot;
            caster.vertexCount =
                vb->GetIndexNum() > 0 ? static_cast<uint32_t>(vb->GetIndexNum())
                                      : static_cast<uint32_t>(vb->GetVertexNum());
            caster.renderFlags =
                o->GetRenderFlags() |
                (o->IsStatic() ? kStandardCullStaticBit : 0u);
            caster.boundsMin = bv->GetMin();
            caster.boundsMax = bv->GetMax();
            shadowCasters.push_back(caster);
          };
          dynamicContainer->QueryAabb(gatherMin, gatherMax, gatherCaster);
          staticContainer->QueryAabb(gatherMin, gatherMax, gatherCaster);
        }
        mpGraphics->globalset->flushMirrors(&mpGraphics->device);
      }
      // FitBudget keeps each light's requests contiguous and in face order.
      size_t cursor = 0;
      while (cursor < requests.size()) {
        const uint32_t owner = requests[cursor].owner;
        const size_t first = cursor;
        while (cursor < requests.size() && requests[cursor].owner == owner)
          ++cursor;
        const size_t end = cursor;
        const ShadowSelection &selected = shadowSelection[owner];
        iLight *light = selected.light;
        bool lightFailed =
            end - first != (selected.point ? kStandardShadowCubeFaces : 1u);
        for (size_t i = first; i < end && !lightFailed; ++i)
          lightFailed =
              !placements[i].IsValid() || requests[i].face != i - first;
        if (lightFailed)
          continue;

        const uint32_t variabilityMask =
            static_cast<uint32_t>(light->GetShadowCastersAffected());
        const float slopeScaleBias =
            StandardFinite(globalSlope)
                ? globalSlope * light->GetShadowMapSlopeScaleBiasMul()
                : 0.0f;
        const float authoredBias = light->GetShadowMapBiasMul();
        const float depthBias =
            (StandardFinite(authoredBias) && authoredBias >= 0.0f)
                ? 0.0005f * authoredBias
                : 0.0f;
        const float radius = light->GetRadius();
        // Object slots are now shared across every light AND with the camera
        // pass, so the per-light salt is gone: one caster means one slot per
        // frame. Both sets were already refreshed once before the camera walk,
        // so the per-light UpdateBeforeRendering pair is gone too.

        // This light's slice of the frame-global caster set. The GPU does the
        // per-tile frustum test; this only narrows to what the light can reach
        // at all, which is a sphere test rather than a container walk.
        lightCandidates.clear();
        const cVector3f lightPosition = light->GetWorldPosition();
        for (size_t c = 0; c < shadowCasters.size(); ++c) {
          const ShadowCaster &caster = shadowCasters[c];
          if (StandardSphereOverlapsAabb(
                  lightPosition.x, lightPosition.y, lightPosition.z, radius,
                  caster.boundsMin.x, caster.boundsMin.y, caster.boundsMin.z,
                  caster.boundsMax.x, caster.boundsMax.y, caster.boundsMax.z)) {
            lightCandidates.push_back(c);
          }
        }
        if (lightCandidates.size() > kStandardShadowMaxCandidatesPerLight)
          continue;
        // A caster that failed to submit is missing from the candidate set. If
        // it could have lit this light's tiles, the light must not publish a
        // shadow that silently omits it.
        bool overlapsFailedCaster = false;
        for (const auto &bounds : failedCasterBounds) {
          if (StandardSphereOverlapsAabb(
                  lightPosition.x, lightPosition.y, lightPosition.z, radius,
                  bounds.first.x, bounds.first.y, bounds.first.z,
                  bounds.second.x, bounds.second.y, bounds.second.z)) {
            overlapsFailedCaster = true;
            break;
          }
        }
        if (overlapsFailedCaster || shadowGatherOverflow)
          continue;

        // One candidate range per LIGHT, not per tile. Every tile of a light
        // tests the same caster list -- the six faces of a point light differ
        // only in their frustum -- and StandardCullTile carries its own
        // candidateBase, so the faces share one range. Allocating per face took
        // six times the candidate ring for a point light, which is what made
        // lights fail to publish a shadow once the ring ran dry.
        RISegmentReq candidateReq = {};
        const uint32_t candidateCount =
            static_cast<uint32_t>(lightCandidates.size());
        if (candidateCount > 0) {
          if (!m_shadowCandidateSegment.request(mpGraphics->frameIndex,
                                                candidateCount, &candidateReq))
            continue;
          auto *candidateSlots = reinterpret_cast<StandardCullCandidate *>(
              static_cast<uint8_t *>(m_shadowCandidateBuffer.mappedAddress) +
              candidateReq.elementOffset * sizeof(StandardCullCandidate));
          for (uint32_t c = 0; c < candidateCount; ++c) {
            const ShadowCaster &caster = shadowCasters[lightCandidates[c]];
            StandardCullCandidate record{};
            record.aabbMinX = caster.boundsMin.x;
            record.aabbMinY = caster.boundsMin.y;
            record.aabbMinZ = caster.boundsMin.z;
            record.aabbMaxX = caster.boundsMax.x;
            record.aabbMaxY = caster.boundsMax.y;
            record.aabbMaxZ = caster.boundsMax.z;
            record.objectSlot = caster.slot;
            record.vertexCount = caster.vertexCount;
            record.renderFlags = caster.renderFlags;
            candidateSlots[c] = record;
          }
        }

        PreparedLight entry{light, placements[first].size, {}};
        bool anyCaster = false;
        for (size_t i = first; i < end && !lightFailed; ++i) {
          const StandardShadowTilePlacement &placement = placements[i];
          float nearClip, fov, aspect;
          cMatrixf view;
          if (selected.point) {
            nearClip = StandardPointShadowNear(radius);
            const float edge =
                static_cast<float>(placement.size) /
                std::max(static_cast<float>(placement.size) -
                             2.0f * kStandardShadowCubeBorderTexels,
                         1.0f);
            fov = 2.0f * std::atan(edge);
            aspect = 1.0f;
            view = StandardCubeFaceView(light->GetWorldPosition(),
                                        static_cast<uint32_t>(i - first));
          } else {
            iLightSpot *spot = static_cast<iLightSpot *>(light);
            nearClip = spot->GetNearClipPlane();
            fov = spot->GetFOV();
            aspect = spot->GetAspect();
            view = spot->GetViewMatrix();
          }
          const cMatrixf projection = cMath::MatrixPerspectiveProjection(
              nearClip, radius, fov, aspect, false);
          const cMatrixf vp = cMath::MatrixMul(projection, view);
          cFrustum tileFrustum;
          tileFrustum.SetupPerspectiveProj(projection, view, radius, nearClip,
                                           fov, aspect,
                                           light->GetWorldPosition(), false);
          PreparedTile tile{};
          tile.atlas = placement.atlas;
          tile.raster.x = placement.x;
          tile.raster.y = placement.y;
          tile.raster.size = placement.size;
          tile.raster.variabilityMask = variabilityMask;
          tile.raster.slopeScaleBias = slopeScaleBias;
          const ml::float4x4 vpData = cMath::ToFloatTranspose4x4(vp);
          std::memcpy(tile.raster.viewProjection, vpData.a,
                      sizeof(tile.raster.viewProjection));
          std::memcpy(tile.record.viewProjection, vpData.a,
                      sizeof(tile.record.viewProjection));
          tile.record.atlasLayer = placement.atlas;
          tile.record.originX = placement.x;
          tile.record.originY = placement.y;
          tile.record.size = placement.size;
          tile.record.bias = depthBias;
          tile.record.clampToTile = selected.point ? 1u : 0u;
          // PCSS: this tile's own frustum, so the filter can linearize the
          // depths it reads, plus the emitter size that sets how fast the
          // penumbra opens. Both light types get the same treatment -- a spot
          // and a point at the same distance from the same occluder should
          // soften identically.
          tile.record.nearPlane = nearClip;
          tile.record.farPlane = radius;
          tile.record.tanHalfFov = std::tan(fov * 0.5f);
          tile.record.lightSize = kStandardShadowLightSize;

          // A cube face whose frustum misses the camera has no visible
          // receiver: its cleared (fully lit) tile needs no casters. Six cheap
          // frustum-frustum tests per point light, so this stays on the host.
          const bool faceVisible =
              !selected.point ||
              apFrustum->CollideFrustum(&tileFrustum) != eCollision_Outside;

          if (faceVisible && candidateCount > 0) {
            // Reserve the worst case: every candidate this light can see. The
            // survivor count is only known on the GPU, so the reservation has
            // to be an upper bound -- which also makes overrunning it
            // impossible, replacing the old post-hoc caster-count check with an
            // invariant.
            RISegmentReq indirectReq = {};
            RISegmentReq countReq = {};
            const uint32_t groupCount =
                (candidateCount + kStandardCullGroupSize - 1u) /
                kStandardCullGroupSize;
            // Tiles and groups are only staged here, so the capacity they will
            // be published into has to be checked by hand. The indirect and
            // count ranges are per tile and absolute, so they may wrap -- but
            // only within the space no in-flight frame still holds, which is
            // why their rings are sized for kStandardShadowRingFrames. A
            // request that fails here silently costs this light its shadow.
            if (cullTiles.size() + 1u > kStandardShadowMaxTiles ||
                cullGroups.size() + groupCount > kStandardShadowMaxCullGroups ||
                !m_shadowIndirectSegment.request(mpGraphics->frameIndex,
                                                 candidateCount, &indirectReq) ||
                !m_shadowDrawCountSegment.request(mpGraphics->frameIndex, 1,
                                                  &countReq)) {
              lightFailed = true;
              break;
            }

            StandardCullTile cullTile{};
            // Extract from the row-major cMatrixf, NOT from
            // tile.raster.viewProjection: that field holds the transposed dump
            // the raster push-constants want, and feeding it here would produce
            // plausible-looking but wrong planes. cMatrixf::v aliases m[row][col],
            // which is the layout StandardExtractFrustumPlanes expects and the
            // one its MathLib equivalence test is written against.
            //
            // The cube-face widening is already baked into vp, so the extracted
            // planes inherit it with no special case.
            StandardExtractFrustumPlanes(vp.v, cullTile.planes);
            cullTile.planeCount = 6u;
            cullTile.candidateBase =
                static_cast<uint32_t>(candidateReq.elementOffset);
            cullTile.candidateCount = candidateCount;
            cullTile.indirectBase =
                static_cast<uint32_t>(indirectReq.elementOffset);
            cullTile.countIndex = static_cast<uint32_t>(countReq.elementOffset);
            cullTile.variabilityMask = variabilityMask;
            // Shadow tiles frustum-test only. Leaving this zero would aim the
            // occlusion test at camera record 0.
            cullTile.cameraIndex = kStandardCullNoCamera;

            const uint32_t stagedTile = static_cast<uint32_t>(cullTiles.size());
            cullTiles.push_back(cullTile);
            for (uint32_t g = 0; g < groupCount; ++g) {
              StandardCullGroup group{};
              group.tileIndex = stagedTile; // patched to absolute on publish
              group.candidateOffset = g * kStandardCullGroupSize;
              cullGroups.push_back(group);
            }

            tile.raster.indirectOffset =
                indirectReq.elementOffset * sizeof(VkDrawIndirectCommand);
            tile.raster.drawCountOffset =
                countReq.elementOffset * sizeof(uint32_t);
            tile.raster.maxDrawCount = candidateCount;
            anyCaster = true;
          }
          entry.tiles.push_back(tile);
        }
        // A light without casters in any tile is fully lit: no shadow to publish.
        if (!lightFailed && anyCaster)
          prepared.push_back(std::move(entry));
      }
      mpGraphics->globalset->flushMirrors(&mpGraphics->device);

      // Publish the staged tiles and groups as one contiguous range each, and
      // patch every group's staging-relative tileIndex into the absolute
      // element index the kernel indexes gShadowCullTiles with.
      uint32_t cullTileBase = 0;
      uint32_t cullGroupBase = 0;
      bool cullRangesValid = cullTiles.empty();
      if (!cullTiles.empty()) {
        RISegmentReq tileReq = {};
        RISegmentReq groupReq = {};
        if (m_shadowCullTileSegment.request(mpGraphics->frameIndex,
                                            cullTiles.size(), &tileReq) &&
            m_shadowCullGroupSegment.request(mpGraphics->frameIndex,
                                             cullGroups.size(), &groupReq)) {
          cullTileBase = static_cast<uint32_t>(tileReq.elementOffset);
          cullGroupBase = static_cast<uint32_t>(groupReq.elementOffset);
          auto *tileSlots = reinterpret_cast<StandardCullTile *>(
              static_cast<uint8_t *>(m_shadowCullTileBuffer.mappedAddress) +
              tileReq.elementOffset * sizeof(StandardCullTile));
          std::memcpy(tileSlots, cullTiles.data(),
                      cullTiles.size() * sizeof(StandardCullTile));
          auto *groupSlots = reinterpret_cast<StandardCullGroup *>(
              static_cast<uint8_t *>(m_shadowCullGroupBuffer.mappedAddress) +
              groupReq.elementOffset * sizeof(StandardCullGroup));
          for (size_t g = 0; g < cullGroups.size(); ++g) {
            StandardCullGroup group = cullGroups[g];
            group.tileIndex += cullTileBase;
            groupSlots[g] = group;
          }
          cullRangesValid = true;
        }
      }

      // Cull every tile of every light in two dispatches, then hand the
      // compacted commands and their counts to the raster below. Must be
      // recorded on the same command buffer RenderAtlas draws from.
      bool cullRecorded = false;
      if (!prepared.empty() && cullRangesValid && m_shadowCull &&
          m_shadowCull->IsLoaded()) {
        cStandardShadowCullPass::Buffers cullBuffers{};
        cullBuffers.candidates = &m_shadowCandidateBuffer;
        cullBuffers.tiles = &m_shadowCullTileBuffer;
        cullBuffers.groups = &m_shadowCullGroupBuffer;
        cullBuffers.indirect = m_shadowIndirectBuffer.gpu();
        cullBuffers.drawCounts = &m_shadowDrawCountBuffer;
        cullBuffers.candidateCapacity = kStandardShadowCandidateRing;
        cullBuffers.indirectCapacity = kStandardShadowIndirectRing;
        cullBuffers.tileCapacity = kStandardShadowTileRing;
        cullBuffers.groupCapacity = kStandardShadowCullGroupRing;
        cullBuffers.drawCountCapacity = kStandardShadowTileRing;
        cullBuffers.cameras = &m_cullCameraBuffer;
        cullBuffers.cameraCapacity = kStandardCullMaxCameras;
        cullBuffers.visibility = &m_cullVisibilityBuffer;
        cullBuffers.visibilityCapacity = kStandardCullVisibilityKeys;
        cullRecorded = m_shadowCull->Dispatch(
            &mpGraphics->primary.cmds[0], mpGraphics->frameIndex, cullBuffers,
            cullTileBase, static_cast<uint32_t>(cullTiles.size()),
            cullGroupBase, static_cast<uint32_t>(cullGroups.size()));
      }
      // Without a cull dispatch the indirect range holds stale commands, so
      // publishing the tiles would draw last frame's casters.
      if (!cullRecorded)
        prepared.clear();

      // Rasterize every packed page, including pages whose lights all failed,
      // so the array view only spans SHADER_RESOURCE pages.
      bool pagesRendered = !prepared.empty();
      std::vector<cStandardShadowPass::Tile> pageTiles;
      for (uint32_t page = 0; page < atlasCount && pagesRendered; ++page) {
        pageTiles.clear();
        for (const PreparedLight &entry : prepared)
          for (const PreparedTile &tile : entry.tiles)
            if (tile.atlas == page)
              pageTiles.push_back(tile.raster);
        pagesRendered = m_shadow->RenderAtlas(
            cntx, &mpGraphics->primary.cmds[0], mpGraphics->frameIndex,
            standardShadowTexture.Get(), page, atlasConfig.atlasSize,
            m_shadowIndirectBuffer.gpu(),
            m_shadowCull->UsesDrawIndirectCount() ? &m_shadowDrawCountBuffer
                                                  : nullptr,
            pageTiles, frameBinding);
      }
      if (pagesRendered) {
        for (const PreparedLight &entry : prepared) {
          shadowLightTiles.push_back(
              {entry.light, static_cast<uint32_t>(shadowTileRecords.size()),
               entry.size});
          for (const PreparedTile &tile : entry.tiles)
            shadowTileRecords.push_back(tile.record);
        }
        RITextureViewDesc vd{};
        vd.viewType = RI_VIEWTYPE_SHADER_RESOURCE_2D_ARRAY;
        vd.format = cStandardShadowPass::kAtlasFormat;
        vd.mipNum = 1;
        vd.layerNum = atlasCount;
        RITextureView v = RITextureView::create(
            &mpGraphics->device, standardShadowTexture.Get(), vd);
        standardShadowView =
            RISharedPointer<RITextureView>(&mpGraphics->device, v);
        if (!standardShadowView.isEmpty()) {
          mpGraphics->graphicsDefer.push(standardShadowTexture);
          mpGraphics->graphicsDefer.push(standardShadowView);
        }
      }
    }
  }
  if (standardShadowTexture.isEmpty() || standardShadowView.isEmpty()) {
    // A failed array view invalidates every index into that array.
    shadowTileRecords.clear();
    shadowLightTiles.clear();
    if (!standardShadowView.isEmpty())
      mpGraphics->graphicsDefer.push(standardShadowView);
    if (!standardShadowTexture.isEmpty())
      mpGraphics->graphicsDefer.push(standardShadowTexture);
    standardShadowView = {};
    standardShadowTexture = {};
    // The cleared 1x1 page is created once and stays sampled across Draws.
    bool fallbackReady =
        !m_shadowFallback.isEmpty() && !m_shadowFallbackView.isEmpty();
    if (!fallbackReady) {
      if (!m_shadowFallbackView.isEmpty())
        mpGraphics->graphicsDefer.push(m_shadowFallbackView);
      if (!m_shadowFallback.isEmpty())
        mpGraphics->graphicsDefer.push(m_shadowFallback);
      m_shadowFallbackView = {};
      m_shadowFallback = {};
      RITextureDesc td{};
      td.type = RI_TEXTURE_2D;
      td.format = cGraphics::DepthFormat;
      td.width = 1;
      td.height = 1;
      td.depth = 1;
      td.layerNum = 1;
      td.mipNum = 1;
      td.sampleCount = 1;
      td.usage = RI_USAGE_DEPTH_STENCIL_ATTACHMENT | RI_USAGE_SHADER_RESOURCE;
      RITexture t = RITexture::create(&mpGraphics->device, td);
      m_shadowFallback = RISharedPointer<RITexture>(&mpGraphics->device, t);
      if (!m_shadowFallback.isEmpty()) {
        NameStandardImage(mpGraphics, *m_shadowFallback,
                          "StandardRenderer.spotShadowFallback");
        RITextureViewDesc vd{};
        vd.viewType = RI_VIEWTYPE_SHADER_RESOURCE_2D_ARRAY;
        vd.format = cGraphics::DepthFormat;
        vd.mipNum = 1;
        vd.layerNum = 1;
        RITextureView v = RITextureView::create(&mpGraphics->device,
                                                m_shadowFallback.Get(), vd);
        m_shadowFallbackView =
            RISharedPointer<RITextureView>(&mpGraphics->device, v);
      }
      fallbackReady =
          !m_shadowFallback.isEmpty() && !m_shadowFallbackView.isEmpty() &&
          ClearStandardShadowFallback(mpGraphics, &mpGraphics->primary.cmds[0],
                                      m_shadowFallback.Get());
      if (!fallbackReady) {
        // The command buffer may already reference the image after the first barrier.
        if (!m_shadowFallbackView.isEmpty())
          mpGraphics->graphicsDefer.push(m_shadowFallbackView);
        if (!m_shadowFallback.isEmpty())
          mpGraphics->graphicsDefer.push(m_shadowFallback);
        m_shadowFallbackView = {};
        m_shadowFallback = {};
      }
    }
    standardShadowTexture = m_shadowFallback;
    standardShadowView = m_shadowFallbackView;
    // Defer both resources even when clearing or view creation failed: the
    // command buffer may already reference the image after the first barrier.
    if (!standardShadowView.isEmpty())
      mpGraphics->graphicsDefer.push(standardShadowView);
    if (!standardShadowTexture.isEmpty())
      mpGraphics->graphicsDefer.push(standardShadowTexture);
    if (!fallbackReady)
      return;
  }
  if (standardShadowTexture.isEmpty() || standardShadowView.isEmpty())
    return;

  // Publish tile and light records only after their pages have been
  // successfully recorded. This keeps shadowIndex and tile records in lockstep.
  RISharedPointer<RIBuffer> shadowTiles;
  size_t shadowTileCapacity = 0;
  if (!UploadStandardLights(mpGraphics, shadowTileRecords, shadowTiles,
                            shadowTileCapacity)) {
    shadowTileRecords.clear();
    shadowLightTiles.clear();
  }
  if (shadowTiles.isEmpty())
    return;
  shadowCount = 0;
  // A null frustum makes BuildStandardLights put every enabled light in the
  // visible prefix -- the HPL_STANDARD_LIGHT_CULL=0 A/B path.
  if (!BuildStandardLights(apWorld, mpGraphics,
                            apFrustum, pointLights,
                           spotLights, boxLights, pointLightCount,
                           spotLightCount, boxLightCount, pointLightCountTotal,
                           spotLightCountTotal, shadowCount,
                           shadowResolutionCap, shadowsAvailable,
                           shadowLightTiles))
    return;

  const bool packedVisibility =
      !m_forceFallback && mpGraphics->device.fragmentShaderBarycentricEnabled &&
      mpGraphics->device.shaderInt16Enabled &&
      mpGraphics->device.shaderFloat16Enabled &&
      mpGraphics->device.geometryShaderEnabled && m_visibility &&
      m_visibilityLoaded && m_reconstruct && m_reconstructLoaded &&
      !state->visibilityTexture[index].isEmpty() &&
      !state->visibilityView[index].isEmpty() &&
      !state->visibilityAttachmentView[index].isEmpty();
  RITextureBarrier barriers[8] = {};
  barriers[0] =
      RI_PogoAttachmentBarrier(state->renderTarget[index].Get(), true);
  barriers[1].texture = state->depthTextures[index].Get();
  barriers[1].before = RI_RESOURCE_STATE_UNDEFINED;
  barriers[1].after = RI_RESOURCE_STATE_DEPTH_WRITE;
  barriers[1].aspect = RI_BARRIER_ASPECT_DEPTH;
  barriers[1].mipCount = 1;
  barriers[1].layerCount = 1;
  uint32_t barrierCount = 2;
  auto appendColorBarrier = [&](RITexture *texture) {
    RITextureBarrier b(texture, RI_RESOURCE_STATE_UNDEFINED,
                       RI_RESOURCE_STATE_RENDER_TARGET, RI_STAGE_NONE,
                       RI_STAGE_FRAGMENT, RI_BARRIER_ASPECT_COLOR);
    b.mipCount = 1;
    b.layerCount = 1;
    assert(barrierCount < std::size(barriers));
    barriers[barrierCount++] = b;
  };
  // The MRT images are newly allocated and must be made attachment
  // images before either raster fallback or fullscreen reconstruction uses them.
  if (!packedVisibility) {
    appendColorBarrier(state->materialColorTexture[index].Get());
    appendColorBarrier(state->positionTexture[index].Get());
    appendColorBarrier(state->normalTexture[index].Get());
    appendColorBarrier(state->shadingNormalTexture[index].Get());
    appendColorBarrier(state->surfaceTexture[index].Get());
  }
  if (packedVisibility && !state->visibilityTexture[index].isEmpty()) {
    appendColorBarrier(state->visibilityTexture[index].Get());
    appendColorBarrier(state->materialColorTexture[index].Get());
  }
  appendColorBarrier(state->velocityTexture[index].Get());
  assert(barrierCount == (packedVisibility ? 5u : 8u));
  for (uint32_t i = 0; i < barrierCount; ++i)
    assert(barriers[i].texture != nullptr);
  mpGraphics->primary.cmds[0].vk_d3d12_textureBarriers<8>(barrierCount,
                                                          barriers);

  RIRenderingAttachment color = {};
  color.view = *state->renderTargetAttachmentView[index];
  color.loadOp = RI_ATTACHMENT_LOAD_OP_CLEAR;
  color.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
  const cColor clear = apSettings ? apSettings->mClearColor : cColor(0, 0);
  color.clearValue.color[0] = clear.r;
  color.clearValue.color[1] = clear.g;
  color.clearValue.color[2] = clear.b;
  color.clearValue.color[3] = clear.a;

  RIRenderingAttachment depth = {};
  depth.view = *state->depthView[index];
  depth.loadOp = RI_ATTACHMENT_LOAD_OP_CLEAR;
  depth.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
  depth.clearValue.depth = 1.0f;

  RIBeginRenderingDesc begin = {};
  begin.renderArea.width = static_cast<int16_t>(state->width);
  begin.renderArea.height = static_cast<int16_t>(state->height);
  RIRenderingAttachment colors[6] = {};
  if (packedVisibility) {
    RIRenderingAttachment visibility = {};
    visibility.view = *state->visibilityAttachmentView[index];
    visibility.loadOp = RI_ATTACHMENT_LOAD_OP_CLEAR;
    visibility.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
    colors[0] = visibility;
  } else {
    colors[0].view = *state->materialColorAttachmentView[index];
  }
  if (packedVisibility) {
    colors[1].view = *state->materialColorAttachmentView[index];
  } else {
    colors[1].view = *state->positionAttachmentView[index];
    colors[2].view = *state->normalAttachmentView[index];
    colors[3].view = *state->shadingNormalAttachmentView[index];
    colors[4].view = *state->surfaceAttachmentView[index];
  }
  if (packedVisibility) {
    colors[2].view = *state->velocityAttachmentView[index];
  } else {
    colors[5].view = *state->velocityAttachmentView[index];
  }
  for (uint32_t i = 0; i < (packedVisibility ? 2u : 5u); ++i) {
    colors[i].loadOp = RI_ATTACHMENT_LOAD_OP_CLEAR;
    colors[i].storeOp = RI_ATTACHMENT_STORE_OP_STORE;
  }
  colors[packedVisibility ? 2 : 5].loadOp = RI_ATTACHMENT_LOAD_OP_CLEAR;
  colors[packedVisibility ? 2 : 5].storeOp = RI_ATTACHMENT_STORE_OP_STORE;
  begin.colorCount = packedVisibility ? 3 : 6;
  begin.colors = colors;
  begin.depthStencil = &depth;
  // Phase 1 of the camera cull: mark the commands that were visible last frame.
  // Recorded before the scope opens -- a dispatch cannot run inside one.
  bool cameraCullDispatched = false;
  uint32_t cameraCommandWordDelta = 0;
  RISegmentReq cameraTileReq = {};
  RISegmentReq cameraReq = {};
  RISegmentReq cameraGroupReq = {};
  cStandardShadowCullPass::Buffers cameraCullBuffers{};
  if (cameraCullReady && drawCount > 0) {
    const uint32_t groupCount =
        (drawCount + kStandardCullGroupSize - 1u) / kStandardCullGroupSize;
    if (m_shadowCullTileSegment.request(mpGraphics->frameIndex, 1,
                                        &cameraTileReq) &&
        m_cullCameraSegment.request(mpGraphics->frameIndex, 1, &cameraReq) &&
        m_shadowCullGroupSegment.request(mpGraphics->frameIndex, groupCount,
                                         &cameraGroupReq)) {
      // Row-major, as StandardExtractFrustumPlanes and standardCullProjectAabb
      // both read it -- not the transposed dump the shaders take.
      const cMatrixf viewProjection = cMath::MatrixMul(
          apFrustum->GetProjectionMatrix(), apFrustum->GetViewMatrix());

      auto *cameraSlot = reinterpret_cast<StandardCullCamera *>(
          static_cast<uint8_t *>(m_cullCameraBuffer.mappedAddress) +
          cameraReq.elementOffset * sizeof(StandardCullCamera));
      StandardCullCamera camera{};
      std::memcpy(camera.viewProjection, occlusionViewProjection.v,
                  sizeof(camera.viewProjection));
      camera.hiZWidth = state->hiZ.width;
      camera.hiZHeight = state->hiZ.height;
      camera.hiZMipCount = state->hiZ.mipCount;
      *cameraSlot = camera;

      auto *tileSlot = reinterpret_cast<StandardCullTile *>(
          static_cast<uint8_t *>(m_shadowCullTileBuffer.mappedAddress) +
          cameraTileReq.elementOffset * sizeof(StandardCullTile));
      StandardCullTile tile{};
      StandardExtractFrustumPlanes(viewProjection.v, tile.planes);
      tile.planeCount = 6u;
      tile.variabilityMask =
          kStandardCullVariabilityStatic | kStandardCullVariabilityDynamic;
      tile.candidateBase =
          static_cast<uint32_t>(cameraCandidateReq.elementOffset);
      tile.candidateCount = drawCount;
      tile.cameraIndex = static_cast<uint32_t>(cameraReq.elementOffset);
      *tileSlot = tile;

      auto *groupSlots = reinterpret_cast<StandardCullGroup *>(
          static_cast<uint8_t *>(m_shadowCullGroupBuffer.mappedAddress) +
          cameraGroupReq.elementOffset * sizeof(StandardCullGroup));
      for (uint32_t group = 0; group < groupCount; ++group) {
        groupSlots[group].tileIndex =
            static_cast<uint32_t>(cameraTileReq.elementOffset);
        groupSlots[group].candidateOffset = group * kStandardCullGroupSize;
      }

      cameraCullBuffers.candidates = &m_cameraCandidateBuffer;
      cameraCullBuffers.tiles = &m_shadowCullTileBuffer;
      cameraCullBuffers.groups = &m_shadowCullGroupBuffer;
      cameraCullBuffers.indirect = m_indirectDrawBuffer.gpu();
      cameraCullBuffers.drawCounts = &m_shadowDrawCountBuffer;
      cameraCullBuffers.cameras = &m_cullCameraBuffer;
      cameraCullBuffers.visibility = &m_cullVisibilityBuffer;
      cameraCullBuffers.hiZ = state->hiZ.sampleView[index].Get();
      cameraCullBuffers.candidateCapacity = kStandardCameraMaxDraws;
      cameraCullBuffers.indirectCapacity = kObjectSlotCapacity;
      cameraCullBuffers.indirectWordCapacity =
          kObjectSlotCapacity * (sizeof(VkDrawIndirectCommand) / sizeof(uint32_t));
      // Shared with the shadow pass, so these span its ring, not one Draw.
      cameraCullBuffers.tileCapacity = kStandardShadowTileRing;
      cameraCullBuffers.groupCapacity = kStandardShadowCullGroupRing;
      cameraCullBuffers.drawCountCapacity = kStandardShadowTileRing;
      cameraCullBuffers.cameraCapacity = kStandardCullMaxCameras;
      cameraCullBuffers.visibilityCapacity = kStandardCullVisibilityKeys;
      // Words from a candidate's phase-1 command to its phase-2 one. Both
      // ranges live in the same ring, so this is just their element distance.
      cameraCommandWordDelta = static_cast<uint32_t>(
          (phaseTwoReq.elementOffset - req.elementOffset) *
          (sizeof(VkDrawIndirectCommand) / sizeof(uint32_t)));

      cameraCullDispatched = m_shadowCull->Dispatch(
          &mpGraphics->primary.cmds[0], mpGraphics->frameIndex,
          cameraCullBuffers, static_cast<uint32_t>(cameraTileReq.elementOffset),
          1, static_cast<uint32_t>(cameraGroupReq.elementOffset), groupCount,
          kStandardCullModeVisibilityReplay);
    }
  }
  // The flush above left the commands as a cull's input. Without the replay
  // dispatch nothing hands them to the draw, so do it here.
  if (cameraCullReady && drawCount > 0 && !cameraCullDispatched &&
      m_indirectDrawBuffer.staged)
    mpGraphics->primary.cmds[0].vk_d3d12_bufferBarrier(RIBufferBarrier(
        m_indirectDrawBuffer.gpu(), RI_RESOURCE_STATE_STORAGE_WRITE,
        RI_RESOURCE_STATE_INDIRECT_ARGUMENT, RI_STAGE_COMPUTE,
        RI_STAGE_DRAW_INDIRECT));

  mpGraphics->primary.cmds[0].vk_d3d12_beginRendering(&mpGraphics->device,
                                                      begin);

  if (drawCount) {
    if (packedVisibility) {
      // Matches the packed begin above: visibility, material colour, velocity.
      const RIGraphicsPipelineDesc pd = MakeGBufferMRTPipelineDesc(
          cGraphics::VisibilityFormat, cGraphics::PogoColorFormat,
          cGraphics::VelocityFormat, cGraphics::DepthFormat);
      m_visibility->bindPipeline(&mpGraphics->device,
                                 &mpGraphics->primary.cmds[0],
                                 HASH_INITIAL_VALUE, "Standard.visibility", pd);
      m_visibility->bindBindlessDescriptorSet(
          &mpGraphics->primary.cmds[0], &mpGraphics->globalset->m_bindlessSet,
          0);
    } else {
      const RIGraphicsPipelineDesc pd = MakeStandardReconstructPipelineDesc(
          cGraphics::PogoColorFormat, RI_FORMAT_RGBA32_SFLOAT,
          RI_FORMAT_RGBA32_SFLOAT, RI_FORMAT_RGBA32_SFLOAT,
          cGraphics::VisibilityFormat, cGraphics::DepthFormat, true,
          /*writesVelocity=*/true);
      m_fallback->bindPipeline(&mpGraphics->device,
                               &mpGraphics->primary.cmds[0], HASH_INITIAL_VALUE,
                               "Standard.fallback", pd);
      m_fallback->bindBindlessDescriptorSet(
          &mpGraphics->primary.cmds[0], &mpGraphics->globalset->m_bindlessSet,
          0);
    }
    (packedVisibility ? m_visibility : m_fallback)
        ->bindDescriptors(&mpGraphics->device, &mpGraphics->primary.cmds[0],
                          mpGraphics->frameIndex, &frameBinding, 1);
    RIViewport vp;
    vp.x = 0;
    vp.y = float(state->height);
    vp.width = float(state->width);
    vp.height = -float(state->height);
    vp.depthMin = 0;
    vp.depthMax = 1;
    RIRect sc;
    sc.x = 0;
    sc.y = 0;
    sc.width = int16_t(state->width);
    sc.height = int16_t(state->height);
    mpGraphics->primary.cmds[0].setViewport(&mpGraphics->device, vp);
    mpGraphics->primary.cmds[0].setScissor(&mpGraphics->device, sc);
    mpGraphics->primary.cmds[0].drawIndirect(
        &mpGraphics->device, m_indirectDrawBuffer.gpu(),
        req.elementOffset * sizeof(VkDrawIndirectCommand), drawCount,
        sizeof(VkDrawIndirectCommand));
  }

  mpGraphics->primary.cmds[0].vk_d3d12_endRendering(&mpGraphics->device);

  // ---------------------------------------------------------------------
  // Phase 2 of the camera cull.
  //
  // What phase 1 just drew is a superset of last frame's visible set, which is
  // enough to build a pyramid from. Test every candidate against it, draw the
  // ones that are visible now but were not drawn above, and record the answer
  // for next frame's phase 1.
  //
  // The scope has to close and reopen around this: the pyramid build and the
  // cull are compute dispatches, and neither can run inside a render pass. The
  // second scope loads every attachment instead of clearing, so phase 1's
  // output survives.
  // ---------------------------------------------------------------------
  if (cameraCullDispatched) {
    RICmd *cameraCmd = &mpGraphics->primary.cmds[0];
    cameraCmd->vk_d3d12_textureBarrier(RITextureBarrier(
        state->depthTextures[index].Get(), RI_RESOURCE_STATE_DEPTH_WRITE,
        RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT, RI_STAGE_COMPUTE,
        RI_BARRIER_ASPECT_DEPTH));
    const bool built =
        m_hiZ->Build(cameraCmd, mpGraphics->frameIndex, state->hiZ, index,
                     state->width, state->height,
                     state->depthSampleView[index].Get());
    cameraCmd->vk_d3d12_textureBarrier(RITextureBarrier(
        state->depthTextures[index].Get(), RI_RESOURCE_STATE_SHADER_RESOURCE,
        RI_RESOURCE_STATE_DEPTH_WRITE, RI_STAGE_COMPUTE, RI_STAGE_FRAGMENT,
        RI_BARRIER_ASPECT_DEPTH));

    const uint32_t groupCount =
        (drawCount + kStandardCullGroupSize - 1u) / kStandardCullGroupSize;
    const bool culled =
        built &&
        m_shadowCull->Dispatch(
            cameraCmd, mpGraphics->frameIndex, cameraCullBuffers,
            static_cast<uint32_t>(cameraTileReq.elementOffset), 1,
            static_cast<uint32_t>(cameraGroupReq.elementOffset), groupCount,
            kStandardCullModeVisibilityUpdate, cameraCommandWordDelta);

    if (culled) {
      // Cover every color attachment, including the fallback G-buffer's MRTs.
      cameraCmd->vk_d3d12_memoryBarrier(
          {RI_RESOURCE_STATE_RENDER_TARGET, RI_RESOURCE_STATE_RENDER_TARGET_READ});
      for (uint32_t i = 0; i < begin.colorCount; ++i) {
        colors[i].loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
        colors[i].storeOp = RI_ATTACHMENT_STORE_OP_STORE;
      }
      depth.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
      cameraCmd->vk_d3d12_beginRendering(&mpGraphics->device, begin);
      if (packedVisibility) {
        const RIGraphicsPipelineDesc pd = MakeGBufferMRTPipelineDesc(
            cGraphics::VisibilityFormat, cGraphics::PogoColorFormat,
            cGraphics::VelocityFormat, cGraphics::DepthFormat);
        m_visibility->bindPipeline(&mpGraphics->device, cameraCmd,
                                   HASH_INITIAL_VALUE, "Standard.visibility",
                                   pd);
        m_visibility->bindBindlessDescriptorSet(
            cameraCmd, &mpGraphics->globalset->m_bindlessSet, 0);
      } else {
        const RIGraphicsPipelineDesc pd = MakeStandardReconstructPipelineDesc(
            cGraphics::PogoColorFormat, RI_FORMAT_RGBA32_SFLOAT,
            RI_FORMAT_RGBA32_SFLOAT, RI_FORMAT_RGBA32_SFLOAT,
            cGraphics::VisibilityFormat, cGraphics::DepthFormat, true,
            /*writesVelocity=*/true);
        m_fallback->bindPipeline(&mpGraphics->device, cameraCmd,
                                 HASH_INITIAL_VALUE, "Standard.fallback", pd);
        m_fallback->bindBindlessDescriptorSet(
            cameraCmd, &mpGraphics->globalset->m_bindlessSet, 0);
      }
      (packedVisibility ? m_visibility : m_fallback)
          ->bindDescriptors(&mpGraphics->device, cameraCmd,
                            mpGraphics->frameIndex, &frameBinding, 1);
      RIViewport vp;
      vp.x = 0;
      vp.y = float(state->height);
      vp.width = float(state->width);
      vp.height = -float(state->height);
      vp.depthMin = 0;
      vp.depthMax = 1;
      RIRect sc;
      sc.x = 0;
      sc.y = 0;
      sc.width = int16_t(state->width);
      sc.height = int16_t(state->height);
      cameraCmd->setViewport(&mpGraphics->device, vp);
      cameraCmd->setScissor(&mpGraphics->device, sc);
      cameraCmd->drawIndirect(
          &mpGraphics->device, m_indirectDrawBuffer.gpu(),
          phaseTwoReq.elementOffset * sizeof(VkDrawIndirectCommand), drawCount,
          sizeof(VkDrawIndirectCommand));
      cameraCmd->vk_d3d12_endRendering(&mpGraphics->device);
    }
  }
  if (!packedVisibility) {
    mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(
        RI_PogoShaderBarrier(state->materialColorTexture[index].Get(), false));
    mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(RITextureBarrier(
        state->positionTexture[index].Get(), RI_RESOURCE_STATE_RENDER_TARGET,
        RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT, RI_STAGE_FRAGMENT,
        RI_BARRIER_ASPECT_COLOR));
    mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(RITextureBarrier(
        state->normalTexture[index].Get(), RI_RESOURCE_STATE_RENDER_TARGET,
        RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT, RI_STAGE_FRAGMENT,
        RI_BARRIER_ASPECT_COLOR));
    mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(RITextureBarrier(
        state->shadingNormalTexture[index].Get(),
        RI_RESOURCE_STATE_RENDER_TARGET, RI_RESOURCE_STATE_SHADER_RESOURCE,
        RI_STAGE_FRAGMENT, RI_STAGE_FRAGMENT, RI_BARRIER_ASPECT_COLOR));
    mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(RITextureBarrier(
        state->surfaceTexture[index].Get(), RI_RESOURCE_STATE_RENDER_TARGET,
        RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT, RI_STAGE_FRAGMENT,
        RI_BARRIER_ASPECT_COLOR));
  }
  if (packedVisibility)
    mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(RITextureBarrier(
        state->visibilityTexture[index].Get(), RI_RESOURCE_STATE_RENDER_TARGET,
        RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT, RI_STAGE_FRAGMENT,
        RI_BARRIER_ASPECT_COLOR));
  mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(RITextureBarrier(
      state->velocityTexture[index].Get(), RI_RESOURCE_STATE_RENDER_TARGET,
      RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT, RI_STAGE_FRAGMENT,
      RI_BARRIER_ASPECT_COLOR));

  if (!packedVisibility)
    mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(RITextureBarrier(
        state->depthTextures[index].Get(), RI_RESOURCE_STATE_DEPTH_WRITE,
        RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT, RI_STAGE_FRAGMENT,
        RI_BARRIER_ASPECT_DEPTH));

  // Decode the packed hit with a fullscreen graphics pass. All material
  // G-buffer outputs are written at native extent, so downstream geometric
  // normal consumers and lighting have real surfaces in the barycentric path.
  if (packedVisibility && m_reconstruct && m_reconstructLoaded) {
    RITextureBarrier reconInputs[6] = {};
    auto makeReconOutputBarrier = [](RITexture *texture,
                                     RIResourceState_e before,
                                     RIStageBits_e beforeStage) {
      return RITextureBarrier(texture, before, RI_RESOURCE_STATE_RENDER_TARGET,
                              beforeStage, RI_STAGE_FRAGMENT,
                              RI_BARRIER_ASPECT_COLOR);
    };
    reconInputs[0] = RITextureBarrier(
        state->visibilityTexture[index].Get(),
        RI_RESOURCE_STATE_SHADER_RESOURCE, RI_RESOURCE_STATE_SHADER_RESOURCE,
        RI_STAGE_FRAGMENT, RI_STAGE_FRAGMENT, RI_BARRIER_ASPECT_COLOR);
    reconInputs[1] = makeReconOutputBarrier(
        state->materialColorTexture[index].Get(),
        RI_RESOURCE_STATE_RENDER_TARGET, RI_STAGE_FRAGMENT);
    reconInputs[2] =
        makeReconOutputBarrier(state->positionTexture[index].Get(),
                               RI_RESOURCE_STATE_UNDEFINED, RI_STAGE_NONE);
    reconInputs[3] =
        makeReconOutputBarrier(state->normalTexture[index].Get(),
                               RI_RESOURCE_STATE_UNDEFINED, RI_STAGE_NONE);
    reconInputs[4] =
        makeReconOutputBarrier(state->shadingNormalTexture[index].Get(),
                               RI_RESOURCE_STATE_UNDEFINED, RI_STAGE_NONE);
    reconInputs[5] =
        makeReconOutputBarrier(state->surfaceTexture[index].Get(),
                               RI_RESOURCE_STATE_UNDEFINED, RI_STAGE_NONE);
    mpGraphics->primary.cmds[0].vk_d3d12_textureBarriers<6>(6, reconInputs);
    mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(RITextureBarrier(
        state->depthTextures[index].Get(), RI_RESOURCE_STATE_DEPTH_WRITE,
        RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT, RI_STAGE_FRAGMENT,
        RI_BARRIER_ASPECT_DEPTH));
    RIRenderingAttachment mrt[5] = {};
    RISharedPointer<RITextureView> *views[5] = {
        &state->materialColorAttachmentView[index],
        &state->positionAttachmentView[index],
        &state->normalAttachmentView[index],
        &state->shadingNormalAttachmentView[index],
        &state->surfaceAttachmentView[index]};
    for (uint32_t i = 0; i < 5; ++i) {
      mrt[i].view = **views[i];
      mrt[i].loadOp = RI_ATTACHMENT_LOAD_OP_CLEAR;
      mrt[i].storeOp = RI_ATTACHMENT_STORE_OP_STORE;
    }
    mrt[0].clearValue = color.clearValue;
    RIBeginRenderingDesc reconBegin = {};
    reconBegin.renderArea.width = static_cast<int16_t>(state->width);
    reconBegin.renderArea.height = static_cast<int16_t>(state->height);
    reconBegin.colorCount = 5;
    reconBegin.colors = mrt;
    reconBegin.depthStencil = nullptr;
    mpGraphics->primary.cmds[0].vk_d3d12_beginRendering(&mpGraphics->device,
                                                        reconBegin);
    const RIGraphicsPipelineDesc pd = MakeStandardReconstructPipelineDesc(
        cGraphics::PogoColorFormat, RI_FORMAT_RGBA32_SFLOAT,
        RI_FORMAT_RGBA32_SFLOAT, RI_FORMAT_RGBA32_SFLOAT,
        cGraphics::VisibilityFormat, cGraphics::DepthFormat, false);
    m_reconstruct->bindPipeline(&mpGraphics->device,
                                &mpGraphics->primary.cmds[0],
                                HASH_INITIAL_VALUE, "Standard.reconstruct", pd);
    m_reconstruct->bindBindlessDescriptorSet(
        &mpGraphics->primary.cmds[0], &mpGraphics->globalset->m_bindlessSet, 0);
    RIProgram::DescriptorBinding inputs[2];
    inputs[0].handle = DescriptorBindingID::Create("visibilityInput");
    inputs[0].descriptor = RIDescriptor::sampledImage(
        &mpGraphics->device, state->visibilityView[index].Get());
    inputs[1].handle = DescriptorBindingID::Create("depthInput");
    inputs[1].descriptor = RIDescriptor::sampledImage(
        &mpGraphics->device, state->depthSampleView[index].Get());
    RIProgram::DescriptorBinding reconBindings[3] = {frameBinding, inputs[0],
                                                     inputs[1]};
    m_reconstruct->bindDescriptors(&mpGraphics->device,
                                   &mpGraphics->primary.cmds[0],
                                   mpGraphics->frameIndex, reconBindings, 3);
    RIViewport vp;
    vp.x = 0;
    vp.y = float(state->height);
    vp.width = float(state->width);
    vp.height = -float(state->height);
    vp.depthMin = 0;
    vp.depthMax = 1;
    RIRect sc;
    sc.x = 0;
    sc.y = 0;
    sc.width = int16_t(state->width);
    sc.height = int16_t(state->height);
    mpGraphics->primary.cmds[0].setViewport(&mpGraphics->device, vp);
    mpGraphics->primary.cmds[0].setScissor(&mpGraphics->device, sc);
    mpGraphics->primary.cmds[0].draw(&mpGraphics->device, 3, 1, 0, 0);
    mpGraphics->primary.cmds[0].vk_d3d12_endRendering(&mpGraphics->device);
    mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(
        RI_PogoShaderBarrier(state->materialColorTexture[index].Get(), false));
    mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(
        RI_PogoShaderBarrier(state->positionTexture[index].Get(), false));
    mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(
        RI_PogoShaderBarrier(state->normalTexture[index].Get(), false));
    mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(
        RI_PogoShaderBarrier(state->shadingNormalTexture[index].Get(), false));
    mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(RITextureBarrier(
        state->surfaceTexture[index].Get(), RI_RESOURCE_STATE_RENDER_TARGET,
        RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT, RI_STAGE_FRAGMENT,
        RI_BARRIER_ASPECT_COLOR));
  }

  // Project clustered decals after reconstruction/fallback and before
  // lighting. The pass writes an independent target, so a failed/empty pass
  // cannot corrupt the identity material color input.
  bool decalsRendered = false;
  if (m_decals && m_decals->IsLoaded() && apWorld->GetDecalCount() > 0 &&
      !state->decalColorTexture[index].isEmpty()) {
    mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(RITextureBarrier(
        state->decalColorTexture[index].Get(),
        state->decalColorInitialized[index] ? RI_RESOURCE_STATE_SHADER_RESOURCE
                                            : RI_RESOURCE_STATE_UNDEFINED,
        RI_RESOURCE_STATE_RENDER_TARGET,
        state->decalColorInitialized[index] ? RI_STAGE_FRAGMENT : RI_STAGE_NONE,
        RI_STAGE_FRAGMENT, RI_BARRIER_ASPECT_COLOR));
    decalsRendered = m_decals->Render(
        cntx, &mpGraphics->primary.cmds[0], mpGraphics->frameIndex,
        state->width, state->height, state->materialColorView[index].Get(),
        state->positionView[index].Get(), state->normalView[index].Get(),
        state->surfaceView[index].Get(),
        state->decalColorAttachmentView[index].Get(), apWorld, &frameBinding);
    if (decalsRendered) {
      mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(
          RI_PogoShaderBarrier(state->decalColorTexture[index].Get(), false));
      // Render validates everything before recording, but keep the resource
      // state explicit if a future implementation can fail after beginning
      // the pass.  The attachment transition above has already happened, so
      // the failure path must hand the image back to shader-read before the
      // resolve samples the identity material path.
    } else {
      mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(RITextureBarrier(
          state->decalColorTexture[index].Get(),
          RI_RESOURCE_STATE_RENDER_TARGET, RI_RESOURCE_STATE_SHADER_RESOURCE,
          RI_STAGE_FRAGMENT, RI_STAGE_FRAGMENT, RI_BARRIER_ASPECT_COLOR));
    }
    state->decalColorInitialized[index] = true;
  }

  // Type="Decal" meshes (dirt_floor / moist_wall / trails). The legacy
  // deferred renderer drew them into its albedo target (RendererDeferred.cpp
  // cmdBuildPrimaryGBuffer). Standard rasterizes them into multiply/add
  // accumulators the light resolve folds into albedo, the same contract as the
  // Hybrid composite. Both clear every frame so the resolve reads identity
  // when nothing draws.
  {
    std::vector<iRenderable *> mulDecals, addDecals;
    for (iRenderable *o :
         m_rendererList.GetRenderableItems(eRenderListType_Decal)) {
      if (!m_meshDecalLoaded || !o || !o->GetMaterial() ||
          !o->GetVertexBuffer() || o->GetVertexBuffer()->GetIndexNum() <= 0)
        continue;
      switch (o->GetMaterial()->GetBlendMode()) {
      case eMaterialBlendMode_Mul:
      case eMaterialBlendMode_MulX2:
        mulDecals.push_back(o);
        break;
      case eMaterialBlendMode_Add:
        addDecals.push_back(o);
        break;
      default:
        break; // Type="Decal" content is Mul/MulX2/Add only.
      }
    }
    struct MeshDecalDraw {
      cVertexBuffer *vb;
      eMaterialBlendMode blend;
      uint32_t slot;
    };
    auto submitDecals = [&](const std::vector<iRenderable *> &list) {
      std::vector<MeshDecalDraw> draws;
      for (iRenderable *o : list) {
        cMaterial *mat = o->GetMaterial();
        auto *vb = static_cast<cVertexBuffer *>(o->GetVertexBuffer());
        vb->SubmitToGPU(&mpGraphics->device);
        const uint32_t materialId =
            mpGraphics->globalset
                ->submitMaterial(cntx, mat,
                                 static_cast<uint32_t>(mpGraphics->frameIndex))
                .materialId;
        if (materialId == UINT32_MAX)
          continue;
        ObjectSubmitDesc object;
        object.modelMatrix = o->GetModelMatrix(apFrustum);
        object.uvMatrix = mat->GetUvMatrix();
        object.materialId = materialId;
        object.dissolveAmount = o->GetCoverageAmount();
        object.renderFlags = o->GetRenderFlags();
        const hash_t cookie = hash_u32(
            hash_u64(HASH_INITIAL_VALUE, o->GetUniqueCookie()), paneSalt);
        const uint32_t slot = mpGraphics->globalset->submitObject(
            cookie, static_cast<uint32_t>(mpGraphics->frameIndex), vb, object,
            kSubmitData | kSubmitVertex | kSubmitIndex);
        if (slot != UINT32_MAX)
          draws.push_back({vb, mat->GetBlendMode(), slot});
      }
      return draws;
    };
    const std::vector<MeshDecalDraw> mulDraws = submitDecals(mulDecals);
    const std::vector<MeshDecalDraw> addDraws = submitDecals(addDecals);
    if (!mulDraws.empty() || !addDraws.empty())
      mpGraphics->globalset->flushMirrors(&mpGraphics->device);

    RICmd *cmd = &mpGraphics->primary.cmds[0];
    cmd->vk_d3d12_textureBarrier(RITextureBarrier(
        state->depthTextures[index].Get(), RI_RESOURCE_STATE_SHADER_RESOURCE,
        RI_RESOURCE_STATE_DEPTH_READ, RI_STAGE_FRAGMENT, RI_STAGE_NONE,
        RI_BARRIER_ASPECT_DEPTH));
    auto accumulate = [&](RITexture *texture, RITextureView *attachment,
                          float clear,
                          const std::vector<MeshDecalDraw> &draws) {
      cmd->vk_d3d12_textureBarrier(RITextureBarrier(
          texture, RI_RESOURCE_STATE_UNDEFINED, RI_RESOURCE_STATE_RENDER_TARGET,
          RI_STAGE_NONE, RI_STAGE_NONE, RI_BARRIER_ASPECT_COLOR));
      RIRenderingAttachment color = {};
      color.view = *attachment;
      color.loadOp = RI_ATTACHMENT_LOAD_OP_CLEAR;
      color.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
      for (int c = 0; c < 4; ++c)
        color.clearValue.color[c] = clear;
      RIRenderingAttachment depth = {};
      depth.view = *state->depthView[index];
      depth.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
      depth.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
      depth.readOnly = true;
      RIBeginRenderingDesc begin = {};
      begin.renderArea.width = static_cast<int16_t>(state->width);
      begin.renderArea.height = static_cast<int16_t>(state->height);
      begin.colorCount = 1;
      begin.colors = &color;
      begin.depthStencil = &depth;
      cmd->vk_d3d12_beginRendering(&mpGraphics->device, begin);
      if (!draws.empty()) {
        RIViewport vp;
        vp.x = 0;
        vp.y = float(state->height);
        vp.width = float(state->width);
        vp.height = -float(state->height);
        vp.depthMin = 0;
        vp.depthMax = 1;
        RIRect sc;
        sc.x = 0;
        sc.y = 0;
        sc.width = int16_t(state->width);
        sc.height = int16_t(state->height);
        cmd->setViewport(&mpGraphics->device, vp);
        cmd->setScissor(&mpGraphics->device, sc);
        m_meshDecal->bindBindlessDescriptorSet(
            cmd, &mpGraphics->globalset->m_bindlessSet, 0);
        m_meshDecal->bindDescriptors(&mpGraphics->device, cmd,
                                     mpGraphics->frameIndex, &frameBinding, 1);
        for (const MeshDecalDraw &draw : draws) {
          uint32_t presentMask = 0;
          if (!BindMeshDecalStreams(cmd, mpGraphics, draw.vb, &presentMask))
            continue;
          m_meshDecal->bindPipeline(
              &mpGraphics->device, cmd, HASH_INITIAL_VALUE,
              "Standard.meshDecal",
              MakeDecalPipelineDesc(cGraphics::PogoColorFormat,
                                    cGraphics::DepthFormat,
                                    MeshDecalBlend(draw.blend), presentMask));
          cmd->drawIndexed(&mpGraphics->device,
                           static_cast<uint32_t>(draw.vb->GetIndexNum()), 1u,
                           0u, 0, draw.slot);
        }
      }
      cmd->vk_d3d12_endRendering(&mpGraphics->device);
      cmd->vk_d3d12_textureBarrier(RI_PogoShaderBarrier(texture, false));
    };
    accumulate(state->decalMulTexture[index].Get(),
               state->decalMulAttachmentView[index].Get(), 1.0f, mulDraws);
    accumulate(state->decalAddTexture[index].Get(),
               state->decalAddAttachmentView[index].Get(), 0.0f, addDraws);
      cmd->vk_d3d12_textureBarrier(RITextureBarrier(
        state->depthTextures[index].Get(), RI_RESOURCE_STATE_DEPTH_READ,
        RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_NONE, RI_STAGE_FRAGMENT,
        RI_BARRIER_ASPECT_DEPTH));
  }

  // Legacy RendererDeferred billboard halos: occluded-texel counts of each
  // halo's source box fade its glow (cStandardHaloPass). Resolved before the
  // particle pass so this frame draws the newest alpha.
  if (m_halo) {
    if (!state->haloQueries) {
      state->haloQueries = std::make_shared<StandardHaloQueryState>();
      state->haloQueries->graphics = mpGraphics;
    }
    const std::span<iRenderable *> haloCandidates =
        m_rendererList.GetRenderableItems(eRenderListType_Translucent);
    m_halo->Resolve(*state->haloQueries, haloCandidates, apFrustum);
    m_halo->Record(cntx, *state->haloQueries, haloCandidates,
                   state->depthTextures[index].Get(),
                   state->depthView[index].Get(), state->width, state->height,
                   frameBinding, paneSalt);
  }

  bool aoRendered = false;
  if (apSettings && apSettings->mbSSAOActive && m_ambientOcclusion &&
      m_ambientOcclusionLoaded) {
    aoRendered = m_ambientOcclusion->Render(cntx, &mpGraphics->primary.cmds[0],
                                            mpGraphics->frameIndex, state,
                                            index, apFrustum, &frameBinding);
  }
  // From here on the render depth sits in SHADER_RESOURCE. Every exit must hand
  // it back in DEPTH_ATTACHMENT_OPTIMAL, the same contract as
  // cHybridRenderer::Draw: the viewport's post-translucence handlers
  // (LuxEffectRenderer) load it as a writable attachment, and temporal
  // presentation takes DEPTH_WRITE as its entry state.
  auto handBackDepth = [&]() {
    mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(RITextureBarrier(
        state->depthTextures[index].Get(), RI_RESOURCE_STATE_SHADER_RESOURCE,
        RI_RESOURCE_STATE_DEPTH_WRITE, RI_STAGE_FRAGMENT, RI_STAGE_NONE,
        RI_BARRIER_ASPECT_DEPTH));
  };

  // The light pass multiplies surface colour by AO, so whatever it samples must
  // read 1 where AO did not run. Record the fallback clear here, outside the
  // resolve render pass; the shader clamps its Load to the texture size.
  RISharedPointer<RITexture> aoFallbackTexture;
  RISharedPointer<RITextureView> aoFallbackView;
  const bool useAoFallback = !aoRendered || state->aoView[index].isEmpty();
  if (useAoFallback) {
    RITextureDesc td{};
    td.type = RI_TEXTURE_2D;
    td.format = RI_FORMAT_R16_SFLOAT;
    td.width = 1;
    td.height = 1;
    td.depth = 1;
    td.layerNum = 1;
    td.mipNum = 1;
    td.sampleCount = 1;
    td.usage = RI_USAGE_SHADER_RESOURCE | RI_USAGE_SHADER_RESOURCE_STORAGE |
               RI_USAGE_TRANSFER_DST;
    RITexture t = RITexture::create(&mpGraphics->device, td);
    aoFallbackTexture = RISharedPointer<RITexture>(&mpGraphics->device, t);
    if (!aoFallbackTexture.isEmpty()) {
      NameStandardImage(mpGraphics, *aoFallbackTexture,
                        "StandardRenderer.aoFallback");
      RITextureViewDesc vd{};
      vd.viewType = RI_VIEWTYPE_SHADER_RESOURCE_2D;
      vd.format = RI_FORMAT_R16_SFLOAT;
      vd.mipNum = 1;
      vd.layerNum = 1;
      RITextureView v = RITextureView::create(&mpGraphics->device,
                                              aoFallbackTexture.Get(), vd);
      aoFallbackView = RISharedPointer<RITextureView>(&mpGraphics->device, v);
      RICmd &cmd = mpGraphics->primary.cmds[0];
      RITextureBarrier toClear(aoFallbackTexture.Get(),
                               RI_RESOURCE_STATE_UNDEFINED,
                               RI_RESOURCE_STATE_CLEAR_STORAGE);
      cmd.vk_d3d12_resourceBarrier<0, 0, 1>(0, nullptr, 0, nullptr, 1,
                                            &toClear);
      const float unoccluded[4] = {1.0f, 1.0f, 1.0f, 1.0f};
      cmd.clearStorageImage(&mpGraphics->device, aoFallbackTexture.Get(),
                            unoccluded);
      RITextureBarrier toRead(aoFallbackTexture.Get(),
                              RI_RESOURCE_STATE_CLEAR_STORAGE,
                              RI_RESOURCE_STATE_SHADER_RESOURCE);
      cmd.vk_d3d12_resourceBarrier<0, 0, 1>(0, nullptr, 0, nullptr, 1, &toRead);
      mpGraphics->graphicsDefer.push(aoFallbackTexture);
      if (!aoFallbackView.isEmpty())
        mpGraphics->graphicsDefer.push(aoFallbackView);
    }
  }

  // Resolve the reconstructed material inputs into the final HDR target.
  // The light buffers are immutable for this draw and are retired only after
  // the frame completes, allowing multiple panes and in-flight frames.
  {
    RIRenderingAttachment output = {};
    output.view = *state->renderTargetAttachmentView[index];
    output.loadOp = RI_ATTACHMENT_LOAD_OP_CLEAR;
    output.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
    RIBeginRenderingDesc resolveBegin = {};
    resolveBegin.renderArea.width = static_cast<int16_t>(state->width);
    resolveBegin.renderArea.height = static_cast<int16_t>(state->height);
#if (DEVICE_IMPL_D3D12)
    if (RIIsTargetSelected(RI_DEVICE_API_D3D12)) {
      const RITextureView &v = output.view;
      const D3D12_RESOURCE_DESC rd = v.d3d12.resource->GetDesc();
    }
#endif
    resolveBegin.colorCount = 1;
    resolveBegin.colors = &output;
    mpGraphics->primary.cmds[0].vk_d3d12_beginRendering(&mpGraphics->device,
                                                        resolveBegin);
    const RIGraphicsPipelineDesc pd =
        MakeStandardResolvePipelineDesc(cGraphics::PogoColorFormat);
    m_lighting->bindPipeline(&mpGraphics->device, &mpGraphics->primary.cmds[0],
                             HASH_INITIAL_VALUE, "Standard.light", pd);
    m_lighting->bindBindlessDescriptorSet(
        &mpGraphics->primary.cmds[0], &mpGraphics->globalset->m_bindlessSet, 0);
    RIProgram::DescriptorBinding bindings[9] = {};
    bindings[0] = frameBinding;
    bindings[1] = RIProgram::DescriptorBinding(
        "standardColorInput",
        RIDescriptor::sampledImage(
            &mpGraphics->device,
            (decalsRendered ? state->decalColorView[index].Get()
                            : state->materialColorView[index].Get())));
    bindings[2] = RIProgram::DescriptorBinding(
        "standardPositionInput",
        RIDescriptor::sampledImage(&mpGraphics->device,
                                   state->positionView[index].Get()));
    bindings[3] = RIProgram::DescriptorBinding(
        "standardNormalInput",
        RIDescriptor::sampledImage(&mpGraphics->device,
                                   state->normalView[index].Get()));
    bindings[4] = RIProgram::DescriptorBinding(
        "standardShadingNormalInput",
        RIDescriptor::sampledImage(&mpGraphics->device,
                                   state->shadingNormalView[index].Get()));
    bindings[5] = RIProgram::DescriptorBinding(
        "standardSurfaceInput",
        RIDescriptor::sampledImage(&mpGraphics->device,
                                   state->surfaceView[index].Get()));
    bindings[6] = RIProgram::DescriptorBinding(
        "standardPointLights",
        RIDescriptor::storageBuffer(&mpGraphics->device, pointLights.Get(), 0,
                                    std::max<uint32_t>(pointLightCount, 1u) *
                                        sizeof(StandardPointLightData)));
    bindings[7] = RIProgram::DescriptorBinding(
        "standardSpotLights",
        RIDescriptor::storageBuffer(&mpGraphics->device, spotLights.Get(), 0,
                                    std::max<uint32_t>(spotLightCount, 1u) *
                                        sizeof(StandardSpotLightData)));
    bindings[8] = RIProgram::DescriptorBinding(
        "standardBoxLights",
        RIDescriptor::storageBuffer(&mpGraphics->device, boxLights.Get(), 0,
                                    std::max<uint32_t>(boxLightCount, 1u) *
                                        sizeof(StandardBoxLightData)));
    RIProgram::DescriptorBinding countBinding("standardLightCounts",
                                              RIDescriptor(), 0, false);
    // Legacy spotlight filter quality. The jitter offsets are only needed for
    // Medium/High; a failed build falls back to the single-compare Low filter.
    const int shadowQuality =
        std::clamp(static_cast<int>(iRenderer::GetShadowMapQuality()),
                   static_cast<int>(eShadowMapQuality_Low),
                   static_cast<int>(eShadowMapQuality_High));
    if (shadowQuality != m_shadowJitterQuality) {
      if (m_shadowJitter.Get())
        mpGraphics->graphicsDefer.push(
            std::function<void()>([retired = m_shadowJitter]() {}));
      m_shadowJitter =
          shadowQuality == eShadowMapQuality_Low
              ? SharedResourceHandle<Image>()
              : CreateStandardShadowJitter(
                    shadowQuality == eShadowMapQuality_High ? 64 : 32,
                    shadowQuality == eShadowMapQuality_High ? 32 : 16);
      m_shadowJitterQuality = shadowQuality;
    }
    cTexture *shadowJitterTexture =
        m_shadowJitter.Get() ? m_shadowJitter->GetTexture() : nullptr;
    const uint32_t activeShadowQuality =
        shadowJitterTexture ? static_cast<uint32_t>(shadowQuality) : 0u;
    if (shadowJitterTexture)
      mpGraphics->graphicsDefer.push(
          std::function<void()>([keep = m_shadowJitter]() {}));
    StandardLightCounts counts{pointLightCount, spotLightCount, boxLightCount,
                               activeShadowQuality};
    mpGraphics->UpdateFrameUBO(&countBinding.descriptor, &counts,
                               sizeof(counts));
    auto rampSampler = mpGraphics->resolve_filter_descriptor(
        eTextureWrap_ClampToEdge, eTextureWrap_ClampToEdge,
        eTextureWrap_ClampToEdge, eTextureFilter_Bilinear);
    auto goboSampler = mpGraphics->resolve_filter_descriptor(
        eTextureWrap_ClampToBorder, eTextureWrap_ClampToBorder,
        eTextureWrap_ClampToBorder, eTextureFilter_Trilinear);
    if (!rampSampler || !goboSampler) {
      mpGraphics->primary.cmds[0].vk_d3d12_endRendering(&mpGraphics->device);
      mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(
          RI_PogoShaderBarrier(state->renderTarget[index].Get(), false));
      handBackDepth();
      return;
    }
    RIProgram::DescriptorBinding samplerBindings[2] = {
        RIProgram::DescriptorBinding("standardRampSampler", *rampSampler),
        RIProgram::DescriptorBinding("standardGoboSampler", *goboSampler)};
    RIProgram::DescriptorBinding shadowBinding(
        "standardShadowMap",
        RIDescriptor::sampledImage(&mpGraphics->device,
                                   standardShadowView.Get()));

    RITextureView *aoInput =
        !useAoFallback ? state->aoView[index].Get() : aoFallbackView.Get();
    if (!aoInput) {
      // Fallback allocation failed: never substitute an unrelated image. Skip
      // this frame's resolve rather than multiplying lighting by garbage.
      mpGraphics->primary.cmds[0].vk_d3d12_endRendering(&mpGraphics->device);
      mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(
          RI_PogoShaderBarrier(state->renderTarget[index].Get(), false));
      handBackDepth();
      return;
    }
    RIProgram::DescriptorBinding aoBinding(
        "standardAmbientOcclusionInput",
        RIDescriptor::sampledImage(&mpGraphics->device, aoInput));

    RIProgram::DescriptorBinding resolveBindings[19] = {
        bindings[0], bindings[1],  bindings[2],        bindings[3],
        bindings[4], bindings[5],  bindings[6],        bindings[7],
        bindings[8], countBinding, samplerBindings[0], samplerBindings[1]};
    resolveBindings[12] = shadowBinding;
    resolveBindings[13] = aoBinding;
    resolveBindings[14] = RIProgram::DescriptorBinding(
        "standardDecalMulInput",
        RIDescriptor::sampledImage(&mpGraphics->device,
                                   state->decalMulView[index].Get()));
    resolveBindings[15] = RIProgram::DescriptorBinding(
        "standardDecalAddInput",
        RIDescriptor::sampledImage(&mpGraphics->device,
                                   state->decalAddView[index].Get()));
    // Low quality never reads the offsets; any valid colour view satisfies the layout.
    resolveBindings[16] = RIProgram::DescriptorBinding(
        "standardShadowJitterInput",
        shadowJitterTexture
            ? shadowJitterTexture->descriptor()
            : RIDescriptor::sampledImage(
                  &mpGraphics->device, state->materialColorView[index].Get()));
    resolveBindings[17] = RIProgram::DescriptorBinding(
        "standardShadowTiles",
        RIDescriptor::storageBuffer(
            &mpGraphics->device, shadowTiles.Get(), 0,
            std::max<size_t>(shadowTileRecords.size(), 1u) *
                sizeof(StandardShadowTileData)));
    // The per-tile light list. Standard.lightCull.cs, which would fill it, is
    // not wired up yet, so counts.lightTilesX stays 0 and psMain loops every
    // light -- but the buffer is statically referenced by the shader, so a
    // valid descriptor has to be here regardless of whether the tiled branch
    // is reachable. One element holding the overflow sentinel: if lightTilesX
    // is ever set before the cull pass exists, tile 0 degrades to the full
    // loop rather than reading a light index out of uninitialised memory.
    std::vector<uint32_t> tileLightFallback(1, kStandardLightTileOverflow);
    RISharedPointer<RIBuffer> tileLights;
    size_t tileLightCapacity = 0;
    if (!UploadStandardLights(mpGraphics, tileLightFallback, tileLights,
                              tileLightCapacity) ||
        tileLights.isEmpty()) {
      mpGraphics->primary.cmds[0].vk_d3d12_endRendering(&mpGraphics->device);
      mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(
          RI_PogoShaderBarrier(state->renderTarget[index].Get(), false));
      handBackDepth();
      return;
    }
    resolveBindings[18] = RIProgram::DescriptorBinding(
        "standardTileLights",
        RIDescriptor::storageBuffer(&mpGraphics->device, tileLights.Get(), 0,
                                    tileLightFallback.size() *
                                        sizeof(uint32_t)));
    m_lighting->bindDescriptors(&mpGraphics->device,
                                &mpGraphics->primary.cmds[0],
                                mpGraphics->frameIndex, resolveBindings, 19);
    RIViewport vp;
    vp.x = 0;
    vp.y = float(state->height);
    vp.width = float(state->width);
    vp.height = -float(state->height);
    vp.depthMin = 0;
    vp.depthMax = 1;
    RIRect sc;
    sc.x = 0;
    sc.y = 0;
    sc.width = int16_t(state->width);
    sc.height = int16_t(state->height);
    mpGraphics->primary.cmds[0].setViewport(&mpGraphics->device, vp);
    mpGraphics->primary.cmds[0].setScissor(&mpGraphics->device, sc);
    mpGraphics->primary.cmds[0].draw(&mpGraphics->device, 3, 1, 0, 0);
    mpGraphics->primary.cmds[0].vk_d3d12_endRendering(&mpGraphics->device);
    mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(
        RI_PogoShaderBarrier(state->renderTarget[index].Get(), false));
  }

  if (m_environment && m_environment->LoadData() &&
      !state->environmentTexture[index].isEmpty()) {
    mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(RITextureBarrier(
        state->environmentTexture[index].Get(),
        state->environmentInitialized[index] ? RI_RESOURCE_STATE_COPY_SRC
                                             : RI_RESOURCE_STATE_UNDEFINED,
        RI_RESOURCE_STATE_RENDER_TARGET, RI_STAGE_NONE, RI_STAGE_FRAGMENT,
        RI_BARRIER_ASPECT_COLOR));
    const bool environmentRendered = m_environment->Render(
        cntx, &mpGraphics->primary.cmds[0], mpGraphics->frameIndex,
        state->width, state->height, state->renderTarget[index].Get(),
        state->renderTargetView[index].Get(), state->positionView[index].Get(),
        state->environmentAttachmentView[index].Get(), apWorld, &frameBinding);
    if (!environmentRendered) {
      // Render returns before recording on failure; the resolved input is
      // still shader-readable and no copy-destination transition occurred.
      handBackDepth();
      return;
    }
    mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(RITextureBarrier(
        state->environmentTexture[index].Get(), RI_RESOURCE_STATE_RENDER_TARGET,
        RI_RESOURCE_STATE_COPY_SRC, RI_STAGE_FRAGMENT, RI_STAGE_COPY,
        RI_BARRIER_ASPECT_COLOR));
    mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(RITextureBarrier(
        state->renderTarget[index].Get(), RI_RESOURCE_STATE_SHADER_RESOURCE,
        RI_RESOURCE_STATE_COPY_DST, RI_STAGE_FRAGMENT, RI_STAGE_COPY,
        RI_BARRIER_ASPECT_COLOR));
    RIImageCopyDesc copy = {};
    copy.width = state->width;
    copy.height = state->height;
    copy.depth = 1;
    mpGraphics->primary.cmds[0].copyImage(
        &mpGraphics->device, state->environmentTexture[index].Get(),
        state->renderTarget[index].Get(), copy);
    mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(RITextureBarrier(
        state->renderTarget[index].Get(), RI_RESOURCE_STATE_COPY_DST,
        RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_COPY, RI_STAGE_FRAGMENT,
        RI_BARRIER_ASPECT_COLOR));
    state->environmentInitialized[index] = true;
  }

  if (cTemporalReactiveMask *reactiveMask =
          viewport->GetTemporalReactiveMask()) {
    TemporalReactiveMaskSnapshotDesc snapshot = {};
    snapshot.cmd = &mpGraphics->primary.cmds[0];
    snapshot.sceneColor = state->renderTarget[index].Get();
    snapshot.sceneColorFormat = cGraphics::PogoColorFormat;
    snapshot.extent = {state->width, state->height};
    snapshot.frameIndex = mpGraphics->frameIndex;
    snapshot.viewportCookie = viewport;
    snapshot.sceneColorEntryState = RI_RESOURCE_STATE_SHADER_RESOURCE;
    snapshot.sceneColorExitState = RI_RESOURCE_STATE_SHADER_RESOURCE;
    snapshot.sceneColorEntryStage = RI_STAGE_FRAGMENT;
    snapshot.sceneColorExitStage = RI_STAGE_FRAGMENT;
    reactiveMask->RecordOpaqueSnapshot(snapshot);
  }

  // Environment is now the completed opaque Standard scene. Walk the sorted
  // translucent list once, in order, as the legacy deferred renderer did:
  // contiguous mesh runs go to the translucent pass, contiguous particle /
  // billboard / beam runs to the particle pass, and each water surface is
  // recorded in place (preserving LargeSurface placement). A particle behind
  // glass or water therefore stays behind it.
  {
    std::vector<RIProgram::DescriptorBinding> fogBindings;
    if (m_environment)
      m_environment->AppendFogBindings(apWorld, fogBindings);
    const bool translucentReady = m_translucent && m_translucent->LoadData();
    const bool particlesReady =
        m_particles && !fogBindings.empty() && m_particles->LoadData();
    const bool waterReady =
        m_water && !fogBindings.empty() && m_water->LoadData();
    if (m_water) {
      m_water->SetWorldReflectionEnabled(!apSettings ||
                                         apSettings->mbRenderWorldReflection);
      m_water->SetClipReflectionScreenRect(
          !apSettings || apSettings->mbClipReflectionScreenRect);
      // The capture draws its reflected translucents through the renderer's
      // own pass, once it is loaded; a second instance would duplicate the
      // program and pipeline cache.
      m_water->SetTranslucentPass(translucentReady ? m_translucent.get()
                                                   : nullptr);
    }
    const uint32_t particleSalt =
        hash_u64(HASH_INITIAL_VALUE, reinterpret_cast<uintptr_t>(viewport));
    // Set by the pyramid build below, before any segment is drawn. A pyramid
    // that was never built is still UNDEFINED, so the cull must not bind it.
    bool hiZBuilt = false;

    auto drawOrdinary = [&](std::span<iRenderable *> segment) {
      if (segment.empty() || !translucentReady)
        return;

      // Reserve the worst case for this run: two commands per renderable, the
      // base draw plus the cube-map reflection draw. Only the pass knows how
      // many each item really needs, and it is the pass that fills the slots.
      // One request per ring, never one per item: RISegmentAlloc restarts at 0
      // when a request does not fit the tail, so per-item requests could split
      // a run across the wrap and the tile's candidateBase could not describe
      // it.
      cStandardTranslucentPass::OcclusionCull cull{};
      const uint32_t worstCase = static_cast<uint32_t>(segment.size()) * 2u;
      const uint32_t worstCaseGroups =
          (worstCase + kStandardCullGroupSize - 1u) / kStandardCullGroupSize;
      RISegmentReq candidateReq = {};
      RISegmentReq commandReq = {};
      RISegmentReq tileReq = {};
      RISegmentReq cameraReq = {};
      RISegmentReq groupReq = {};
      const bool reserved =
          hiZBuilt && m_shadowCull && m_shadowCull->IsLoaded() &&
          worstCase > 0 && worstCase <= kStandardTranslucentMaxDraws &&
          m_translucentCandidateSegment.request(mpGraphics->frameIndex,
                                                worstCase, &candidateReq) &&
          m_translucentCommandSegment.request(mpGraphics->frameIndex, worstCase,
                                              &commandReq) &&
          m_shadowCullTileSegment.request(mpGraphics->frameIndex, 1, &tileReq) &&
          m_cullCameraSegment.request(mpGraphics->frameIndex, 1, &cameraReq) &&
          m_shadowCullGroupSegment.request(mpGraphics->frameIndex,
                                           worstCaseGroups, &groupReq);
      if (reserved) {
        // Row-major, the order StandardExtractFrustumPlanes and
        // standardCullProjectAabb both read. NOT the transposed dump
        // cFrustum::GetViewProjectionMat returns for the shaders.
        const cMatrixf viewProjection = cMath::MatrixMul(
            apFrustum->GetProjectionMatrix(), apFrustum->GetViewMatrix());

        auto *cameraSlot = reinterpret_cast<StandardCullCamera *>(
            static_cast<uint8_t *>(m_cullCameraBuffer.mappedAddress) +
            cameraReq.elementOffset * sizeof(StandardCullCamera));
        StandardCullCamera camera{};
        std::memcpy(camera.viewProjection, occlusionViewProjection.v,
                    sizeof(camera.viewProjection));
        camera.hiZWidth = state->hiZ.width;
        camera.hiZHeight = state->hiZ.height;
        camera.hiZMipCount = state->hiZ.mipCount;
        *cameraSlot = camera;

        auto *tileSlot = reinterpret_cast<StandardCullTile *>(
            static_cast<uint8_t *>(m_shadowCullTileBuffer.mappedAddress) +
            tileReq.elementOffset * sizeof(StandardCullTile));
        StandardCullTile tile{};
        StandardExtractFrustumPlanes(viewProjection.v, tile.planes);
        tile.planeCount = 6u;
        // A camera tile keeps everything the frustum keeps; the variability
        // gate exists for lights, which choose which casters they accept.
        tile.variabilityMask =
            kStandardCullVariabilityStatic | kStandardCullVariabilityDynamic;
        tile.cameraIndex = static_cast<uint32_t>(cameraReq.elementOffset);
        *tileSlot = tile;

        cull.pass = m_shadowCull.get();
        cull.buffers.candidates = &m_translucentCandidateBuffer;
        cull.buffers.tiles = &m_shadowCullTileBuffer;
        cull.buffers.groups = &m_shadowCullGroupBuffer;
        cull.buffers.indirect = m_translucentCommandBuffer.gpu();
        cull.buffers.drawCounts = &m_shadowDrawCountBuffer;
        cull.buffers.cameras = &m_cullCameraBuffer;
        cull.buffers.hiZ = state->hiZ.sampleView[index].Get();
        cull.buffers.candidateCapacity = kStandardTranslucentMaxDraws;
        cull.buffers.indirectCapacity = kStandardTranslucentMaxDraws;
        cull.buffers.indirectWordCapacity =
            kStandardTranslucentMaxDraws *
            (sizeof(VkDrawIndexedIndirectCommand) / sizeof(uint32_t));
        // Shared with the shadow pass, so these span its ring, not one Draw.
        cull.buffers.tileCapacity = kStandardShadowTileRing;
        cull.buffers.groupCapacity = kStandardShadowCullGroupRing;
        cull.buffers.drawCountCapacity = kStandardShadowTileRing;
        cull.buffers.cameraCapacity = kStandardCullMaxCameras;
        cull.buffers.visibility = &m_cullVisibilityBuffer;
        cull.buffers.visibilityCapacity = kStandardCullVisibilityKeys;
        cull.candidateBase = static_cast<uint32_t>(candidateReq.elementOffset);
        cull.commandBase = static_cast<uint32_t>(commandReq.elementOffset);
        cull.capacity = worstCase;
        cull.tileBase = static_cast<uint32_t>(tileReq.elementOffset);
        cull.groupBase = static_cast<uint32_t>(groupReq.elementOffset);
        cull.groupCapacity = worstCaseGroups;
        cull.candidateSlots =
            reinterpret_cast<StandardCullCandidate *>(
                static_cast<uint8_t *>(
                    m_translucentCandidateBuffer.mappedAddress) +
                candidateReq.elementOffset * sizeof(StandardCullCandidate));
        cull.commandWords =
            reinterpret_cast<uint32_t *>(m_translucentCommandBuffer.mapped());
        cull.flushCommands = [this](uint64_t byteOffset, uint64_t byteSize) {
          m_translucentCommandBuffer.Flush(
              &mpGraphics->device, &mpGraphics->primary.cmds[0], byteOffset,
              byteSize, m_translucentCommandFirstUse, /*cullFollows*/ true);
          m_translucentCommandFirstUse = false;
        };
        cull.tileSlot = tileSlot;
        cull.groupSlots = reinterpret_cast<StandardCullGroup *>(
            static_cast<uint8_t *>(m_shadowCullGroupBuffer.mappedAddress) +
            groupReq.elementOffset * sizeof(StandardCullGroup));
      }

      m_translucent->Draw(cntx, state, index, segment, apFrustum, apWorld,
                          &frameBinding,
                          fogBindings.empty() ? nullptr : &fogBindings.front(),
                          standardShadowView.Get(), &pointLights, &spotLights,
                          pointLightCount, spotLightCount,
                          reserved ? &cull : nullptr);
    };
    // Billboard and beam streams are copied into this frame's scratch ring,
    // keeping every viewport independent.
    auto drawParticles = [&](std::span<iRenderable *> segment) {
      if (segment.empty() || !particlesReady)
        return;
      RICmd *cmd = &mpGraphics->primary.cmds[0];
      // Blending reads the existing scene color while writing the particle
      // result.  RENDER_TARGET is write-only and is not a valid dependency
      // for that read; retain the color-attachment layout with read|write
      // access for the rendering scope.
      cmd->vk_d3d12_textureBarrier(RITextureBarrier(
          state->renderTarget[index].Get(), RI_RESOURCE_STATE_SHADER_RESOURCE,
          RI_RESOURCE_STATE_RENDER_TARGET_READ, RI_STAGE_FRAGMENT,
          RI_STAGE_FRAGMENT, RI_BARRIER_ASPECT_COLOR));
      cmd->vk_d3d12_textureBarrier(RITextureBarrier(
          state->depthTextures[index].Get(), RI_RESOURCE_STATE_SHADER_RESOURCE,
          RI_RESOURCE_STATE_DEPTH_READ | RI_RESOURCE_STATE_SHADER_RESOURCE,
          RI_STAGE_FRAGMENT, RI_STAGE_ALL_GRAPHICS, RI_BARRIER_ASPECT_DEPTH));
      RIRenderingAttachment color = {};
      // renderTargetView is the SHADER_RESOURCE_2D view; D3D12 needs the
      // COLOR_ATTACHMENT view to build an RTV.
      color.view = *state->renderTargetAttachmentView[index];
      color.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
      color.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
      RIRenderingAttachment depth = {};
      depth.view = *state->depthView[index];
      depth.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
      depth.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
      depth.readOnly = true;
      RIBeginRenderingDesc begin = {};
      begin.renderArea.width = static_cast<int16_t>(state->width);
      begin.renderArea.height = static_cast<int16_t>(state->height);
      begin.colorCount = 1;
      begin.colors = &color;
      begin.depthStencil = &depth;
      cmd->vk_d3d12_beginRendering(&mpGraphics->device, begin);
      RIViewport vp = {};
      vp.width = static_cast<float>(state->width);
      vp.height = -static_cast<float>(state->height);
      vp.y = static_cast<float>(state->height);
      vp.depthMax = 1.0f;
      RIRect sc = {};
      sc.width = static_cast<int16_t>(state->width);
      sc.height = static_cast<int16_t>(state->height);
      cmd->setViewport(&mpGraphics->device, vp);
      cmd->setScissor(&mpGraphics->device, sc);
      m_particles->Render(cntx, cmd, mpGraphics->frameIndex, state->width,
                          state->height, state->renderTarget[index].Get(),
                          state->renderTargetView[index].Get(),
                          state->depthSampleView[index].Get(), apFrustum,
                          afFrameTime, segment, apWorld, particleSalt,
                          &frameBinding, fogBindings.front(), pointLights,
                          spotLights, pointLightCount, spotLightCount);
      cmd->vk_d3d12_endRendering(&mpGraphics->device);
      cmd->vk_d3d12_textureBarrier(
          RITextureBarrier(state->renderTarget[index].Get(),
                           RI_RESOURCE_STATE_RENDER_TARGET_READ,
                           RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT,
                           RI_STAGE_FRAGMENT, RI_BARRIER_ASPECT_COLOR));
      cmd->vk_d3d12_textureBarrier(RITextureBarrier(
          state->depthTextures[index].Get(),
          RI_RESOURCE_STATE_DEPTH_READ | RI_RESOURCE_STATE_SHADER_RESOURCE,
          RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_ALL_GRAPHICS,
          RI_STAGE_FRAGMENT, RI_BARRIER_ASPECT_DEPTH));
    };
    auto isParticleRun = [](iRenderable *object) {
      if (!object)
        return false;
      const eRenderableType type = object->GetRenderType();
      return type == eRenderableType_ParticleEmitter ||
             type == eRenderableType_Billboard || type == eRenderableType_Beam;
    };
    auto flush = [&](bool particleRun, std::span<iRenderable *> segment) {
      if (particleRun)
        drawParticles(segment);
      else
        drawOrdinary(segment);
    };

    // Depth pyramid for the camera occlusion cull. Built here, after the
    // opaque pass has finished writing depth and before anything translucent
    // is recorded, so the pyramid the cull tests against is THIS frame's final
    // opaque depth -- exact, with none of the temporal error a two-phase
    // opaque cull has to carry.
    //
    // Depth is in SHADER_RESOURCE at this point (the barrier above leaves it
    // there), which is what the build reads it as.
    if (m_hiZ && m_hiZLoaded && !state->depthSampleView[index].isEmpty()) {
      hiZBuilt = m_hiZ->Build(&mpGraphics->primary.cmds[0],
                              mpGraphics->frameIndex, state->hiZ, index,
                              state->width, state->height,
                              state->depthSampleView[index].Get());
    }

    auto translucent =
        m_rendererList.GetRenderableItems(eRenderListType_Translucent);
    size_t begin = 0;
    bool currentParticleRun = false;
    for (size_t i = 0; i < translucent.size(); ++i) {
      iRenderable *object = translucent[i];
      if (object && object->GetMaterial() &&
          object->GetMaterial()->GetMaterialID() == MaterialID::Water) {
        flush(currentParticleRun, translucent.subspan(begin, i - begin));
        if (waterReady)
          m_water->RecordSurface(
              cntx, state, index, object, apFrustum, apWorld, &frameBinding,
              &fogBindings.front(), &pointLights, &spotLights, pointLightCount,
              spotLightCount, pointLightCountTotal, spotLightCountTotal,
              standardShadowView.Get(), m_rendererList.GetFogAreas(),
              &boxLights, boxLightCount);
        begin = i + 1;
        continue;
      }
      const bool particleRun = isParticleRun(object);
      if (particleRun != currentParticleRun) {
        flush(currentParticleRun, translucent.subspan(begin, i - begin));
        begin = i;
        currentParticleRun = particleRun;
      }
    }
    flush(currentParticleRun, translucent.subspan(begin));
  }

  // Every Standard pass above leaves the render depth in SHADER_RESOURCE.
  handBackDepth();

  // DebugDraw overlay (editor grid / gizmos / icons and in-game debug lines,
  // queued by OnPreWorldDraw callbacks). The legacy deferred renderer flushed
  // debug draws into its output after the solid and translucent passes; draw
  // into the finished scene against the scene depth and leave the render
  // target in SHADER_READ with depth in DEPTH_WRITE, as Draw's contract requires.
  DebugDraw *debugDraw = mpGraphics->GetDebugDraw();
  if (debugDraw && debugDraw->HasRequests()) {
    RICmd *cmd = &mpGraphics->primary.cmds[0];
    cmd->vk_d3d12_textureBarrier(RITextureBarrier(
        state->renderTarget[index].Get(), RI_RESOURCE_STATE_SHADER_RESOURCE,
        RI_RESOURCE_STATE_RENDER_TARGET_READ, RI_STAGE_FRAGMENT,
        RI_STAGE_FRAGMENT, RI_BARRIER_ASPECT_COLOR));
    RIRenderingAttachment color = {};
    color.view = *state->renderTargetAttachmentView[index];
    color.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
    RIRenderingAttachment depth = {};
    depth.view = *state->depthView[index];
    depth.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
    depth.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
    RIBeginRenderingDesc begin = {};
    begin.renderArea.width = static_cast<int16_t>(state->width);
    begin.renderArea.height = static_cast<int16_t>(state->height);
    begin.colorCount = 1;
    begin.colors = &color;
    begin.depthStencil = &depth;
    cmd->vk_d3d12_beginRendering(&mpGraphics->device, begin);
    debugDraw->flush(cntx, cmd, apFrustum, state->width, state->height,
                     cGraphics::PogoColorFormat);
    cmd->vk_d3d12_endRendering(&mpGraphics->device);
    cmd->vk_d3d12_textureBarrier(
        RI_PogoShaderBarrier(state->renderTarget[index].Get(), false));
  }
}

} // namespace hpl
