#ifndef HPL_XESS_UPSCALER_H
#define HPL_XESS_UPSCALER_H

#include "graphics/TemporalUpscaler.h"

// Supplies the backend-selection macros AND each enabled backend's types
// (volk for Vulkan, d3d12.h for D3D12), so the XeSS per-backend headers below
// see the same Vulkan declarations the rest of RI does -- including
// VK_NO_PROTOTYPES, which volk requires and a raw <vulkan/vulkan.h> would
// violate.
#include "graphics/RIPreamble.h"

#include <cstdint>

#if defined(HPL2_XESS_AVAILABLE) && HPL2_XESS_AVAILABLE
#include <xess/xess.h>
#if (DEVICE_IMPL_VULKAN)
#include <xess/xess_vk.h>
#endif
#if (DEVICE_IMPL_D3D12)
#include <xess/xess_d3d12.h>
#endif
#endif

struct RIVkInstanceRequirements;
struct RIVkDeviceRequirements;

namespace hpl {

class cGraphics;

// Process/device-lifetime XeSS loader. The SDK module is kept loaded for the
// lifetime of this object so SDK-owned extension names and feature-chain
// structures remain valid while a device or XeSS context uses them.
//
// One libxess.dll serves both backends, so unlike FfxApiLoader there is no
// per-backend module to pick -- but availability is still tracked per backend,
// for two reasons. A build compiled for one backend must not be taken down by
// the other backend's entry points failing to resolve, and the Vulkan
// device-requirement contribution below vetoes XeSS on its own device
// (SetVulkanUnavailable) without that verdict having any bearing on a D3D12
// device in the same process.
class cXessSupport {
public:
  cXessSupport();
  ~cXessSupport();

  // Whether XeSS can run on the backend the renderer selected, and why not.
  bool IsAvailable() const;
  const char *UnavailableReason() const;

  // The same pair for one specific RIDeviceAPI_e, for callers that need a
  // verdict before a backend has been selected.
  bool IsAvailableFor(uint8_t backendApi) const;
  const char *UnavailableReasonFor(uint8_t backendApi) const;

  // Used when RI cannot satisfy the Vulkan instance or device requirements
  // XeSS contributed. This does not affect ordinary Vulkan initialization, and
  // it deliberately leaves the D3D12 verdict alone: the two devices are
  // independent, and a multi-backend build initializes Vulkan first.
  void SetVulkanUnavailable(const char *reason);

#if defined(HPL2_XESS_AVAILABLE) && HPL2_XESS_AVAILABLE
  typedef xess_result_t (*PFN_XessDestroyContext)(
      xess_context_handle_t context);
  typedef xess_result_t (*PFN_XessGetOptimalInputResolution)(
      xess_context_handle_t context, const xess_2d_t *outputResolution,
      xess_quality_settings_t qualitySettings,
      xess_2d_t *inputResolutionOptimal, xess_2d_t *inputResolutionMin,
      xess_2d_t *inputResolutionMax);
  typedef xess_result_t (*PFN_XessSetVelocityScale)(
      xess_context_handle_t context, float x, float y);
  typedef xess_result_t (*PFN_XessSetJitterScale)(
      xess_context_handle_t context, float x, float y);
  typedef xess_result_t (*PFN_XessSetExposureMultiplier)(
      xess_context_handle_t context, float scale);
  typedef xess_result_t (*PFN_XessGetProperties)(
      xess_context_handle_t context, const xess_2d_t *outputResolution,
      xess_properties_t *properties);
  typedef xess_result_t (*PFN_XessIsOptimalDriver)(
      xess_context_handle_t context);
  typedef xess_result_t (*PFN_XessGetVersion)(xess_version_t *version);
  typedef xess_result_t (*PFN_XessSetLoggingCallback)(
      xess_context_handle_t context, xess_logging_level_t loggingLevel,
      xess_app_log_callback_t loggingCallback);

  PFN_XessDestroyContext DestroyContext() const { return m_destroyContext; }
  PFN_XessGetOptimalInputResolution GetOptimalInputResolution() const {
    return m_getOptimalInputResolution;
  }
  PFN_XessSetVelocityScale SetVelocityScale() const {
    return m_setVelocityScale;
  }
  PFN_XessSetJitterScale SetJitterScale() const { return m_setJitterScale; }
  PFN_XessSetExposureMultiplier SetExposureMultiplier() const {
    return m_setExposureMultiplier;
  }
  PFN_XessGetProperties GetProperties() const { return m_getProperties; }
  PFN_XessIsOptimalDriver IsOptimalDriver() const {
    return m_isOptimalDriver;
  }
  PFN_XessGetVersion GetVersion() const { return m_getVersion; }
  PFN_XessSetLoggingCallback SetLoggingCallback() const {
    return m_setLoggingCallback;
  }

#if (DEVICE_IMPL_VULKAN)
  typedef xess_result_t (*PFN_XessVKGetRequiredInstanceExtensions)(
      uint32_t *count, const char *const **names, uint32_t *minVkApiVersion);
  typedef xess_result_t (*PFN_XessVKGetRequiredDeviceExtensions)(
      VkInstance instance, VkPhysicalDevice physicalDevice, uint32_t *count,
      const char *const **names);
  typedef xess_result_t (*PFN_XessVKGetRequiredDeviceFeatures)(
      VkInstance instance, VkPhysicalDevice physicalDevice, void **features);
  typedef xess_result_t (*PFN_XessVKCreateContext)(
      VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device,
      xess_context_handle_t *context);
  typedef xess_result_t (*PFN_XessVKInit)(
      xess_context_handle_t context, const xess_vk_init_params_t *params);
  typedef xess_result_t (*PFN_XessVKExecute)(
      xess_context_handle_t context, VkCommandBuffer commandBuffer,
      const xess_vk_execute_params_t *params);

  PFN_XessVKGetRequiredInstanceExtensions
  VKGetRequiredInstanceExtensions() const {
    return m_vkGetRequiredInstanceExtensions;
  }
  PFN_XessVKGetRequiredDeviceExtensions VKGetRequiredDeviceExtensions() const {
    return m_vkGetRequiredDeviceExtensions;
  }
  PFN_XessVKGetRequiredDeviceFeatures VKGetRequiredDeviceFeatures() const {
    return m_vkGetRequiredDeviceFeatures;
  }
  PFN_XessVKCreateContext VKCreateContext() const { return m_vkCreateContext; }
  PFN_XessVKInit VKInit() const { return m_vkInit; }
  PFN_XessVKExecute VKExecute() const { return m_vkExecute; }
#endif

#if (DEVICE_IMPL_D3D12)
  typedef xess_result_t (*PFN_XessD3D12CreateContext)(
      ID3D12Device *device, xess_context_handle_t *context);
  typedef xess_result_t (*PFN_XessD3D12Init)(
      xess_context_handle_t context, const xess_d3d12_init_params_t *params);
  typedef xess_result_t (*PFN_XessD3D12Execute)(
      xess_context_handle_t context, ID3D12GraphicsCommandList *commandList,
      const xess_d3d12_execute_params_t *params);

  PFN_XessD3D12CreateContext D3D12CreateContext() const {
    return m_d3d12CreateContext;
  }
  PFN_XessD3D12Init D3D12Init() const { return m_d3d12Init; }
  PFN_XessD3D12Execute D3D12Execute() const { return m_d3d12Execute; }
#endif
#endif

private:
  // The DLL plus the backend-independent entry points. Both per-backend
  // verdicts are false while this is false.
  bool m_moduleAvailable;
  char m_moduleUnavailableReason[128];

  bool m_vkAvailable;
  char m_vkUnavailableReason[128];
  bool m_d3d12Available;
  char m_d3d12UnavailableReason[128];

#if defined(HPL2_XESS_AVAILABLE) && HPL2_XESS_AVAILABLE
  void *m_library;
  PFN_XessDestroyContext m_destroyContext;
  PFN_XessGetOptimalInputResolution m_getOptimalInputResolution;
  PFN_XessSetVelocityScale m_setVelocityScale;
  PFN_XessSetJitterScale m_setJitterScale;
  PFN_XessSetExposureMultiplier m_setExposureMultiplier;
  PFN_XessGetProperties m_getProperties;
  PFN_XessIsOptimalDriver m_isOptimalDriver;
  PFN_XessGetVersion m_getVersion;
  PFN_XessSetLoggingCallback m_setLoggingCallback;
#if (DEVICE_IMPL_VULKAN)
  PFN_XessVKGetRequiredInstanceExtensions m_vkGetRequiredInstanceExtensions;
  PFN_XessVKGetRequiredDeviceExtensions m_vkGetRequiredDeviceExtensions;
  PFN_XessVKGetRequiredDeviceFeatures m_vkGetRequiredDeviceFeatures;
  PFN_XessVKCreateContext m_vkCreateContext;
  PFN_XessVKInit m_vkInit;
  PFN_XessVKExecute m_vkExecute;
#endif
#if (DEVICE_IMPL_D3D12)
  PFN_XessD3D12CreateContext m_d3d12CreateContext;
  PFN_XessD3D12Init m_d3d12Init;
  PFN_XessD3D12Execute m_d3d12Execute;
#endif
#endif
};

cXessSupport &XessSupportInstance();

#if (DEVICE_IMPL_VULKAN)
// Fill `out` with the Vulkan prerequisites XeSS needs folded into instance and
// device creation; hand them to RI through RIBackendInit::vk and RIDeviceDesc
// (see cGraphics::Init). Both return false when the loader is unavailable or
// the active backend is not Vulkan, in which case nothing is contributed and RI
// takes its plain path.
//
// Neither call commits the engine to anything: if RI cannot satisfy a
// contribution it invokes the rejection callback installed here, which vetoes
// XeSS on the loader (SetVulkanUnavailable). That verdict is what
// cXessUpscaler::Supports reads back afterwards, so a rejected contribution
// shows up as "XeSS unavailable" rather than as a failed device.
bool XessVkInstanceRequirements(RIVkInstanceRequirements *out);
bool XessVkDeviceRequirements(RIVkDeviceRequirements *out, VkInstance instance,
                              VkPhysicalDevice physicalDevice);
#endif

// XeSS implementation of the engine-neutral temporal upscaler contract.
// Inputs and the destination binding are borrowed from the presentation
// owner.  The only SDK object retained here is the per-viewport XeSS context;
// its destruction is parked in cGraphics::graphicsDefer.
class cXessUpscaler final : public iTemporalUpscaler {
public:
  explicit cXessUpscaler(cGraphics *graphics = nullptr);
  ~cXessUpscaler() override;

  cXessUpscaler(const cXessUpscaler &) = delete;
  cXessUpscaler &operator=(const cXessUpscaler &) = delete;
  cXessUpscaler(cXessUpscaler &&) = delete;
  cXessUpscaler &operator=(cXessUpscaler &&) = delete;

  bool Supports(TemporalUpscalerProvider provider,
                TemporalUpscalerQuality quality) const override;

  TemporalUpscalerExtent GetRecommendedRenderExtent(
      TemporalUpscalerExtent output,
      TemporalUpscalerQuality quality) const override;

  uint32_t GetJitterPhaseCount(TemporalUpscalerExtent render,
                               TemporalUpscalerExtent output) const override;

  bool PrepareContext(const TemporalUpscalerSettings &settings,
                     TemporalUpscalerExtent render,
                     TemporalUpscalerExtent output,
                     cGraphics::FrameContext *frame) override;

  TemporalUpscalerOutput RecordResolve(
      TemporalUpscalerExtent render, TemporalUpscalerExtent output,
      const TemporalUpscalerFrameInput &input) override;

private:
  cGraphics *Graphics() const;

  // Whether the active backend's device exists and can host an XeSS context.
  // Distinct from the loader verdict: the loader can be perfectly healthy
  // before any device has been created, and a query can arrive that early.
  static bool DeviceIsUsable(cGraphics *graphics, const char **reason);

#if defined(HPL2_XESS_AVAILABLE) && HPL2_XESS_AVAILABLE
  bool EnsureContextForQuery() const;
  void RetireContext();
  bool InitializeContext(TemporalUpscalerExtent render,
                         TemporalUpscalerExtent output,
                         TemporalUpscalerQuality quality,
                         uint32_t initFlags);

  mutable xess_context_handle_t m_context = nullptr;
  bool m_contextInitialized = false;
  bool m_responsiveMaskEnabled = false;
  bool m_hasExecuted = false;
  uint32_t m_initFlags = 0;
  TemporalUpscalerQuality m_preparedQuality =
      TemporalUpscalerQuality::NativeAA;
  TemporalUpscalerExtent m_preparedRender = {};
  TemporalUpscalerExtent m_preparedOutput = {};
#endif

  cGraphics *mpGraphics = nullptr;
};

} // namespace hpl

#endif // HPL_XESS_UPSCALER_H
