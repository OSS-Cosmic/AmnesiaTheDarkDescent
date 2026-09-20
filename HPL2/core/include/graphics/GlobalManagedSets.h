#ifndef HPL_GLOBAL_MANAGED_SETS_H
#define HPL_GLOBAL_MANAGED_SETS_H

#include "graphics/BindlessPool.h"
#include "graphics/Graphics.h"
#include "graphics/RIProgram.h"
#include "graphics/RIBuffer.h"
#include "graphics/RIRenderer.h"
#include "graphics/RITypes.h"
#include "math/MathTypes.h"
#include "resources/ResourceBase.h"
#include "system/Event.h"
#include "system/Hasher.h"

#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <vector>

#include "Constants.h"
#include "SceneTypes.slang"

namespace hpl {

class Image;
class cMaterial;
class cResources;
class cVertexBuffer;

// CPU shadow of a device-local bindless slot buffer (m_opaque*Handles /
// m_bindlessSlotGenerationBuffer). Those buffers have no mapped pointer, so
// per-slot host writes land here and flushMirrors() stages the dirty byte range
// into the device buffer once per frame. write<T>() addresses slot i at
// i*sizeof(T) (matching CreateBindlessSlotBuffer's layout) and widens the dirty
// range; markAllDirty() stages the whole shadow on the first frame to seed the
// device == shadow invariant.
struct BindlessShadowMirror {
  std::vector<uint8_t> shadow;
  size_t dirtyMinByte = 0;
  size_t dirtyMaxByte = 0;

  // slotCount * elemSize bytes, zero-filled; dirty range starts empty.
  void init(size_t slotCount, size_t elemSize) {
    shadow.assign(slotCount * elemSize, uint8_t(0));
    clearDirty();
  }

  // Store `value` at the slot's byte offset and grow the dirty range. An
  // out-of-range slot is dropped (mirrors GPU robust-access) rather than
  // overrunning the shadow.
  template <typename T> void write(uint32_t slot, T value) {
    const size_t off = (size_t)slot * sizeof(T);
    if (off + sizeof(T) > shadow.size())
      return;
    // Unchanged value: the device buffer already holds it (shadow mirrors
    // device), so don't widen the dirty range — keeps flushMirrors() from
    // re-uploading BDAs / slot generations that didn't move this frame.
    if (std::memcmp(shadow.data() + off, &value, sizeof(T)) == 0)
      return;
    std::memcpy(shadow.data() + off, &value, sizeof(T));
    if (off < dirtyMinByte)
      dirtyMinByte = off;
    if (off + sizeof(T) > dirtyMaxByte)
      dirtyMaxByte = off + sizeof(T);
  }

  void markAllDirty() {
    dirtyMinByte = 0;
    dirtyMaxByte = shadow.size();
  }

  // Empty range encoded as min > max so hasDirty() is false and a subsequent
  // write() always lowers min / raises max.
  void clearDirty() {
    dirtyMinByte = shadow.size();
    dirtyMaxByte = 0;
  }

  bool hasDirty() const { return dirtyMaxByte > dirtyMinByte; }
};

namespace detail {

// Allocate a bindless slot buffer: slotCount * elementStride bytes. When
// deviceLocalOnly the allocation has no host-mapped pointer (out.mappedAddress
// is null) and must be seeded through Interface<cGraphics>::Get()->uploader; otherwise it is persistently
// mapped for direct host writes. Shared by GlobalManagedSets (set-0
// buffers) and cHybridRenderer (the indirect-draw buffer), so it lives in this
// header rather than a single .cpp.
static inline struct RIBuffer
CreateBindlessSlotBuffer(RIDevice *device, uint64_t slotCount,
                         size_t elementStride, uint32_t usage,
                         bool deviceLocalOnly = false,
                         const char *debugName = nullptr) {
  struct RIBuffer out;
  if (elementStride == 0 ||
      slotCount > std::numeric_limits<uint64_t>::max() /
                      uint64_t(elementStride)) {
    Warning("Bindless slot buffer size overflows the RI size contract\n");
    return out;
  }
  const uint64_t size = slotCount * uint64_t(elementStride);
  if (size == 0) {
    Warning("Bindless slot buffer size must be non-zero\n");
    return out;
  }
  out = RIBuffer::create(device, {size, usage,
                                  deviceLocalOnly ? RI_MEMORY_DEVICE
                                                  : RI_MEMORY_HOST_UPLOAD,
                                  0});
  if (debugName && !out.isEmpty())
    out.setDebugObjectName(device, debugName);
  return out;
}
} // namespace detail

// Per-object inputs for submitObject(): the part that varies per pass. The
// class does the matrix transpose / invert / uv-transpose + memcpy into the GPU
// UniformObject. Matrices are engine cMatrixf (row-major); modelMatrix == null
// means identity. Material sites set uvMatrix to pMat->GetUvMatrix(); others
// leave it identity. materialId comes from submitMaterial().
struct ObjectSubmitDesc {
  const cMatrixf *modelMatrix = nullptr;
  cMatrixf        uvMatrix    = cMatrixf::Identity;
  uint32_t        materialId  = 0;
  float           dissolveAmount     = 0.0f;
  float           illuminationAmount = 0.0f;
  uint32_t        decalList   = 0;
  uint32_t        renderFlags = 0;

  // Explicit per-stream vertex/index buffer references, folded into the
  // UniformObject. When `set`, submitObject uses these verbatim instead of
  // deriving from `vb` — particles point them at the per-viewport translucent
  // scratch ring (normal/tangent = 0). Leave `set` false to derive from `vb`
  // (kSubmitVertex/kSubmitIndex), or to carry forward the slot's existing
  // handles (data-only passes that bind fixed-function vertex/index buffers).
  // The references are borrowed; their buffers must remain alive through GPU use.
  struct StreamRefs {
    struct Ref {
      RIBuffer *buffer = nullptr;
      uint64_t  byteOffset = 0;
    } pos, normal, tangent, uv0, color, index;
    bool set = false;
  } streamRefs;

  // Legacy packed stream handles remain supported for Vulkan callers only.
  struct StreamHandles {
    uint64_t pos = 0, normal = 0, tangent = 0, uv0 = 0, color = 0, index = 0;
    bool     set = false;
  } streamHandles;
};

// What submitObject() writes for the slot this frame. Slot allocation + the
// reuse-generation bump always run; these gate the per-slot data writes. The
// fixed-function raster passes (decal/water/translucent) pass kSubmitData only —
// they bind fixed-function vertex/index buffers and never read the opaque*Handles. The bindless-pull
// passes (solids/TLAS/particle) add kSubmitVertex | kSubmitIndex.
enum ObjectSubmitFlags : uint32_t {
  kSubmitData   = 1u << 0, // stage the UniformObject payload into m_objectBuffer[slot]
  kSubmitVertex = 1u << 1, // write pos/normal/tangent/color/uv stream BDAs (absent → 0)
  kSubmitIndex  = 1u << 2, // write the index-buffer BDA (absent → 0)
};

// Owner of the renderer's single global descriptor set — set 0, the bindless
// RIBindlessDescriptorSet that nearly every GPU buffer is bound to. Builds the
// binding layout, creates + seeds every set-0 buffer, writes them into their
// descriptor slots, resolves textures / materials into bindless slots, and
// stages per-frame host writes through CPU shadow mirrors. cHybridRenderer holds
// one of these and pushes data into it; the members are public so the per-frame
// fill / copy / bind sites in Draw() reach them directly.
class GlobalManagedSets {
public:
  GlobalManagedSets();
  // Defined out-of-line in the .cpp (where Image is complete) so the
  // SharedResourceHandle<Image> member destructors instantiate there, keeping
  // owners that hold this set by value free of an Image.h dependency.
  ~GlobalManagedSets();

  // Build the descriptor-set layout + pool, create and seed all set-0 buffers,
  // and run the one-time descriptor write batch.
  bool initialize(RIDevice *device, cResources *resources);

  // Destroy all owned buffers and the descriptor set.
  void destroy(RIDevice *device);

  // Result of submitMaterial: the object's flat material id (index into the
  // gMaterials table), stored in UniformObject.materialID. materialId is
  // UINT32_MAX when the material pool is exhausted.
  struct MaterialSubmitResult {
    uint32_t materialId = 0;
  };

  // Resolve + upload `mat`'s GPU material entry. Builds the typed material struct
  // (DiffuseMaterial / TranslucentMaterial / WaterMaterial), packs it into a
  // fixed-size MaterialDataBlob, and uploads it to m_materialBuffer[slot] (Falcor
  // MaterialSystem model — one table, type tag in the blob header). Allocates a
  // slot on first sight, re-uploads when the material's generation differs from
  // the cached one. materialId is UINT32_MAX when the pool is exhausted.
  MaterialSubmitResult submitMaterial(cGraphics::FrameContext *cntx,
                                      cMaterial *mat, uint32_t frameIndex);

  // Texture bindless slots are no longer resolved here. cTextureManager owns the
  // texture heap (slot pools + descriptor writes) and stamps each Image with a
  // lifetime-stable slot at load; consumers read img->GetBindlessSlot() directly
  // (binding 0 = textures_2d[], binding 1 = textures_cube[]).

  // Stage every dirty mirror into its matching device-local buffer. Called once
  // at the end of Draw() so all bindless handle / slot-generation writes land
  // ahead of the primary submit's reads regardless of recording order.
  void flushMirrors(RIDevice *device);

  // Own the object's stable slot and stage its payload. Finds/allocates the slot
  // for `objectCookie` (stable per object — keyed on the renderable's unique
  // cookie, NOT its transform, so a moving object keeps its slot), then builds a
  // UniformObject from `desc` and stages it into m_objectBuffer[slot] every
  // frame. Bumps the slot's reuse
  // generation when the slot is (re)assigned to a different object or `vb`'s index
  // count changes (the only `primitiveIndex >= triangleCount` fault), and binds a
  // VB-destroy hook on a fresh slot that bumps the generation when the geometry is
  // freed. Returns the slot, or UINT32_MAX when the pool is exhausted this frame.
  uint32_t submitObject(uint64_t objectCookie, uint32_t frameIndex,
                        cVertexBuffer *vb, const ObjectSubmitDesc &desc,
                        uint32_t flags = kSubmitData);

  // === Public state (same names as the former cHybridRenderer members) ===

  RIBindlessDescriptorSet m_bindlessSet;

  // Scene-object table (set 0, kBindingSceneObjects): one UniformObject per
  // assigned object slot, filled per-frame in Draw() from the object cache.
  struct RIBuffer m_objectBuffer;

  // Per-object-slot state in the object cache (m_objectSlots). The cache hands a
  // stable slot per object (keyed on the renderable's unique cookie); this rides
  // along so submitObject can detect a topology change (index count) and bump the
  // slot's reuse generation when the geometry is destroyed.
  struct ObjectSlotState {
    EventHandler<> onDestroy;     // VB-destroy → bump this slot's generation
    uint32_t       indexCount = 0; // last-seen index count; change → bump generation
    // Previous-frame GPU modelMat (row-major float4x4), staged into
    // UniformObject.prevModelMat for motion vectors. prevModelMat is the matrix
    // PUBLISHED as "previous" for the current frame; curModelMat is this frame's
    // matrix, which rotates into prevModelMat at the first submit of the NEXT
    // frame. The rotation is keyed on lastSubmitFrame so submitObject can be
    // called multiple times per frame for the same object (cWorld::PrepareFrame's
    // whole-scene submit + the renderer's raster loop) and still publish the same
    // prev — rotating on every call would zero the velocity (temporal smear).
    float          prevModelMat[16] = {};
    float          curModelMat[16]  = {};
    uint32_t       lastSubmitFrame  = UINT32_MAX;
    // Last UniformObject staged into m_objectBuffer[slot]. submitObject compares
    // the freshly-built payload against this and skips the upload when identical
    // (static objects: matrices + material never change frame-to-frame).
    UniformObject  lastPayload = {};
  };
  // Stable object-slot cache. frameInFlight = 0: a slot is reusable the frame
  // after its last use (the object-data path is synchronized), and is only
  // "exhausted" when every slot is already taken within the current frame.
  LRUCacheState<ObjectSlotState> m_objectSlots;

  // Point/spot/area light SSBOs and the fog-area SSBO are persistent per-world
  // buffers owned by cWorld (all world lights at stable slots; counts = world
  // totals). They ride the dedicated per-world set kWorldSet — each consuming
  // pass binds them via RIProgram::bindDescriptors (cached). This set (set 0)
  // neither owns, stages, nor binds them.

  // The six per-stream BDA handle buffers (gOpaque*Handles) were folded into
  // UniformObject (m_objectBuffer) — see ObjectSubmitDesc::StreamHandles and
  // submitObject(); they were always indexed by the same object slot.

  // Per-object-slot reuse generation (sized kObjectSlotCapacity). Bumped to a
  // fresh monotonic value each time the object cache (re)assigns a slot to a
  // different object or its geometry changes, so a consumer that cached an
  // object-slot reference can detect that the slot now describes different
  // geometry. m_nextSlotGeneration is the host-side source of new values so we
  // never read back the write-combined mapped buffer.
  struct RIBuffer m_bindlessSlotGenerationBuffer;
  uint32_t m_nextSlotGeneration = 0;

  // CPU shadow of the device-local slot-generation buffer. Host per-slot writes
  // go through this mirror; flushMirrors() stages its dirty byte range into the
  // device buffer once per frame. (The per-stream BDA handles now ride the
  // UniformObject upload in submitObject, so they no longer need mirrors.)
  BindlessShadowMirror m_bindlessSlotGenerationMirror;

  // Coarse world-space light grid (LightGridBuildPass writes, the direct
  // lighting pass and the path tracer's getCellLights read): per-cell light
  // count + packed per-cell unified-light-index list.
  struct RIBuffer m_lightGridCountBuffer;
  struct RIBuffer m_lightGridListBuffer;

  // Bindless material wiring (Falcor MaterialSystem model). One flat table of
  // fixed-size MaterialDataBlobs (m_materialBuffer, kBindingMaterials) indexed by
  // a flat materialID; one LRU pool hands out slots. The blob's header tags the
  // type so the shader reinterprets it into the right struct — no per-type buffers.
  LRUCache m_materialBindless;
  struct RIBuffer m_materialBuffer = {};
  std::optional<RIDescriptor> m_materialSampler;

  // Per-2D-array-slot animation record table (gAnimTex, kBindingAnimTex). One
  // AnimTexRec per textures_2d_array[] slot; written by cTextureManager when an
  // animated image is assigned its slot, read by the bindless sample helper.
  struct RIBuffer m_animTexBuffer = {};

  // Legacy dissolve noise (core_dissolve.tga), bound once to set 0 as the
  // immutable gDissolveMap — the CoverageAmount fade's screen-space dither.
  SharedResourceHandle<Image> m_dissolveMap;

  // The textures_2d[] / textures_cube[] bindless arrays (bindings 0/1) are
  // written into directly by cTextureManager, which owns the slot pools. This
  // set holds only the descriptor set + arrays as state; it does not manage the
  // texture heap.
};

// === Engine-lifetime set, owned by cGraphics as globalset ===
// Construct/cleanup happen once, in cGraphics::Init / teardown (after the
// RIDevice + cResources exist, before the device is destroyed). Reach the set
// through Interface<cGraphics>::Get()->globalset everywhere.
void InitGlobalManagedSets(RIDevice *device, cResources *resources);
void ShutdownGlobalManagedSets(RIDevice *device);

} // namespace hpl

#endif
