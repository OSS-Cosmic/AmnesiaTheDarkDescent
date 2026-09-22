
#ifndef RI_PROGRAM_H
#define RI_PROGRAM_H

// Must precede the DEVICE_IMPL_D3D12 test below -- the prelude is what defines
// those macros. Without it a TU including this header first would evaluate the
// block as 0 while the class body further down, by then preceded by includes
// that do pull the prelude, sees 1.
#include "graphics/RIPreamble.h"

#include "system/Hasher.h"
#include <array>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if (DEVICE_IMPL_D3D12)
// Forward declaration only, as in RIBuffer.h / RITexture.h / RIDescriptor.h:
// D3D12MemAlloc.h is on an include path only the engine and RI tests carry, so
// a header every consumer sees must not require it. Members needing the
// complete type are defined in RIProgram.cpp.
namespace D3D12MA {
class Allocation;
}
struct ID3D12PipelineState;
struct ID3D12RootSignature;
struct ID3D12StateObject;
#endif

#include "resources/FileSearcher.h"
#include "system/SystemTypes.h"

#include "RIDescriptorSetAllocator.h"
#include "RIPipelineDesc.h"
#include "RIShaderArtifact.h"
// RTPipelineSlot holds a D3D12 shader binding table by value. RIBuffer.h is
// itself free of backend headers, so this does not widen the include budget.
#include "graphics/RIBuffer.h"

namespace hpl {

enum class RIBindlessRegisterClass : uint8_t { CBV, SRV, UAV, Sampler };

#if (DEVICE_IMPL_D3D12)
struct RIBindlessD3D12Binding {
  uint32_t binding = 0;
  RIBindlessRegisterClass registerClass = RIBindlessRegisterClass::SRV;
  uint32_t descriptorCount = 1;
  uint32_t registerIndex = 0;
  uint32_t registerSpace = 0;
  uint32_t descriptorOffset = 0;
  VkDescriptorType descriptorType = VK_DESCRIPTOR_TYPE_MAX_ENUM;
  D3D12_SRV_DIMENSION srvDimension = D3D12_SRV_DIMENSION_BUFFER;
  D3D12_UAV_DIMENSION uavDimension = D3D12_UAV_DIMENSION_BUFFER;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  // Geometry handles are absolute indices into the device-wide raw-SRV arena,
  // rather than indices relative to this bindless set's allocation.
  bool geometryHeap = false;
};

struct RIBindlessD3D12Layout {
  ID3D12RootSignature *rootSignature = nullptr;       // borrowed
  std::span<const RIBindlessD3D12Binding> bindings{}; // borrowed
  // Whole-table root-parameter metadata, copied onto the program at
  // initialize() and used by the bind path alongside the per-binding contract
  // above. UINT32_MAX means the layout has no such table.
  uint32_t resourceRootParameter = UINT32_MAX;
  uint32_t geometryRootParameter = UINT32_MAX;
  uint32_t samplerRootParameter = UINT32_MAX;
  uint32_t geometryRangeCount = 0;
  uint32_t geometryRangeOffset = UINT32_MAX;
};
#endif

// Backend-neutral reference to a caller-owned bindless layout. Vulkan uses
// its descriptor-set-layout handle; D3D12 keeps an opaque borrowed handle so
// callers do not have to manufacture or inspect a Vulkan value.
struct RIBindlessLayout {
#if (DEVICE_IMPL_VULKAN)
  VkDescriptorSetLayout vk = VK_NULL_HANDLE;
#endif
#if (DEVICE_IMPL_D3D12)
  RIBindlessD3D12Layout d3d12;
#endif
};

class RIBindlessDescriptorSet {
public:
#if (DEVICE_IMPL_D3D12)
  using BackendBinding = RIBindlessD3D12Binding;
#else
  struct BackendBinding {
    uint32_t binding;
    RIBindlessRegisterClass registerClass;
    uint32_t descriptorCount = 1;
    uint32_t registerIndex = 0;
    uint32_t registerSpace = 0;
    uint32_t descriptorOffset = 0;
  };
#endif

  struct {
    VkDescriptorSetLayout m_bindlessSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_bindlessPool = VK_NULL_HANDLE;
    VkDescriptorSet m_bindlessSet = VK_NULL_HANDLE;
  } vk;
#if (DEVICE_IMPL_D3D12)
  struct {
    RIDescriptorArenaAllocation allocation{};
    ID3D12RootSignature *rootSignature = nullptr; // borrowed
    uint32_t resourceRootParameter = UINT32_MAX;
    uint32_t geometryRootParameter = UINT32_MAX;
    uint32_t samplerRootParameter = UINT32_MAX;
    uint32_t geometryRangeCount = 0;
    uint32_t geometryRangeOffset = UINT32_MAX;
    uint32_t resourceTableBase = 0;
    RIDescriptorArenaAllocation geometryAllocation{};
    bool usesGeometryAllocation = false;
    bool ownsAllocation = false;
    bool hasBeenBound = false;
    std::vector<BackendBinding> bindings;
    std::vector<ID3D12Resource *> resources;
    std::vector<D3D12MA::Allocation *> allocations;
  } d3d12;
#endif

  RIBindlessDescriptorSet() {}

  RIBindlessLayout layout() const {
    RIBindlessLayout result{};
#if (DEVICE_IMPL_VULKAN)
#if DEVICE_MULTI_BACKEND
    if (RIIsTargetSelected(RI_DEVICE_API_VK))
      result.vk = vk.m_bindlessSetLayout;
#else
    result.vk = vk.m_bindlessSetLayout;
#endif
#endif
#if (DEVICE_IMPL_D3D12)
#if DEVICE_MULTI_BACKEND
    if (RIIsTargetSelected(RI_DEVICE_API_D3D12))
      result.d3d12 = {d3d12.rootSignature,         d3d12.bindings,
                      d3d12.resourceRootParameter, d3d12.geometryRootParameter,
                      d3d12.samplerRootParameter,  d3d12.geometryRangeCount,
                      d3d12.geometryRangeOffset};
#else
    result.d3d12 = {d3d12.rootSignature,         d3d12.bindings,
                    d3d12.resourceRootParameter, d3d12.geometryRootParameter,
                    d3d12.samplerRootParameter,  d3d12.geometryRangeCount,
                    d3d12.geometryRangeOffset};
#endif
#endif
    return result;
  }

  struct Binding {
    uint32_t binding;
    VkDescriptorType descriptorType;
    uint32_t descriptorCount;
    VkShaderStageFlags stageFlags;
    VkDescriptorBindingFlags flags;
#if (DEVICE_IMPL_D3D12)
    VkDescriptorType d3d12DescriptorType = VK_DESCRIPTOR_TYPE_MAX_ENUM;
    D3D12_SRV_DIMENSION d3d12SrvDimension = D3D12_SRV_DIMENSION_BUFFER;
    D3D12_UAV_DIMENSION d3d12UavDimension = D3D12_UAV_DIMENSION_BUFFER;
    DXGI_FORMAT d3d12Format = DXGI_FORMAT_UNKNOWN;
#endif
  };

#if (DEVICE_IMPL_D3D12)
  // Convert descriptor kinds with an unambiguous D3D12 register class and
  // preserve reflected view metadata for typed null initialization.
  static bool convertBinding(const Binding &binding, BackendBinding &out);
#endif

  struct WriteBinding {
    uint32_t binding;
    uint32_t arrayElement;
    RIDescriptor descriptor;
  };

  // Once a set has been bound, updates require a fence that has already
  // completed. This makes in-place descriptor updates explicit and prevents
  // replacing resources still referenced by submitted GPU work.
  //
  // An EMPTY descriptor releases the slot instead of writing one: the element
  // goes back to the typed null descriptor initialize() put there, and D3D12
  // drops the reference it retained, so the slot stops counting as live and can
  // be written again. Releasing needs no fence -- there is no new resource to
  // protect -- but the caller must still only release once the GPU is past the
  // frames that referenced the slot, which for pooled indices means from the
  // graphicsDefer drain (see cTextureManager::ReturnBindlessSlot). Vulkan keeps
  // no per-slot state, so a release is a no-op there.
  //
  // All or nothing: every write is validated before any descriptor is touched,
  // and one rejected entry fails the whole batch.
  bool
  writeDescriptors(RIDevice *device, std::span<const WriteBinding> writes,
                   const RIDescriptorArenaFence *completionFence = nullptr);

  void initialize(RIDevice *device, std::span<const Binding> bindings,
                  std::span<const VkDescriptorPoolSize> poolSizes);
#if (DEVICE_IMPL_D3D12)
  bool initialize(RIDevice *device, std::span<const BackendBinding> bindings,
                  const RIBindlessD3D12Layout &layout);
#endif

  // Out of line: the D3D12 half releases D3D12MA::Allocation, which is only
  // forward-declared here. See RIProgram.cpp.
  void destroy(RIDevice *device);
};

class RIProgram {
public:
  static constexpr size_t DESCRIPTOR_SET_MAX = 4;
  static constexpr size_t MAX_VERTEX_ATTRIBUTES = 16;
  struct PipelineSlot {
    union {
#if (DEVICE_IMPL_VULKAN)
      struct {
        VkPipeline handle;
      } vk;
#endif
#if (DEVICE_IMPL_D3D12)
      struct {
        ID3D12PipelineState *handle;
        uint32_t topology;
        // Stencil reference is PSO state in Vulkan but command-list state in
        // D3D12 (OMSetStencilRef), so it rides along here and is replayed at
        // bind time, exactly like topology and the vertex strides.
        uint32_t stencilRef;
        uint32_t vertexBindingCount;
        uint32_t vertexBindingStrides[MAX_VERTEX_ATTRIBUTES];
      } d3d12;
#endif
    };
  };

  // Ray-tracing pipeline cache slot. Carries the SBT buffer + the four
  // strided-device-address regions vkCmdTraceRaysKHR / DispatchRays need so
  // traceRays() can dispatch without recomputing them.
  struct RTPipelineSlot {
    union {
#if (DEVICE_IMPL_VULKAN)
      struct {
        VkPipeline handle;
        VkBuffer sbtBuffer;
        VmaAllocation sbtAlloc;
        VkStridedDeviceAddressRegionKHR raygenRegion;
        VkStridedDeviceAddressRegionKHR missRegion;
        VkStridedDeviceAddressRegionKHR hitRegion;
        VkStridedDeviceAddressRegionKHR callableRegion;
      } vk;
#endif
#if (DEVICE_IMPL_D3D12)
      struct {
        // A DXR pipeline is a state object, not an ID3D12PipelineState, so it
        // is bound with SetPipelineState1 rather than SetPipelineState.
        ID3D12StateObject *handle;
        // Hash of the shaders and desc this state object was built from. The
        // map is keyed by the caller's variant tag alone -- as the Vulkan path
        // keys it -- so that traceRays, which is handed nothing else, can find
        // the slot again. This is what still catches the shaders changing
        // under a reused tag: a mismatch rebuilds the slot in place rather
        // than leaving a second entry aliasing the same state object, which
        // dispose() would then release twice.
        hash_t contentKey;
        // Precomputed SBT regions, copied straight into a
        // D3D12_DISPATCH_RAYS_DESC by traceRays. Raygen is a plain range
        // rather than a range-and-stride because DXR, like Vulkan, allows
        // exactly one raygen record.
        D3D12_GPU_VIRTUAL_ADDRESS_RANGE raygenRange;
        D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE missRange;
        D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE hitRange;
        D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE callableRange;
      } d3d12;
#endif
    };
#if (DEVICE_IMPL_D3D12)
    // The shader binding table backing the addresses above. Held whole rather
    // than as a raw resource + allocation the way the Vulkan arm does, because
    // a D3D12 buffer must be torn down through RIBuffer::dispose -- releasing
    // the ID3D12Resource directly would skip the buffer registration registry.
    // RIBuffer has a user-provided constructor, so it cannot live in the union
    // above either.
    RIBuffer sbt;
#endif
  };

  struct DescriptorBinding {
    DescriptorBinding()
        : handle(), registerOffset(0), descriptor(), optional(false),
          useSlot(false), slotSet(0), slotBinding(0) {}
    explicit DescriptorBinding(const char *name, const RIDescriptor &desc,
                               uint32_t registerOffset = 0,
                               bool optional = false)
        : handle(DescriptorBindingID::Create(name)),
          registerOffset(registerOffset), descriptor(desc), optional(optional),
          useSlot(false), slotSet(0), slotBinding(0) {}

    // NRD describes resources by numeric (set, binding) slots rather than by
    // the reflected resource names used by the normal engine bind path.
    DescriptorBinding(uint32_t set, uint32_t binding, const RIDescriptor &desc,
                      bool optional = false)
        : handle(), registerOffset(0), descriptor(desc), optional(optional),
          useSlot(true), slotSet(set), slotBinding(binding) {}

    struct DescriptorBindingID handle;
    uint32_t registerOffset;
    struct RIDescriptor descriptor;
    // Optional: this binding may legitimately carry an empty descriptor
    // (e.g. a TLAS that does not exist until the first build). Marks it
    // exempt from the debug unwritten-binding check; it is still skipped at
    // write time exactly as before.
    bool optional = false;
    bool useSlot = false;
    uint32_t slotSet = 0;
    uint32_t slotBinding = 0;
  };

  struct DescriptorSetSlot {
    union {
#if (DEVICE_IMPL_VULKAN)
      struct {
        VkDescriptorSetLayout setLayout;
      } vk;
#endif
#if (DEVICE_IMPL_D3D12)
      struct {
        ID3D12RootSignature *rootSignature;
        uint32_t resourceRootParameter;
        uint32_t geometryRootParameter;
        uint32_t samplerRootParameter;
        uint32_t geometryRangeOffset;
      } d3d12;
#endif
    };
    struct RIDescriptorSetAlloc alloc; // the set allocator
    uint16_t samplerMaxNum;
    uint16_t combinedImageSamplerMaxNum;
    uint16_t constantBufferMaxNum;
    uint16_t dynamicConstantBufferMaxNum;
    uint16_t textureMaxNum;
    uint16_t storageTextureMaxNum;
    uint16_t bufferMaxNum;
    uint16_t storageBufferMaxNum;
    uint16_t structuredBufferMaxNum;
    uint16_t storageStructuredBufferMaxNum;
    uint16_t accelerationStructureMaxNum;
    // External: set layout is borrowed from the caller; the program does
    // not allocate, cache, or write any descriptor set for this slot.
    // The caller is responsible for binding the descriptor set via
    // vkCmdBindDescriptorSets at the matching set index.
    bool isExternal;
#if (DEVICE_IMPL_D3D12)
    struct D3D12ExternalTable {
      uint32_t binding = 0;
      RIBindlessRegisterClass registerClass = RIBindlessRegisterClass::SRV;
      uint32_t registerIndex = 0;
      uint32_t registerSpace = 0;
      uint32_t rootParameter = UINT32_MAX;
      bool geometryHeap = false;
    };
    std::vector<D3D12ExternalTable> d3d12ExternalTables;
#endif
  };
  struct ShaderReflection;
  struct ShaderBinary {
    std::vector<char> buf;
    // SPIR-V entry-point function name. Defaults to "main" (GLSL +
    // single-entry-point HLSL); Slang shaders compiled with
    // `-fvk-use-entrypoint-name` keep their function name in OpEntryPoint
    // (e.g. "csMain", "collectCellInfo"), which must match
    // VkPipelineShaderStageCreateInfo::pName exactly.
    std::string entryPoint = "main";
    std::shared_ptr<const ShaderReflection> reflection;
  };

  enum class ShaderRegisterClass : uint8_t { Unknown, CBV, SRV, UAV, Sampler };
  struct ShaderResourceReflection {
    std::string name;
    std::string stage;
    ShaderRegisterClass registerClass = ShaderRegisterClass::Unknown;
    uint32_t registerIndex = 0;
    uint32_t registerSpace = 0;
    uint32_t arrayCount = 1;
    // Slang emits either a numeric count or the string "unbounded".  Keep
    // that distinction instead of silently turning the latter into one.
    bool unbounded = false;
    uint32_t stride = 0;
    uint32_t size = 0;
    std::string type;
    std::string format;
    bool used = true;
  };
  struct ShaderVertexInputReflection {
    std::string semanticName;
    uint32_t semanticIndex = 0;
    uint32_t location = 0;
  };
  struct ShaderPushConstantReflection {
    uint32_t offset = 0;
    uint32_t size = 0;
    uint32_t registerIndex = 0;
    uint32_t registerSpace = 0;
    bool present = false;
  };
  struct ShaderReflection {
    std::string entryPoint;
    std::string stage;
    std::vector<ShaderResourceReflection> resources;
    std::vector<ShaderVertexInputReflection> vertexInputs;
    ShaderPushConstantReflection pushConstants;
    std::shared_ptr<const std::string> json;
  };
  struct ShaderArtifact {
    std::shared_ptr<const std::vector<char>> bytes;
    RIShaderArtifactFormat format = RIShaderArtifactFormat::Unknown;
    std::shared_ptr<const ShaderReflection> reflection;
    // Parsed `.meta` stage/entry map. Shared rather than held by value because
    // one artifact is copied into every ModuleStage that names an entry point
    // in it -- a ray-tracing library is loaded once and referenced four times.
    std::shared_ptr<const RIShaderArtifactMeta> meta;
    const char *data() const { return bytes ? bytes->data() : nullptr; }
    size_t size() const { return bytes ? bytes->size() : 0; }
    bool empty() const { return size() == 0; }
    operator std::span<char>() const {
      return std::span<char>(const_cast<char *>(data()), size());
    }
  };

  struct BindingReflection {
    hash_t hash;
    uint16_t isArray : 1;
    uint16_t dimCount : 8;
    uint16_t set : 3;
    uint16_t baseRegisterIndex;
#if (DEVICE_IMPL_D3D12)
    RIBindlessRegisterClass registerClass = RIBindlessRegisterClass::CBV;
    uint32_t descriptorCount = 1;
    uint32_t d3d12DescriptorOffset = 0;
    bool d3d12Sampler = false;
    bool d3d12External = false;
    uint8_t d3d12ExternalSet = UINT8_MAX;
#endif
#if !defined(NDEBUG)
    // Reflected resource name, kept only so the debug unwritten-binding
    // diagnostic in bindDescriptors can name the offending binding.
    std::string debugName;
#endif
  };

  enum ProgramStages {
    PROGRAM_STAGE_VERTEX,
    PROGRAM_STAGE_FRAGMENT,
    PROGRAM_STAGE_COMPUTE,
    PROGRAM_STAGE_RAYGEN,
    PROGRAM_STAGE_MISS,
    PROGRAM_STAGE_CLOSEST_HIT,
    PROGRAM_STAGE_ANY_HIT,
    PROGRAM_STAGE_INTERSECTION,
    PROGRAM_STAGE_CALLABLE,
    PROGRAM_STAGES_MAX
  };

  struct ModuleStage {
    ModuleStage() = default;
    ModuleStage(uint8_t stage, std::span<char> data,
                const char *entryPoint = "main",
                RIShaderArtifactFormat format = RIShaderArtifactFormat::Unknown)
        : stage(stage), data(data), entryPoint(entryPoint), format(format) {}
    ModuleStage(uint8_t stage, const ShaderArtifact &artifact,
                const char *entryPoint = "main")
        : stage(stage), data(artifact), entryPoint(entryPoint),
          format(artifact.format), artifact(artifact) {}
    uint8_t stage;
    std::span<char> data;
    // Optional override of the entry-point function name. Leave as "main"
    // for GLSL or default-named HLSL. Slang shaders compiled with
    // `-fvk-use-entrypoint-name` emit OpEntryPoint with the source
    // function name; set this to match (e.g. "csMain", "collectCellInfo").
    const char *entryPoint = "main";
    // Unknown is used for caller-owned blobs; initialize detects the standard
    // SPIR-V/DXBC container magic and still permits raw SPIR-V in memory.
    RIShaderArtifactFormat format = RIShaderArtifactFormat::Unknown;
    ShaderArtifact artifact;
  };
  const struct BindingReflection *
  findReflection(const struct DescriptorBindingID &handle);
  const struct BindingReflection *findReflectionBySlot(uint32_t set,
                                                       uint32_t binding);
  // `externalLayouts` (optional) lets the caller hand in a pre-built
  // backend-native bindless layout for any set index. D3D12 global-set layouts
  // may use a null root signature as a wildcard because programs own the
  // signature; the t4/space3 geometry metadata is still validated. External
  // slots skip descriptor-set allocation/writes and are bound by the caller.
  void initialize(RIDevice *device, std::span<ModuleStage> init,
                  std::span<const RIBindlessLayout> externalLayouts = {},
                  const char *debugName = nullptr);
#if (DEVICE_IMPL_VULKAN && !DEVICE_IMPL_D3D12)
  // Compatibility for Vulkan callers that still keep native layout arrays.
  // Multi-backend code must pass RIBindlessLayout so D3D12 metadata cannot be
  // silently discarded when the active backend changes at runtime.
  template <size_t N>
  void initialize(RIDevice *device, std::span<ModuleStage> init,
                  const VkDescriptorSetLayout (&externalLayouts)[N],
                  const char *debugName = nullptr) {
    std::array<RIBindlessLayout, N> layouts{};
    for (size_t i = 0; i < N; ++i) {
      layouts[i].vk = externalLayouts[i];
    }
    initialize(device, init, std::span<const RIBindlessLayout>(layouts),
               debugName);
  }
#endif
  // Destroy everything initialize()/bind*Pipeline() created: cached pipelines
  // (+ RT SBT buffers), owned set layouts + descriptor pools, and the pipeline
  // layout. External set layouts are caller-owned and skipped. Idempotent;
  // the GPU must be idle.
  void dispose(RIDevice *device);
  static ShaderArtifact loadShaderStage(cFileSearcher *searcher,
                                        const tString &asName,
                                        const char *entryPoint = nullptr);
  static ShaderArtifact loadShaderArtifact(cFileSearcher *searcher,
                                           const tString &asName,
                                           const char *entryPoint = nullptr);
  // Bind a graphics pipeline described in backend-neutral RI terms: a plain
  // value with no pNext chain and no sub-struct pointers to keep alive. Vulkan
  // and D3D12 each translate from it directly; neither backend reads the
  // other's structs.
  //
  // `pipelineHash` is only a variant discriminator -- the full pipeline state
  // is hashed into the cache key for you, so two distinct descs cannot collide
  // on a shared hash.
  void bindPipeline(struct RIDevice *device, struct RICmd *cmd,
                    hash_t pipelineHash, const char *debugName,
                    const RIGraphicsPipelineDesc &desc);
  // A compute pipeline is fully described by the program's shader stage and
  // layout, so there is no state to pass.
  void bindComputePipeline(struct RIDevice *device, struct RICmd *cmd,
                           hash_t pipelineHash, const char *debugName);
  // Bind a ray-tracing pipeline. The cache slot also owns the SBT so traceRays
  // can dispatch without recomputing the regions. Stages, groups and layout
  // all come from the program's own shaderBin plus its pipeline layout (Vulkan)
  // or root signature (D3D12); the desc carries only what neither can infer.
  //
  // Both backends implement this. Vulkan builds a VkPipeline from one
  // VkShaderModule per stage and fills the SBT from
  // vkGetRayTracingShaderGroupHandlesKHR; D3D12 builds a DXR state object from
  // the single DXIL library all the stages share and fills the SBT from
  // ID3D12StateObjectProperties::GetShaderIdentifier.
  void bindRayTracingPipeline(struct RIDevice *device, struct RICmd *cmd,
                              hash_t pipelineHash, const char *debugName,
                              const RIRayTracingPipelineDesc &desc);
  // Dispatch a width x height x depth ray grid against the cached SBT for
  // `pipelineHash` (vkCmdTraceRaysKHR / DispatchRays). The pipeline must have
  // been created earlier via bindRayTracingPipeline.
  void traceRays(struct RICmd *cmd, hash_t pipelineHash, uint32_t width,
                 uint32_t height, uint32_t depth);
  // Issue vkCmdTraceRaysIndirectKHR: dimensions come from a
  // VkTraceRaysIndirectCommandKHR struct at indirectAddress (a device address
  // into a SHADER_DEVICE_ADDRESS buffer).
  //
  // Vulkan-only, and the raw VkDeviceAddress in the signature says so. D3D12's
  // equivalent is not a drop-in: DispatchRaysIndirect consumes a whole
  // D3D12_DISPATCH_RAYS_DESC through an ID3D12CommandSignature, not a bare
  // 3-uint extent, so the argument buffer layout is not portable. Nothing
  // calls this today; give it an RI-neutral address and a backend-side desc
  // builder before it grows a second implementation.
  void traceRaysIndirect(struct RICmd *cmd, hash_t pipelineHash,
                         VkDeviceAddress indirectAddress);
  // Write + bind one descriptor set per non-external set index. A binding this
  // program does not reflect is ignored (one shared binding vector may serve
  // several programs), and an empty descriptor is skipped.
  //
  // Debug builds report each reflected-but-unwritten binding once via Error():
  // that is a descriptor the shader reads undefined, invisible without a
  // validation layer. Mark a legitimately-empty binding `optional = true` to
  // opt out; array bindings are exempt (they are partially bound).
  void bindDescriptors(
      struct RIDevice *device, struct RICmd *cmd, uint32_t frameIndex,
      DescriptorBinding *binding, size_t bindingCount,
      VkPipelineBindPoint bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS);
  // Bind an externally-owned bindless descriptor set at `setIndex` against
  // this program's pipeline layout. The matching slot in `programDescriptors`
  // must have been registered as external via `externalLayouts` at
  // initialize(); the program does not allocate or write descriptors for it.
  bool bindBindlessDescriptorSet(
      struct RICmd *cmd, RIBindlessDescriptorSet *bindless, uint32_t setIndex,
      VkPipelineBindPoint bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS);
#if (DEVICE_IMPL_D3D12)
  // Binds the shared shader-visible arena tables and the program's reflected
  // root signature. Graphics and compute use the same table allocation; only
  // the command-list entry point differs.
  bool bindD3D12BindlessDescriptorSet(struct RICmd *cmd,
                                      RIBindlessDescriptorSet *bindless,
                                      bool compute, uint32_t setIndex = 0);
  ID3D12RootSignature *getD3D12RootSignature() const {
    return impl.d3d12.rootSignature;
  }
  // D3D12 root-constant contract reflected from the shader/root signature.
  // UINT32_MAX means that this program has no push-constant range.
  uint32_t getD3D12PushConstantRootParameter() const {
    return impl.d3d12.pushConstantRootParameter;
  }
  uint32_t getD3D12PushConstantOffset() const {
    return impl.d3d12.pushConstantOffset;
  }
  uint32_t getD3D12PushConstantSize() const {
    return impl.d3d12.pushConstantSize;
  }
  // Returns the translated byte strides for the graphics pipeline variant
  // most recently bound by this program. The span is owned by this program
  // and remains valid until the next bind or dispose().
  std::span<const uint32_t> getD3D12VertexBindingStrides() const {
    return {d3d12VertexBindingStrides.data(), d3d12VertexBindingCount};
  }
  // Number of backend PSOs currently retained by this program.  This is
  // intentionally a read-only observation for D3D12 smoke/diagnostic tests;
  // creation and binding remain owned by the cache below.
  size_t getD3D12PipelineCacheSize() const { return pipeline.size(); }
  // Live descriptor-table entries; bounded by the LRU recycling in
  // bindDescriptors. Read-only observation for smoke/diagnostic tests.
  size_t getD3D12DescriptorCacheSize() const {
    return d3d12DescriptorCache.size();
  }
#endif
#if (DEVICE_IMPL_VULKAN)
  VkPipelineLayout getPipelineLayout() const { return impl.vk.pipelineLayout; }
  // Reflected push-constant range stage flags; used by
  // RICmd::vk_d3d12_setPushConstants so call sites need not respecify the
  // stage.
  VkShaderStageFlags getPushConstantStageFlags() const {
    return impl.vk.pushConstant.shaderStageFlags;
  }
#endif
  explicit RIProgram() {}

private:
  RIProgram(const RIProgram &) = delete;
  RIProgram(RIProgram &&) = delete;
  RIProgram &operator=(RIProgram &&) = delete;

#if (DEVICE_IMPL_D3D12)
  // Translates straight from RI enums and formats -- no Vulkan struct and no
  // pNext walk in between.
  void bindD3D12Pipeline(struct RIDevice *device, struct RICmd *cmd,
                         hash_t pipelineHash, const char *debugName,
                         const RIGraphicsPipelineDesc &desc);
  // createD3D12GraphicsPipeline creates the PSO, names it, and caches it under
  // `cacheKey`; applyD3D12GraphicsPipeline binds the cached slot along with the
  // root signature, vertex strides, primitive topology and stencil reference.
  void createD3D12GraphicsPipeline(
      struct RIDevice *device, hash_t cacheKey, const char *debugName,
      D3D12_GRAPHICS_PIPELINE_STATE_DESC &psoDesc, PipelineSlot &slot);
  void applyD3D12GraphicsPipeline(struct RICmd *cmd, const PipelineSlot &slot,
                                  const char *debugName);
  void
  bindD3D12ComputePipeline(struct RIDevice *device, struct RICmd *cmd,
                           hash_t pipelineHash, const char *debugName);
  // Build (on cache miss) and bind a DXR state object. All the ray-tracing
  // stages share one DXIL library, so the state object carries a single
  // library subobject exporting each stage's entry point by name.
  void bindD3D12RayTracingPipeline(struct RIDevice *device, struct RICmd *cmd,
                                   hash_t pipelineHash, const char *debugName,
                                   const RIRayTracingPipelineDesc &desc);
  // Fills `slot` with a fresh state object plus its shader binding table.
  void createD3D12RayTracingPipeline(struct RIDevice *device,
                                     const char *debugName,
                                     const RIRayTracingPipelineDesc &desc,
                                     RTPipelineSlot &slot);
  void traceD3D12Rays(struct RICmd *cmd, hash_t pipelineHash, uint32_t width,
                      uint32_t height, uint32_t depth);
#endif

  union __impl {
    struct {
#if (DEVICE_IMPL_VULKAN)
      struct {
        VkShaderStageFlags shaderStageFlags;
        uint32_t size;
      } pushConstant;
      VkPipelineLayout pipelineLayout;
#endif
    } vk;
#if (DEVICE_IMPL_D3D12)
    struct {
      ID3D12RootSignature *rootSignature;
      uint32_t resourceRootParameter;
      uint32_t geometryRootParameter;
      uint32_t samplerRootParameter;
      uint32_t geometryRangeOffset;
      uint32_t geometryRangeCount;
      uint32_t pushConstantRootParameter;
      uint32_t pushConstantOffset;
      uint32_t pushConstantSize;
      // Bit per root parameter this program's shaders actually read. Compared
      // against what the command list has bound to catch a draw whose
      // descriptor tables were invalidated before it.
      uint64_t rootArgumentMask;
    } d3d12;
#endif
  } impl{};
#if (DEVICE_IMPL_D3D12)
  struct D3D12DescriptorCacheEntry {
    hash_t hash = HASH_INITIAL_VALUE;
    uint32_t lastUsedFrame = 0;
    // Resource range only; samplers live in a shared table (see
    // acquireSamplerTableArena) so identical sampler sets across entries and
    // programs occupy the 2048-entry sampler heap once.
    RIDescriptorArenaAllocation allocation{};
    uint32_t samplerOffset = 0;
    uint32_t samplerCount = 0;
    uint64_t samplerKey = 0;
    std::vector<ID3D12Resource *> resources;
    std::vector<D3D12MA::Allocation *> allocations;
  };
  // Descriptor tables keyed by binding-set hash, following the same policy as
  // Vulkan's resolveDescriptorSetAlloc: an entry untouched for more than
  // RI_NUMBER_FRAMES_FLIGHT frames is rewritten in place (clock sweep from
  // d3d12DescriptorRecycleCursor), so per-draw transient bindings cannot grow
  // the shared arena without bound.
  std::vector<D3D12DescriptorCacheEntry> d3d12DescriptorCache;
  std::unordered_map<hash_t, uint32_t> d3d12DescriptorIndex;
  uint32_t d3d12DescriptorRecycleCursor = 0;
  bool d3d12ArenaExhaustedReported = false;
  std::array<uint32_t, MAX_VERTEX_ATTRIBUTES> d3d12VertexBindingStrides{};
  uint32_t d3d12VertexBindingCount = 0;
#endif
  RIDevice *device = NULL;
  uint16_t reflection_len = 0;
  uint32_t vertex_input_mask = 0;
  std::array<uint32_t, MAX_VERTEX_ATTRIBUTES> vertex_input_format{};
  bool hasPushConstant = false;
  std::array<struct DescriptorSetSlot, DESCRIPTOR_SET_MAX> programDescriptors{};
  std::array<ShaderBinary, PROGRAM_STAGES_MAX> shaderBin;
  std::unordered_map<hash_t, PipelineSlot> pipeline;
  std::unordered_map<hash_t, RTPipelineSlot> rtPipeline;
  std::vector<BindingReflection> bindingReflection;
#if !defined(NDEBUG)
  std::string m_debugName;
  // Set index + reflected binding hash of every unwritten binding already
  // reported, so the per-frame check logs each one once per program.
  std::unordered_set<uint64_t> m_reportedUnwrittenBindings;
#endif
};
} // namespace hpl
#endif
