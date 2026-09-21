#include "graphics/TemporalUpscaler.h"

#include "engine/Interface.h"
#include "graphics/FfxApiLoader.h"
#include "graphics/FsrUpscaler.h"
#include "graphics/RIDevice.h"
#include "graphics/XessUpscaler.h"

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
    "FSR graphics device is unavailable";
constexpr const char *kFsrRequirementsUnavailable =
    "FSR requirements are not satisfied";
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
    "XeSS graphics device is unavailable";
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
  return XessRuntimeReasonFor(XessSupportInstance().UnavailableReason());
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
    // The loader already spells out the per-backend failure -- a build with no
    // module for the live renderer, or a module that would not load -- so it
    // gives a better reason than any fixed string here could. FfxApiFor caches
    // one FfxApi per backend in a function-local static, so this pointer into
    // its unavailableReason buffer stays valid. cFsrUpscaler's own reason is
    // not reachable from here: AdapterSupports builds a stack temporary whose
    // supportFailure dies with it, so a DeviceIsUsable failure falls through
    // to the generic string below.
    {
      const hpl::FfxApi &api = hpl::FfxApiActive();
      if (!api.available)
        return api.unavailableReason;
    }
    return kFsrRequirementsUnavailable;
#endif
  case TemporalUpscalerProvider::XeSS:
#if !defined(HPL2_XESS_AVAILABLE) || !HPL2_XESS_AVAILABLE
    (void)graphics;
    return kXessNotCompiled;
#else
    if (!IsXessWindowsPlatform())
      return kXessUnsupportedPlatform;
    // The loader now carries the whole verdict: a DLL or symbol problem, the
    // build having no arm for the active backend, or a veto recorded while the
    // device was being built because RI could not honour what XeSS asked for.
    // Only the first of those is a runtime-installation problem; the rest read
    // to the player as "this device cannot do it".
    const cXessSupport &support = XessSupportInstance();
    if (!support.IsAvailable()) {
      if (IsXessRuntimeFailure(support.UnavailableReason()))
        return XessRuntimeReason();
      return kXessDeviceUnsupported;
    }
    if (!graphics)
      return kXessGraphicsUnavailable;
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
  // No backend gate for FSR: cFsrUpscaler::Supports -> DeviceIsUsable picks
  // the live backend's device handles one arm at a time, and the ffx-api
  // module is resolved per backend, so an unsupported renderer is rejected
  // there with a reason that names it.
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
