#include "graphics/TemporalUpscaler.h"

#include "engine/Interface.h"
#include "graphics/FsrUpscaler.h"
#include "graphics/RIDevice.h"
#include "graphics/XessUpscaler.h"
#include "graphics/XessVulkanSupport.h"

#include <cstring>

namespace hpl {
namespace {

constexpr const char *kProviderDisabled = "provider disabled";
constexpr const char *kUnknownProvider = "unknown provider";

constexpr const char *kFsrNotCompiled = "FSR support is not compiled in";
constexpr const char *kFsrUnsupportedPlatform =
    "FSR requires Windows x64 or Linux x86_64";
#if defined(HPL2_FSR_AVAILABLE) && HPL2_FSR_AVAILABLE
constexpr const char *kFsrGraphicsUnavailable =
    "FSR Vulkan device is unavailable";
constexpr const char *kFsrRequiresVulkan =
    "FSR requires the Vulkan renderer";
constexpr const char *kFsrRequirementsUnavailable =
    "FSR Vulkan requirements are not satisfied";
#endif

constexpr const char *kXessNotCompiled = "XeSS support is not compiled in";
constexpr const char *kXessUnsupportedPlatform =
    "XeSS is only supported on Windows";
#if defined(HPL2_XESS_AVAILABLE) && HPL2_XESS_AVAILABLE
constexpr const char *kXessRuntimeNotFound = "XeSS runtime library not found";
constexpr const char *kXessRuntimeSymbolMissing =
    "XeSS runtime symbol is missing";
constexpr const char *kXessRuntimeUnavailable = "XeSS runtime is unavailable";
constexpr const char *kXessGraphicsUnavailable =
    "XeSS Vulkan device is unavailable";
constexpr const char *kXessDeviceUnsupported = "XeSS device is unsupported";
constexpr char kXessDllLoadFailure[] =
    "XeSS DLL libxess.dll could not be loaded";
constexpr char kXessSymbolFailure[] = "XeSS DLL symbol ";
#endif

constexpr const char *kQualityUnsupported =
    "quality level not supported by provider";

constexpr TemporalUpscalerQuality kQualities[] = {
    TemporalUpscalerQuality::NativeAA,
    TemporalUpscalerQuality::Quality,
    TemporalUpscalerQuality::Balanced,
    TemporalUpscalerQuality::Performance,
    TemporalUpscalerQuality::UltraPerformance,
};

bool IsFsrPlatformSupported() {
#if defined(_WIN32)
#if defined(_M_X64) || defined(__x86_64__) || \
    defined(__amd64__)
  return true;
#else
  return false;
#endif
#elif defined(__linux__)
#if defined(__x86_64__) || defined(__amd64__)
  return true;
#else
  return false;
#endif
#else
  return false;
#endif
}

bool IsXessWindowsPlatform() {
#if defined(_WIN32)
  return true;
#else
  return false;
#endif
}

bool AdapterSupports(TemporalUpscalerProvider provider,
                     TemporalUpscalerQuality quality, cGraphics *graphics) {
  if (!graphics)
    return false;

  // These temporary adapters contain no prepared state. FSR's constructor
  // allocates only its CPU-side Impl, and XeSS's constructor only stores the
  // pointer. Supports() performs the real adapter capability checks without
  // creating an SDK context, allocating a GPU resource, or recording a
  // command.
  switch (provider) {
  case TemporalUpscalerProvider::Fsr: {
#if defined(HPL2_FSR_AVAILABLE) && HPL2_FSR_AVAILABLE
    if (!IsFsrPlatformSupported())
      return false;
    cFsrUpscaler adapter(graphics);
    return adapter.Supports(provider, quality);
#else
    (void)quality;
    return false;
#endif
  }
  case TemporalUpscalerProvider::XeSS: {
#if defined(HPL2_XESS_AVAILABLE) && HPL2_XESS_AVAILABLE
    if (!IsXessWindowsPlatform())
      return false;
    cXessUpscaler adapter(graphics);
    return adapter.Supports(provider, quality);
#else
    (void)quality;
    return false;
#endif
  }
  case TemporalUpscalerProvider::Off:
    return false;
  }

  return false;
}

#if defined(HPL2_XESS_AVAILABLE) && HPL2_XESS_AVAILABLE
bool IsXessRuntimeFailure(const char *reason) {
  if (reason &&
      std::strncmp(reason, kXessDllLoadFailure,
                   sizeof(kXessDllLoadFailure) - 1) == 0)
    return true;
  if (reason &&
      std::strncmp(reason, kXessSymbolFailure,
                   sizeof(kXessSymbolFailure) - 1) == 0)
    return true;
  return false;
}

const char *XessRuntimeReasonFor(const char *reason) {
  if (reason &&
      std::strncmp(reason, kXessDllLoadFailure,
                   sizeof(kXessDllLoadFailure) - 1) == 0)
    return kXessRuntimeNotFound;
  if (reason &&
      std::strncmp(reason, kXessSymbolFailure,
                   sizeof(kXessSymbolFailure) - 1) == 0)
    return kXessRuntimeSymbolMissing;
  return kXessRuntimeUnavailable;
}

const char *XessRuntimeReason() {
  return XessRuntimeReasonFor(XessVulkanSupportInstance().UnavailableReason());
}
#endif

const char *ProviderUnavailableReason(TemporalUpscalerProvider provider,
                                       cGraphics *graphics) {
  switch (provider) {
  case TemporalUpscalerProvider::Off:
    return kProviderDisabled;
  case TemporalUpscalerProvider::Fsr:
#if !defined(HPL2_FSR_AVAILABLE) || !HPL2_FSR_AVAILABLE
    (void)graphics;
    return kFsrNotCompiled;
#else
    if (!IsFsrPlatformSupported())
      return kFsrUnsupportedPlatform;
    if (!graphics)
      return kFsrGraphicsUnavailable;
    if (!RIIsTargetSelected(RI_DEVICE_API_VK))
      return kFsrRequiresVulkan;
    return kFsrRequirementsUnavailable;
#endif
  case TemporalUpscalerProvider::XeSS:
#if !defined(HPL2_XESS_AVAILABLE) || !HPL2_XESS_AVAILABLE
    (void)graphics;
    return kXessNotCompiled;
#else
    if (!IsXessWindowsPlatform())
      return kXessUnsupportedPlatform;
    const cXessVulkanSupport &support = XessVulkanSupportInstance();
    const char *deviceReason =
        graphics ? graphics->device.xessUnavailableReason : nullptr;
    if (!support.IsAvailable()) {
      if (deviceReason && deviceReason[0] &&
          !IsXessRuntimeFailure(deviceReason))
        return kXessDeviceUnsupported;
      return XessRuntimeReason();
    }
    if (!graphics)
      return kXessGraphicsUnavailable;
    if (deviceReason && deviceReason[0]) {
      if (IsXessRuntimeFailure(deviceReason))
        return XessRuntimeReasonFor(deviceReason);
      return kXessDeviceUnsupported;
    }
    return kXessDeviceUnsupported;
#endif
  }

  (void)graphics;
  return kUnknownProvider;
}

bool ProviderAvailable(TemporalUpscalerProvider provider, cGraphics *graphics,
                       const char **outReason) {
  if (outReason)
    *outReason = nullptr;

  if (provider == TemporalUpscalerProvider::Off) {
    if (outReason)
      *outReason = kProviderDisabled;
    return false;
  }

  if (provider == TemporalUpscalerProvider::Fsr &&
      !IsFsrPlatformSupported()) {
    if (outReason)
      *outReason = kFsrUnsupportedPlatform;
    return false;
  }
#if defined(HPL2_FSR_AVAILABLE) && HPL2_FSR_AVAILABLE
  if (provider == TemporalUpscalerProvider::Fsr && graphics &&
      !RIIsTargetSelected(RI_DEVICE_API_VK)) {
    if (outReason)
      *outReason = kFsrRequiresVulkan;
    return false;
  }
#endif
  if (provider == TemporalUpscalerProvider::XeSS &&
      !IsXessWindowsPlatform()) {
    if (outReason)
      *outReason = kXessUnsupportedPlatform;
    return false;
  }

  if (AdapterSupports(provider, TemporalUpscalerQuality::Quality, graphics))
    return true;

  if (outReason)
    *outReason = ProviderUnavailableReason(provider, graphics);
  return false;
}

} // namespace

bool TemporalUpscalerAvailable(TemporalUpscalerProvider provider,
                               const char **outReason) {
  return ProviderAvailable(provider, Interface<cGraphics>::Get(), outReason);
}

uint32_t TemporalUpscalerAvailableQualities(
    TemporalUpscalerProvider provider, TemporalUpscalerQuality *outQualities,
    uint32_t maxQualities) {
  if (!outQualities || maxQualities == 0 ||
      !TemporalUpscalerAvailable(provider))
    return 0;

  cGraphics *graphics = Interface<cGraphics>::Get();
  uint32_t written = 0;
  for (TemporalUpscalerQuality quality : kQualities) {
    if (!AdapterSupports(provider, quality, graphics))
      continue;
    if (written == maxQualities)
      break;
    outQualities[written++] = quality;
  }
  return written;
}

TemporalUpscalerStatus
TemporalUpscalerQuery(const TemporalUpscalerSettings &desired) {
  TemporalUpscalerStatus status = {};
  status.requestedProvider = desired.provider;
  status.requestedQuality = desired.quality;

  if (desired.provider == TemporalUpscalerProvider::Off) {
    // Off needs no adapter, so it is a valid effective setting even though
    // TemporalUpscalerAvailable(Off) intentionally reports "provider
    // disabled" to callers deciding whether to create an adapter.
    status.available = true;
    return status;
  }

  cGraphics *graphics = Interface<cGraphics>::Get();
  const char *providerReason = nullptr;
  if (!ProviderAvailable(desired.provider, graphics, &providerReason)) {
    status.unavailableReason = providerReason;
    return status;
  }

  if (!AdapterSupports(desired.provider, desired.quality, graphics)) {
    status.unavailableReason = kQualityUnsupported;
    return status;
  }

  status.available = true;
  status.effectiveProvider = desired.provider;
  status.effectiveQuality = desired.quality;
  return status;
}

std::unique_ptr<iTemporalUpscaler> TemporalUpscalerCreate(
    TemporalUpscalerProvider provider, cGraphics *graphics) {
  if (provider == TemporalUpscalerProvider::Off)
    return nullptr;

  cGraphics *resolvedGraphics = graphics ? graphics : Interface<cGraphics>::Get();
  if (!ProviderAvailable(provider, resolvedGraphics, nullptr))
    return nullptr;

  switch (provider) {
  case TemporalUpscalerProvider::Fsr:
#if defined(HPL2_FSR_AVAILABLE) && HPL2_FSR_AVAILABLE
    return std::make_unique<cFsrUpscaler>(resolvedGraphics);
#else
    return nullptr;
#endif
  case TemporalUpscalerProvider::XeSS:
#if defined(HPL2_XESS_AVAILABLE) && HPL2_XESS_AVAILABLE
    return std::make_unique<cXessUpscaler>(resolvedGraphics);
#else
    return nullptr;
#endif
  case TemporalUpscalerProvider::Off:
    return nullptr;
  }

  return nullptr;
}

} // namespace hpl
