#include "graphics/RID3D12.h"

#if DEVICE_IMPL_D3D12

#include "graphics/RIDevice.h"
#include "graphics/RICommand.h"
#include "graphics/RIBuffer.h"
#include "graphics/RIFormat.h"
#include "graphics/RITexture.h"
#include "system/LowLevelSystem.h"

struct RID3D12BarrierSubresourceRange {
  uint32_t baseMip;
  uint32_t mipCount;
  uint32_t baseLayer;
  uint32_t layerCount;
  UINT firstPlane;
  UINT planeCount;
  UINT resourcePlaneCount;
  bool wholeResource;
};

static bool ri_d3d12_GetBarrierSubresourceRange(
    const RITextureBarrier &src, RID3D12BarrierSubresourceRange &out) {
  const RIFormatProps *props = GetRIFormatProps(src.texture->format);
  const uint32_t mipNum = src.texture->d3d12.mipNum;
  const uint32_t layerNum = src.texture->d3d12.layerNum;
  if (!mipNum || !layerNum)
    return false;

  const bool hasDepth = props->isDepth != 0;
  const bool hasStencil = props->isStencil != 0;
  out.resourcePlaneCount = (hasDepth && hasStencil) ? 2 : 1;
  switch (src.aspect) {
  case RI_BARRIER_ASPECT_COLOR:
    if (hasDepth || hasStencil)
      return false;
    out.firstPlane = 0;
    out.planeCount = 1;
    break;
  case RI_BARRIER_ASPECT_DEPTH:
    if (!hasDepth)
      return false;
    // A combined depth/stencil resource is two D3D12 planes, and D3D12 requires
    // every plane of a DSV to be in the depth state when it is bound. The
    // shared call sites all pass ASPECT_DEPTH because Vulkan (without
    // separateDepthStencilLayouts) transitions both aspects for a depth-only
    // aspectMask; covering both planes here keeps that contract. Leaving plane
    // 1 out left the stencil plane in COMMON for the resource's lifetime.
    out.firstPlane = 0;
    out.planeCount = hasStencil ? 2 : 1;
    break;
  case RI_BARRIER_ASPECT_STENCIL:
    if (!hasStencil)
      return false;
    out.firstPlane = hasDepth ? 1 : 0;
    out.planeCount = 1;
    break;
  case RI_BARRIER_ASPECT_DEPTH_STENCIL:
    if (!hasDepth || !hasStencil)
      return false;
    out.firstPlane = 0;
    out.planeCount = 2;
    break;
  default:
    assert(false);
    return false;
  }

  if (src.baseMip >= mipNum || src.baseLayer >= layerNum)
    return false;
  out.baseMip = src.baseMip;
  out.baseLayer = src.baseLayer;
  out.mipCount = src.mipCount ? src.mipCount : mipNum - out.baseMip;
  out.layerCount = src.layerCount ? src.layerCount : layerNum - out.baseLayer;
  if (out.mipCount > mipNum - out.baseMip ||
      out.layerCount > layerNum - out.baseLayer)
    return false;
  out.wholeResource = out.baseMip == 0 && out.mipCount == mipNum &&
                      out.baseLayer == 0 && out.layerCount == layerNum;
  return true;
}

// A rejected range means the barrier is dropped outright, which on D3D12 shows
// up only as corrupt output much later. RITextureBarrier defaults its aspect to
// COLOR, so a call site that forgets the aspect on a depth resource lands here
// and would otherwise be invisible. Reported once — these repeat every frame.
static void ri_d3d12_WarnBarrierDropped(const RITextureBarrier &src) {
  static bool sbWarned = false;
  if (sbWarned)
    return;
  sbWarned = true;
  hpl::Warning("RI D3D12: dropped a texture barrier (aspect %u, format %u, "
               "baseMip %u mipCount %u baseLayer %u layerCount %u); the "
               "resource will not be transitioned\n",
               unsigned(src.aspect), unsigned(src.texture->format),
               src.baseMip, src.mipCount, src.baseLayer, src.layerCount);
}

// True when `outer` covers every mip, layer and plane `inner` names.
static bool
ri_d3d12_RangeContains(const RID3D12BarrierSubresourceRange &outer,
                       const RID3D12BarrierSubresourceRange &inner) {
  return outer.firstPlane <= inner.firstPlane &&
         outer.firstPlane + outer.planeCount >=
             inner.firstPlane + inner.planeCount &&
         outer.baseMip <= inner.baseMip &&
         outer.baseMip + outer.mipCount >= inner.baseMip + inner.mipCount &&
         outer.baseLayer <= inner.baseLayer &&
         outer.baseLayer + outer.layerCount >=
             inner.baseLayer + inner.layerCount;
}

// Reported once; like the dropped-barrier warning, this would repeat every
// frame otherwise.
static void ri_d3d12_WarnBarrierSubsumed(const RITextureBarrier &src,
                                         const RITextureBarrier &earlier) {
  static bool sbWarned = false;
  if (sbWarned)
    return;
  sbWarned = true;
  hpl::Warning("RI D3D12: two barriers in one batch cover the same "
               "subresources of format %u with different after-states "
               "(aspect %u -> state %u, then aspect %u -> state %u); the "
               "first one wins\n",
               unsigned(src.texture->format), unsigned(earlier.aspect),
               earlier.after, unsigned(src.aspect), src.after);
}

// D3D12 has no per-plane resource state for a bound DSV, so ASPECT_DEPTH above
// deliberately widens to both planes of a combined depth/stencil resource. A
// call site that also issues the matching ASPECT_STENCIL barrier -- which
// Vulkan needs, because it takes the two aspects to DEPTH_ATTACHMENT_OPTIMAL
// and STENCIL_ATTACHMENT_OPTIMAL separately -- therefore names plane 1 twice
// in one batch.
//
// The second barrier is not merely redundant. Barriers in a group are
// validated in order, so the stencil one declares SyncBefore NONE / AccessBefore
// NO_ACCESS (what RI_RESOURCE_STATE_UNDEFINED translates to) for a subresource
// the widened depth barrier just touched, and the debug layer rejects that:
// "subresource [1] has been accessed before, a barrier that specifies
// D3D12_BARRIER_SYNC_NONE as SyncBefore is invalid" (INVALID_BARRIER_ACCESS
// #1417). The legacy path is no better off -- it would transition plane 1 out
// of a before-state it no longer holds. The widened barrier already leaves the
// plane in the state the DSV needs, so drop the later one.
static bool
ri_d3d12_IsBarrierSubsumed(const RITextureBarrier *textureBarriers,
                           uint32_t index,
                           const RID3D12BarrierSubresourceRange &range) {
  const RITextureBarrier &src = textureBarriers[index];
  for (uint32_t j = 0; j < index; ++j) {
    const RITextureBarrier &earlier = textureBarriers[j];
    if (earlier.texture != src.texture)
      continue;
    RID3D12BarrierSubresourceRange earlierRange = {};
    if (!ri_d3d12_GetBarrierSubresourceRange(earlier, earlierRange))
      continue;
    if (!ri_d3d12_RangeContains(earlierRange, range))
      continue;
    // Same subresources, different destination: one of the two is a call-site
    // bug rather than the depth/stencil aspect split this exists for. Dropping
    // is still the only legal option, but say so.
    if (earlier.after != src.after)
      ri_d3d12_WarnBarrierSubsumed(src, earlier);
    return true;
  }
  return false;
}

// The state a not-yet-initialized attachment must be in for DiscardResource:
// D3D12 accepts a discard only in RENDER_TARGET or DEPTH_WRITE.
static uint32_t ri_d3d12_InitializationState(const RITexture &texture) {
  return (texture.d3d12.usage & RI_USAGE_DEPTH_STENCIL_ATTACHMENT)
             ? RI_RESOURCE_STATE_DEPTH_WRITE
             : RI_RESOURCE_STATE_RENDER_TARGET;
}

// D3D12MA hands out placed resources, and a placed render-target / depth-stencil
// resource must be initialized by a Discard, Clear or Copy before anything else
// touches it -- otherwise ExecuteCommandLists fails validation with
// RENDER_TARGET_OR_DEPTH_STENCIL_RESOUCE_NOT_INITIALIZED (id 1422), and on real
// hardware the compression metadata is genuinely undefined. Neither a compute
// UAV write nor a LOAD_OP_LOAD attachment bind counts, and the engine has
// targets whose first attachment use is LOAD (the pogo halves are compute-written
// first), so the discard has to happen before the producing pass, not at the bind.
//
// The texture's first barrier is that point: every attachment reaches its first
// use through one, and at that moment its contents are undefined by the RI
// contract anyway. Transition it to the discardable state, discard the whole
// resource (a null region covers every mip, slice and plane -- including the
// stencil plane, which ClearDepthStencilView's DEPTH-only flag never touches),
// and let the caller's barrier run from there.
//
// Both the transition into the discardable state and the one back out are
// whole-resource, since DiscardResource with a null region is. When the
// caller's own barrier also covers the whole resource, the discard just leaves
// it in the discardable state and `initialized[i]` tells the main pass to start
// from there. When it covers only a subrange, the resource is put back the way
// the caller believes it to be, so the main pass needs no adjustment.
static void ri_d3d12_TransitionWholeResource(RICmd &cmd, RITexture &texture,
                                             uint32_t before, uint32_t after) {
  if (cmd.d3d12.enhancedBarriersSupported && cmd.d3d12.cmdList7) {
    D3D12_TEXTURE_BARRIER barrier = {};
    barrier.SyncBefore = ri_d3d12_RIStageMaskFromStateBarrier(before);
    barrier.SyncAfter = ri_d3d12_RIStageMaskFromStateBarrier(after);
    barrier.AccessBefore = ri_d3d12_RIResourceStateToBarrierAccess(before);
    barrier.AccessAfter = ri_d3d12_RIResourceStateToBarrierAccess(after);
    barrier.LayoutBefore = ri_d3d12_RIResourceStateToBarrierLayout(before);
    // UNDEFINED is only ever a LayoutBefore. Land on COMMON instead; the
    // caller's barrier may then declare UNDEFINED as its own LayoutBefore,
    // which is legal from any layout.
    barrier.LayoutAfter = after == RI_RESOURCE_STATE_UNDEFINED
                              ? D3D12_BARRIER_LAYOUT_COMMON
                              : ri_d3d12_RIResourceStateToBarrierLayout(after);
    barrier.pResource = texture.d3d12.resource;
    // 0xffffffff in IndexOrFirstMipLevel selects every subresource.
    barrier.Subresources.IndexOrFirstMipLevel = 0xffffffff;
    barrier.Flags = D3D12_TEXTURE_BARRIER_FLAG_NONE;
    D3D12_BARRIER_GROUP group = {};
    group.Type = D3D12_BARRIER_TYPE_TEXTURE;
    group.NumBarriers = 1;
    group.pTextureBarriers = &barrier;
    cmd.d3d12.cmdList7->Barrier(1, &group);
    ++g_riD3D12EnhancedBarrierCallCount;
    return;
  }
  const D3D12_RESOURCE_STATES from = ri_d3d12_RIResourceStateToStates(before);
  const D3D12_RESOURCE_STATES to = ri_d3d12_RIResourceStateToStates(after);
  if (from == to)
    return;
  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition = {texture.d3d12.resource,
                        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
  cmd.d3d12.cmdList->ResourceBarrier(1, &barrier);
  ++g_riD3D12LegacyBarrierCallCount;
}

// `initialized[i]` records which barriers were left in the discardable state so
// the main pass can substitute the post-discard before-state.
static void ri_d3d12_InitializeAttachments(RICmd &cmd,
                                           uint32_t textureBarrierNum,
                                           const RITextureBarrier *textureBarriers,
                                           std::vector<uint8_t> &initialized) {
  bool any = false;
  for (uint32_t i = 0; i < textureBarrierNum; ++i) {
    const RITextureBarrier &src = textureBarriers[i];
    RITexture *texture = src.texture;
    if (!texture || !texture->d3d12.needsInitialization ||
        !texture->d3d12.resource)
      continue;
    // Claim it before recording: two barriers on the same texture in one batch
    // must only produce one discard.
    texture->d3d12.needsInitialization = 0;
    // A simultaneous-access texture's layout is pinned to COMMON, so it can
    // never reach RENDER_TARGET / DEPTH_WRITE. None carry attachment usage, so
    // rule 1422 does not apply to them either; nothing to do.
    if (texture->d3d12.usage & RI_USAGE_SIMULTANEOUS_ACCESS)
      continue;

    const uint32_t state = ri_d3d12_InitializationState(*texture);
    ri_d3d12_TransitionWholeResource(cmd, *texture, src.before, state);
    cmd.d3d12.cmdList->DiscardResource(texture->d3d12.resource, nullptr);

    RID3D12BarrierSubresourceRange range = {};
    const bool wholeResource =
        ri_d3d12_GetBarrierSubresourceRange(src, range) && range.wholeResource &&
        range.firstPlane == 0 && range.planeCount == range.resourcePlaneCount;
    if (!wholeResource) {
      ri_d3d12_TransitionWholeResource(cmd, *texture, state, src.before);
      continue;
    }
    if (!any) {
      initialized.assign(textureBarrierNum, 0);
      any = true;
    }
    initialized[i] = 1;
  }
}

void RID3D12_ResourceBarrier(RICmd &cmd, uint32_t memoryBarrierNum,
                             const RIMemoryBarrier *memoryBarriers,
                             uint32_t bufferBarrierNum,
                             const RIBufferBarrier *bufferBarriers,
                             uint32_t textureBarrierNum,
                             const RITextureBarrier *textureBarriers) {
  if (memoryBarrierNum + bufferBarrierNum + textureBarrierNum == 0)
    return;
  // Empty unless a texture in this batch was discarded just now; an entry of 1
  // means that barrier's before-state is the discardable state, not src.before.
  std::vector<uint8_t> initialized;
  if (cmd.d3d12.cmdList)
    ri_d3d12_InitializeAttachments(cmd, textureBarrierNum, textureBarriers,
                                   initialized);
  auto beforeStateOf = [&](uint32_t i, const RITextureBarrier &src) {
    return (i < initialized.size() && initialized[i])
               ? ri_d3d12_InitializationState(*src.texture)
               : src.before;
  };
  if (cmd.d3d12.enhancedBarriersSupported && cmd.d3d12.cmdList7) {
    std::vector<D3D12_GLOBAL_BARRIER> globals(memoryBarrierNum);
    std::vector<D3D12_BUFFER_BARRIER> buffers(bufferBarrierNum);
    std::vector<D3D12_TEXTURE_BARRIER> textures(textureBarrierNum);
    uint32_t textureBarrierCount = 0;
    for (uint32_t i = 0; i < memoryBarrierNum; ++i) {
      const RIMemoryBarrier &src = memoryBarriers[i];
      globals[i] = {ri_d3d12_RIStageBitsToBarrierSync(src.beforeStages, src.before),
                    ri_d3d12_RIStageBitsToBarrierSync(src.afterStages, src.after),
                    ri_d3d12_RIResourceStateToBarrierAccess(src.before),
                    ri_d3d12_RIResourceStateToBarrierAccess(src.after)};
    }
    for (uint32_t i = 0; i < bufferBarrierNum; ++i) {
      const RIBufferBarrier &src = bufferBarriers[i];
      buffers[i] = {ri_d3d12_RIStageBitsToBarrierSync(src.beforeStages, src.before),
                    ri_d3d12_RIStageBitsToBarrierSync(src.afterStages, src.after),
                    ri_d3d12_RIResourceStateToBarrierAccess(src.before),
                    ri_d3d12_RIResourceStateToBarrierAccess(src.after),
                     src.buffer->d3d12.resource, src.offset,
                     src.size ? src.size : UINT64_MAX};
    }
    for (uint32_t i = 0; i < textureBarrierNum; ++i) {
      const RITextureBarrier &src = textureBarriers[i];
      RID3D12BarrierSubresourceRange range = {};
      if (!ri_d3d12_GetBarrierSubresourceRange(src, range)) {
        ri_d3d12_WarnBarrierDropped(src);
        continue;
      }
      if (ri_d3d12_IsBarrierSubsumed(textureBarriers, i, range))
        continue;
      // A texture initialized above left the discard in RENDER_TARGET /
      // DEPTH_WRITE, so that -- not src.before -- is where this barrier starts.
      const uint32_t before = beforeStateOf(i, src);
      D3D12_TEXTURE_BARRIER &dst = textures[textureBarrierCount++] = {};
      dst.SyncBefore = ri_d3d12_RIStageBitsToBarrierSync(src.beforeStages, before);
      dst.SyncAfter = ri_d3d12_RIStageBitsToBarrierSync(src.afterStages, src.after);
      dst.AccessBefore = ri_d3d12_RIResourceStateToBarrierAccess(before);
      dst.AccessAfter = ri_d3d12_RIResourceStateToBarrierAccess(src.after);
      dst.LayoutBefore = ri_d3d12_RIResourceStateToBarrierLayout(before);
      dst.LayoutAfter = ri_d3d12_RIResourceStateToBarrierLayout(src.after);
      // A simultaneous-access texture's layout is immutable: the debug layer
      // rejects any other value outright ("Invalid LayoutBefore
      // D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS for simultaneous access
      // texture"). Barriers on one therefore only ever carry an
      // execution/memory dependency, which is exactly what lets it hold
      // SHADER_RESOURCE and UNORDERED_ACCESS together -- no ordinary layout
      // admits both, and that is the whole reason for the flag. The cost is
      // that it can never be UAV-cleared; see clearStorageImage.
      if (src.texture->d3d12.usage & RI_USAGE_SIMULTANEOUS_ACCESS) {
        dst.LayoutBefore = D3D12_BARRIER_LAYOUT_COMMON;
        dst.LayoutAfter = D3D12_BARRIER_LAYOUT_COMMON;
        // An UNDEFINED before-state maps to NO_ACCESS, which D3D12 only pairs
        // with LAYOUT_UNDEFINED or SYNC_NONE (#1331). With the layout pinned,
        // a barrier that must wait on earlier work (SYNC_NONE is rejected once
        // the texture has been accessed in the list, #1417) expresses the
        // discard as ACCESS_COMMON: every access the COMMON layout admits.
        if (dst.AccessBefore == D3D12_BARRIER_ACCESS_NO_ACCESS &&
            dst.SyncBefore != D3D12_BARRIER_SYNC_NONE)
          dst.AccessBefore = D3D12_BARRIER_ACCESS_COMMON;
      }
      dst.pResource = src.texture->d3d12.resource;
      dst.Subresources.IndexOrFirstMipLevel = range.baseMip;
      dst.Subresources.NumMipLevels = range.mipCount;
      dst.Subresources.FirstArraySlice = range.baseLayer;
      dst.Subresources.NumArraySlices = range.layerCount;
      dst.Subresources.FirstPlane = range.firstPlane;
      dst.Subresources.NumPlanes = range.planeCount;
      dst.Flags = D3D12_TEXTURE_BARRIER_FLAG_NONE;
    }
    textures.resize(textureBarrierCount);
    D3D12_BARRIER_GROUP groups[3] = {};
    UINT groupCount = 0;
    if (!globals.empty()) {
      groups[groupCount].Type = D3D12_BARRIER_TYPE_GLOBAL;
      groups[groupCount].NumBarriers = (UINT)globals.size();
      groups[groupCount].pGlobalBarriers = globals.data();
      ++groupCount;
    }
    if (!buffers.empty()) {
      groups[groupCount].Type = D3D12_BARRIER_TYPE_BUFFER;
      groups[groupCount].NumBarriers = (UINT)buffers.size();
      groups[groupCount].pBufferBarriers = buffers.data();
      ++groupCount;
    }
    if (!textures.empty()) {
      groups[groupCount].Type = D3D12_BARRIER_TYPE_TEXTURE;
      groups[groupCount].NumBarriers = (UINT)textures.size();
      groups[groupCount].pTextureBarriers = textures.data();
      ++groupCount;
    }
    if (groupCount) {
      cmd.d3d12.cmdList7->Barrier(groupCount, groups);
      ++g_riD3D12EnhancedBarrierCallCount;
    }
    return;
  }

  size_t barrierBudget = memoryBarrierNum + bufferBarrierNum;
  for (uint32_t i = 0; i < textureBarrierNum; ++i) {
    const RITextureBarrier &src = textureBarriers[i];
    RID3D12BarrierSubresourceRange range = {};
    if (!ri_d3d12_GetBarrierSubresourceRange(src, range) ||
        ri_d3d12_IsBarrierSubsumed(textureBarriers, i, range))
      continue;
    barrierBudget += (range.wholeResource &&
                      range.firstPlane == 0 &&
                      range.planeCount == range.resourcePlaneCount) ? 1 :
                     static_cast<size_t>(range.mipCount) * range.layerCount *
                         range.planeCount;
  }
  std::vector<D3D12_RESOURCE_BARRIER> barriers;
  barriers.reserve(barrierBudget);
  for (uint32_t i = 0; i < memoryBarrierNum; ++i)
    barriers.push_back({D3D12_RESOURCE_BARRIER_TYPE_UAV, D3D12_RESOURCE_BARRIER_FLAG_NONE,
                        {nullptr}});
  for (uint32_t i = 0; i < bufferBarrierNum; ++i) {
    const RIBufferBarrier &src = bufferBarriers[i];
    const D3D12_RESOURCE_STATES before = ri_d3d12_RIResourceStateToStates(src.before);
    const D3D12_RESOURCE_STATES after = ri_d3d12_RIResourceStateToStates(src.after);
    D3D12_RESOURCE_BARRIER dst = {};
    if (before == after) { dst.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; dst.UAV.pResource = src.buffer->d3d12.resource; }
    else { dst.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; dst.Transition = {src.buffer->d3d12.resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after}; }
    barriers.push_back(dst);
  }
  for (uint32_t i = 0; i < textureBarrierNum; ++i) {
    const RITextureBarrier &src = textureBarriers[i];
    RID3D12BarrierSubresourceRange range = {};
    if (!ri_d3d12_GetBarrierSubresourceRange(src, range)) {
      ri_d3d12_WarnBarrierDropped(src);
      continue;
    }
    if (ri_d3d12_IsBarrierSubsumed(textureBarriers, i, range))
      continue;
    // See the enhanced path: an initialized texture starts from the state the
    // discard left it in.
    const bool wasInitialized = i < initialized.size() && initialized[i];
    const D3D12_RESOURCE_STATES before =
        ri_d3d12_RIResourceStateToStates(beforeStateOf(i, src));
    const D3D12_RESOURCE_STATES after = ri_d3d12_RIResourceStateToStates(src.after);
    // Only a genuine same-state barrier is a UAV barrier. When the discard
    // already left the resource in the requested state there is nothing to do,
    // and a UAV barrier on a non-UAV resource would be invalid.
    if (before == after && wasInitialized) continue;
    if (before == after) { D3D12_RESOURCE_BARRIER dst = {}; dst.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; dst.UAV.pResource = src.texture->d3d12.resource; barriers.push_back(dst); continue; }
    const uint32_t mipNum = src.texture->d3d12.mipNum;
    const uint32_t layerNum = src.texture->d3d12.layerNum;
    if (range.wholeResource && range.firstPlane == 0 &&
        range.planeCount == range.resourcePlaneCount) {
      D3D12_RESOURCE_BARRIER dst = {};
      dst.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      dst.Transition = {src.texture->d3d12.resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
      barriers.push_back(dst);
    } else {
      for (uint32_t plane = 0; plane < range.planeCount; ++plane)
        for (uint32_t layer = 0; layer < range.layerCount; ++layer)
          for (uint32_t mip = 0; mip < range.mipCount; ++mip) {
            D3D12_RESOURCE_BARRIER dst = {};
            dst.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            dst.Transition = {src.texture->d3d12.resource,
                              range.baseMip + mip + (range.baseLayer + layer) * mipNum +
                                  (range.firstPlane + plane) * mipNum * layerNum,
                              before, after};
            barriers.push_back(dst);
          }
    }
  }
  if (!barriers.empty()) {
    cmd.d3d12.cmdList->ResourceBarrier((UINT)barriers.size(), barriers.data());
    ++g_riD3D12LegacyBarrierCallCount;
  }
}

#endif
