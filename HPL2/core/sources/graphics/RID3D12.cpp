#include "graphics/RID3D12.h"

#if DEVICE_IMPL_D3D12

uint32_t g_riD3D12EnhancedBarrierCallCount = 0;
uint32_t g_riD3D12LegacyBarrierCallCount = 0;
std::atomic<uint32_t> g_riD3D12DescriptorCacheEntries{0};

#include "graphics/RIRenderer.h"
#include "graphics/RIDevice.h"
#include "graphics/RICommand.h"
#include "graphics/RIDescriptorSetAllocator.h"
#include "system/LowLevelSystem.h"

#include <d3d12sdklayers.h>
#include <dxgidebug.h>
#include <D3D12MemAlloc.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>
#include <cstring>

bool g_riD3D12EnableDebugLayer = false;
static bool g_riD3D12DredEnabled = false;
static uint8_t g_riD3D12DredMode = RI_D3D12_DRED_DEFAULT; // RID3D12DredMode_e

static std::mutex g_riD3D12AdapterMutex;
static std::vector<IDXGIAdapter4 *> g_riD3D12Adapters;
static bool g_riD3D12OwnsCOM = false;

// Device-loss and debug-layer diagnostics go to stderr and hpl.log: the game
// is a WinMain app with no console, so stderr alone is usually lost.
static void ri_d3d12_diag(const char *fmt, ...) {
  char text[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(text, sizeof(text), fmt, ap);
  va_end(ap);
  text[sizeof(text) - 1] = '\0';
  fputs(text, stderr);
  fflush(stderr);
  hpl::Log("%s", text);
}

static void CALLBACK ri_d3d12_info_queue_callback(
    D3D12_MESSAGE_CATEGORY category,
    D3D12_MESSAGE_SEVERITY severity,
    D3D12_MESSAGE_ID id,
    LPCSTR description,
    void *context) {
  (void)context;
  ri_d3d12_diag("RI D3D12 debug: sev=%d cat=%d id=%d %s\n",
          int(severity), int(category), int(id),
          description ? description : "<null>");
}

static void ri_d3d12_drain_info_queue(ID3D12InfoQueue *iq) {
  if (!iq)
    return;
  const UINT64 count = iq->GetNumStoredMessagesAllowedByRetrievalFilter();
  for (UINT64 idx = 0; idx < count; ++idx) {
    SIZE_T size = 0;
    if (FAILED(iq->GetMessageA(idx, nullptr, &size)) || size == 0)
      continue;
    std::vector<uint8_t> buffer(size);
    D3D12_MESSAGE *msg = reinterpret_cast<D3D12_MESSAGE *>(buffer.data());
    if (FAILED(iq->GetMessageA(idx, msg, &size)) || !msg)
      continue;
    ri_d3d12_diag("RI D3D12 debug: sev=%d cat=%d id=%d %s\n",
            int(msg->Severity), int(msg->Category), int(msg->ID),
            msg->pDescription ? msg->pDescription : "<null>");
  }
  iq->ClearStoredMessages();
}

static inline enum RIVendor_e ri_d3d12_vendor_from_id(uint32_t vendorId) {
  switch (vendorId) {
  case 0x10DE:
    return RI_NVIDIA;
  case 0x1002:
    return RI_AMD;
  case 0x8086:
    return RI_INTEL;
  default:
    return RI_UNKNOWN;
  }
}

static bool ri_d3d12_feature_level(IDXGIAdapter4 *adapter,
                                   D3D_FEATURE_LEVEL *level, uint8_t *major,
                                   uint8_t *minor) {
  static const D3D_FEATURE_LEVEL levels[] = {
      D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1,
      D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1,
      D3D_FEATURE_LEVEL_11_0};
  static const uint8_t levelMajor[] = {12, 12, 12, 11, 11};
  static const uint8_t levelMinor[] = {2, 1, 0, 1, 0};
  for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); ++i) {
    HRESULT hr = D3D12CreateDevice(adapter, levels[i], __uuidof(ID3D12Device), nullptr);
    if (D3D12_WrapResult(hr)) {
      *level = levels[i];
      *major = levelMajor[i];
      *minor = levelMinor[i];
      return true;
    }
  }
  return false;
}

// One summary line per enumerated adapter. Both enumeration passes (hardware
// and WARP) print the same fields, so they share this: a field added to only
// one of two copies is a log that disagrees with itself. `dxr` is the raw tier
// the driver reported, which is what tells a bug report apart from a mapping
// bug in the saturating RT value beside it.
static void ri_d3d12_log_adapter(const struct RIPhysicalAdapter &adapter) {
  hpl::Log("RI D3D12 adapter: %s vendor=%u type=%u FL=%u.%u SM=%u.%u "
           "RT=%u (dxr=%u)\n",
           adapter.name, adapter.vendor, adapter.type,
           adapter.d3d12.highestFeatureLevelMajor,
           adapter.d3d12.highestFeatureLevelMinor,
           adapter.d3d12.highestShaderModelMajor,
           adapter.d3d12.highestShaderModelMinor,
           adapter.d3d12.rayTracingTier, adapter.d3d12.rayTracingTierNative);
}

static bool ri_d3d12_populate_adapter(IDXGIAdapter4 *src, bool isWarp,
                                      struct RIPhysicalAdapter &dst) {
  DXGI_ADAPTER_DESC3 desc = {};
  HRESULT hr = src->GetDesc3(&desc);
  if (!D3D12_WrapResult(hr))
    return false;

  memset(&dst, 0, sizeof(dst));
  int nameLength = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1,
                                       dst.name, sizeof(dst.name), nullptr, nullptr);
  if (nameLength == 0)
    dst.name[0] = '\0';
  else
    dst.name[sizeof(dst.name) - 1] = '\0';

  dst.luid = (uint64_t(uint32_t(desc.AdapterLuid.HighPart)) << 32) |
             uint32_t(desc.AdapterLuid.LowPart);
  dst.videoMemorySize = desc.DedicatedVideoMemory;
  dst.systemMemorySize = desc.SharedSystemMemory + desc.DedicatedSystemMemory;
  dst.deviceId = desc.DeviceId;
  dst.vendor = ri_d3d12_vendor_from_id(desc.VendorId);
  dst.type = isWarp ? RI_ADAPTER_TYPE_CPU : RI_ADAPTER_TYPE_DISCRETE_GPU;
  // D3D12 exposes a fixed eight-slot output-merger render-target array.
  // Publish it through the backend-neutral adapter limits so renderers do not
  // mistake the zero-initialized field for an incapable device and reject
  // their MRT viewport state before recording any scene draws.
  dst.colorAttachmentMaxNum = D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT;

  D3D_FEATURE_LEVEL bestLevel = D3D_FEATURE_LEVEL_11_0;
  if (!ri_d3d12_feature_level(src, &bestLevel,
                              &dst.d3d12.highestFeatureLevelMajor,
                              &dst.d3d12.highestFeatureLevelMinor))
    return false;

  src->AddRef();
  dst.d3d12.adapter = src;
  dst.d3d12.dedicatedVideoMemory = desc.DedicatedVideoMemory;
  dst.d3d12.dedicatedSystemMemory = desc.DedicatedSystemMemory;
  dst.d3d12.sharedSystemMemory = desc.SharedSystemMemory;
  dst.d3d12.vendorId = desc.VendorId;
  dst.d3d12.deviceId = desc.DeviceId;
  dst.d3d12.isWarp = isWarp;

  ID3D12Device *probe = nullptr;
  hr = D3D12CreateDevice(src, bestLevel, IID_PPV_ARGS(&probe));
  if (!D3D12_WrapResult(hr)) {
    dst.d3d12.adapter->Release();
    memset(&dst, 0, sizeof(dst));
    return false;
  }

  // A UMA adapter shares system memory with the CPU: an integrated GPU. Rank
  // it below a discrete one so adapter selection prefers dedicated hardware.
  D3D12_FEATURE_DATA_ARCHITECTURE1 architecture = {};
  if (!isWarp && SUCCEEDED(probe->CheckFeatureSupport(
                     D3D12_FEATURE_ARCHITECTURE1, &architecture,
                     sizeof(architecture))) &&
      architecture.UMA)
    dst.type = RI_ADAPTER_TYPE_INTEGRATED_GPU;

  D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
  if (D3D12_WrapResult(probe->CheckFeatureSupport(
          D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)))) {
    dst.d3d12.resourceBindingTier = uint8_t(options.ResourceBindingTier);
    dst.bindlessTier = options.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_3 ? 1 : 0;
  }
  D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1 = {};
  if (D3D12_WrapResult(probe->CheckFeatureSupport(
          D3D12_FEATURE_D3D12_OPTIONS1, &options1, sizeof(options1))))
    dst.isShaderNativeI64Supported = options1.Int64ShaderOps ? 1 : 0;
  D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
  if (D3D12_WrapResult(probe->CheckFeatureSupport(
          D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5)))) {
    // D3D12_RAYTRACING_TIER is ordered and open-ended -- 1.2 (=12) already
    // exists and ships on Turing and later -- so compare with >=. An equality
    // chain makes every tier past the newest one it names read as "not
    // supported", which is how an RTX 3070 came up as a raster-only adapter
    // while WARP, still reporting 1.1, looked ray-tracing capable.
    dst.d3d12.rayTracingTierNative = uint8_t(options5.RaytracingTier);
    dst.d3d12.rayTracingTier =
        options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1 ? 2 :
        options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_0 ? 1 : 0;
  }
  // Publish the backend-neutral capability bits every caller reads. Without
  // these the adapter looks incapable of everything, because nothing outside
  // this file speaks D3D12 tiers.
  dst.rayTracingTier = dst.d3d12.rayTracingTier;
  dst.isRayTracingSupported = dst.d3d12.rayTracingTier >= 1 ? 1 : 0;
  dst.isRayQuerySupported = dst.d3d12.rayTracingTier >= 2 ? 1 : 0;
  // D3D12 provides these unconditionally: a swapchain needs no device feature
  // (DXGI is a precondition of enumeration reaching this point), GPU virtual
  // addresses are intrinsic to ID3D12Resource, HLSL packs structured buffers
  // natively, and there are no render pass objects to opt out of.
  dst.isSwapChainSupported = 1;
  dst.isBufferDeviceAddressSupported = 1;
  dst.isShaderStorageScalarLayoutSupported = 1;
  dst.isDynamicRenderingSupported = 1;
  // Quad wave intrinsics (QuadReadAcrossX/Y, what NRD's REBLUR passes use) are core DXIL from SM 6.0
  dst.isComputeShaderDerivativesSupported = 1;
  D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7 = {};
  if (D3D12_WrapResult(probe->CheckFeatureSupport(
          D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7))))
    dst.d3d12.meshShaderTier = uint8_t(options7.MeshShaderTier);
  D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12 = {};
  if (D3D12_WrapResult(probe->CheckFeatureSupport(
          D3D12_FEATURE_D3D12_OPTIONS12, &options12, sizeof(options12))))
    dst.isEnchancedBarrierSupported = options12.EnhancedBarriersSupported ? 1 : 0;

  // HighestShaderModel is an in/out field: the runtime reports at most the
  // value passed in, and rejects values newer than it knows with E_INVALIDARG.
  // Start above the newest model and step down until the runtime accepts one.
  for (uint32_t candidate = 0x69; candidate >= 0x60; --candidate) {
    D3D12_FEATURE_DATA_SHADER_MODEL shaderModel = {
        static_cast<D3D_SHADER_MODEL>(candidate)};
    if (SUCCEEDED(probe->CheckFeatureSupport(
            D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel)))) {
      const uint32_t value = uint32_t(shaderModel.HighestShaderModel);
      dst.d3d12.highestShaderModelMajor = uint8_t(value >> 4);
      dst.d3d12.highestShaderModelMinor = uint8_t(value & 0xf);
      break;
    }
  }
  probe->Release();
  dst.uploadBufferTextureRowAlignment = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
  dst.uploadBufferOffsetAlignment = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
  dst.constantBufferOffsetAlignment =
      D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
  dst.accelerationStructureScratchOffsetAlignment =
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT;
  return true;
}

int RID3D12_InitRenderer(struct RIRenderer &renderer,
                         const struct RIBackendInit *init) {
  memset(&renderer.d3d12, 0, sizeof(renderer.d3d12));
  const uint8_t validationLevel =
      init ? init->d3d12.validationLevel : RI_D3D12_VALIDATION_LEVEL_NONE;
  if (validationLevel != RI_D3D12_VALIDATION_LEVEL_NONE)
    g_riD3D12EnableDebugLayer = true;
  g_riD3D12DredMode = init ? init->d3d12.dredMode : RI_D3D12_DRED_DEFAULT;
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (hr == S_OK)
    g_riD3D12OwnsCOM = true;
  else if (hr != S_FALSE && hr != RPC_E_CHANGED_MODE && FAILED(hr)) {
    D3D12_WrapResult(hr);
    return RI_FAIL;
  }

  if (g_riD3D12EnableDebugLayer) {
    hr = D3D12GetDebugInterface(IID_PPV_ARGS(&renderer.d3d12.debug));
    if (D3D12_WrapResult(hr)) {
      renderer.d3d12.debug->EnableDebugLayer();
      renderer.d3d12.enableDebugLayer = 1;

      HMODULE sdkLayers = GetModuleHandleW(L"d3d12SDKLayers.dll");
      if (sdkLayers) {
        wchar_t sdkLayersPath[MAX_PATH] = {};
        DWORD pathLength = GetModuleFileNameW(sdkLayers, sdkLayersPath,
                                               ARRAYSIZE(sdkLayersPath));
        if (pathLength != 0) {
          char utf8Path[ MAX_PATH * 4 ] = {};
          int utf8Length = WideCharToMultiByte(
              CP_UTF8, 0, sdkLayersPath, int(pathLength), utf8Path,
              sizeof(utf8Path) - 1, nullptr, nullptr);
          if (utf8Length > 0) {
            utf8Path[utf8Length] = '\0';
            hpl::Log("RI D3D12: d3d12SDKLayers.dll loaded from %s\n", utf8Path);
          } else {
            hpl::Log("RI D3D12: d3d12SDKLayers.dll loaded (path conversion failed)\n");
          }
        } else {
          hpl::Log("RI D3D12: d3d12SDKLayers.dll loaded (path unavailable)\n");
        }
      } else {
        hpl::Log("RI D3D12: d3d12SDKLayers.dll not loaded\n");
      }

      HMODULE process = GetModuleHandleW(nullptr);
      const UINT *sdkVersion = process
          ? reinterpret_cast<const UINT *>(GetProcAddress(process, "D3D12SDKVersion"))
          : nullptr;
      if (sdkVersion)
        hpl::Log("RI D3D12: current process D3D12SDKVersion=%u\n", *sdkVersion);
      else
        hpl::Log("RI D3D12: current process D3D12SDKVersion unavailable\n");

      renderer.d3d12.enableGpuValidation = 0;
      if (validationLevel >= RI_D3D12_VALIDATION_LEVEL_GPU_BASED) {
        ID3D12Debug1 *debug1 = nullptr;
        hr = renderer.d3d12.debug->QueryInterface(IID_PPV_ARGS(&debug1));
        if (D3D12_WrapResult(hr)) {
          debug1->SetEnableGPUBasedValidation(TRUE);
          renderer.d3d12.enableGpuValidation = 1;
          debug1->Release();
        } else {
          hpl::Warning("RI D3D12: GPU-based validation unavailable; continuing without it\n");
        }
      }
    } else {
      hpl::Warning("RI D3D12: debug layer unavailable\n");
      renderer.d3d12.debug = nullptr;
    }
  }

  // The DXGI debug layer ships with the Graphics Tools optional feature, so a
  // debug factory can fail where the D3D12 debug layer works; fall back to a
  // plain factory.
  const UINT factoryFlags =
      g_riD3D12EnableDebugLayer ? DXGI_CREATE_FACTORY_DEBUG : 0;
  hr = CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&renderer.d3d12.factory));
  if (FAILED(hr) && factoryFlags != 0) {
    hpl::Warning("RI D3D12: DXGI debug factory unavailable (hr=0x%08x); using a plain factory\n",
                 unsigned(hr));
    hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&renderer.d3d12.factory));
  }
  if (!D3D12_WrapResult(hr)) {
    if (renderer.d3d12.debug)
      renderer.d3d12.debug->Release();
    memset(&renderer.d3d12, 0, sizeof(renderer.d3d12));
    return RI_FAIL;
  }
  hpl::Log("RI D3D12: factory created (D3D12 debug=%u, GPU validation=%u)\n",
           renderer.d3d12.enableDebugLayer, renderer.d3d12.enableGpuValidation);
  return RI_SUCCESS;
}

void RID3D12_ShutdownRenderer(struct RIRenderer &renderer) {
  std::lock_guard<std::mutex> lock(g_riD3D12AdapterMutex);
  const bool reportLiveObjects = renderer.d3d12.enableDebugLayer != 0;
  for (IDXGIAdapter4 *adapter : g_riD3D12Adapters)
    adapter->Release();
  g_riD3D12Adapters.clear();
  if (renderer.d3d12.factory)
    renderer.d3d12.factory->Release();
  if (renderer.d3d12.debug)
    renderer.d3d12.debug->Release();
  memset(&renderer.d3d12, 0, sizeof(renderer.d3d12));
  // Everything the renderer owns is released by now, so any D3D12/DXGI object
  // still alive is a leak. The report goes to the debugger output.
  if (reportLiveObjects) {
    IDXGIDebug1 *dxgiDebug = nullptr;
    if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&dxgiDebug)))) {
      hpl::Log("RI D3D12: reporting live DXGI/D3D12 objects to the debugger\n");
      dxgiDebug->ReportLiveObjects(
          DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_FLAGS(DXGI_DEBUG_RLO_SUMMARY |
                                               DXGI_DEBUG_RLO_IGNORE_INTERNAL));
      dxgiDebug->Release();
    }
  }
  if (g_riD3D12OwnsCOM) {
    CoUninitialize();
    g_riD3D12OwnsCOM = false;
  }
}

int RID3D12_EnumerateAdapters(struct RIRenderer &renderer,
                              struct RIPhysicalAdapter *adapters,
                              uint32_t *numAdapters) {
  if (!renderer.d3d12.factory || !numAdapters)
    return RI_FAIL;
  uint32_t capacity = *numAdapters;
  uint32_t count = 0;
  std::lock_guard<std::mutex> lock(g_riD3D12AdapterMutex);
  for (IDXGIAdapter4 *adapter : g_riD3D12Adapters)
    adapter->Release();
  g_riD3D12Adapters.clear();

  auto enumerate_pass = [&]() {
    for (UINT index = 0;; ++index) {
      IDXGIAdapter4 *hardware = nullptr;
      HRESULT hr = renderer.d3d12.factory->EnumAdapterByGpuPreference(
          index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&hardware));
      if (hr == DXGI_ERROR_NOT_FOUND)
        break;
      if (!D3D12_WrapResult(hr))
        break;
      DXGI_ADAPTER_DESC3 desc = {};
      hr = hardware->GetDesc3(&desc);
      if (!D3D12_WrapResult(hr) || (desc.Flags & (DXGI_ADAPTER_FLAG3_REMOTE | DXGI_ADAPTER_FLAG3_SOFTWARE))) {
        hardware->Release();
        continue;
      }
      RIPhysicalAdapter temp;
      if (ri_d3d12_populate_adapter(hardware, false, temp)) {
        if (adapters && count < capacity)
          adapters[count] = temp;
        ri_d3d12_log_adapter(temp);
        g_riD3D12Adapters.push_back(temp.d3d12.adapter);
        ++count;
      }
      hardware->Release();
    }

    IDXGIAdapter4 *warp = nullptr;
    HRESULT hr = renderer.d3d12.factory->EnumWarpAdapter(IID_PPV_ARGS(&warp));
    if (D3D12_WrapResult(hr)) {
      RIPhysicalAdapter temp;
      if (ri_d3d12_populate_adapter(warp, true, temp)) {
        if (adapters && count < capacity)
          adapters[count] = temp;
        ri_d3d12_log_adapter(temp);
        g_riD3D12Adapters.push_back(temp.d3d12.adapter);
        ++count;
      }
      warp->Release();
    }
  };

  enumerate_pass();
  if (count == 0) {
    hpl::Warning("RI D3D12: adapter enumeration returned 0; retrying after IDXGIFactory reset\n");
    renderer.d3d12.factory->Release();
    renderer.d3d12.factory = nullptr;
    const UINT factoryFlags =
        g_riD3D12EnableDebugLayer ? DXGI_CREATE_FACTORY_DEBUG : 0;
    HRESULT hr =
        CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&renderer.d3d12.factory));
    if (FAILED(hr) && factoryFlags != 0)
      hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&renderer.d3d12.factory));
    if (!D3D12_WrapResult(hr))
      return RI_FAIL;
    enumerate_pass();
  }
  *numAdapters = adapters ? (count < capacity ? count : capacity) : count;
  hpl::Log("RI D3D12: enumerated %u adapters\n", count);
  return RI_SUCCESS;
}

// DRED (Device Removed Extended Data) records auto-breadcrumbs and page-fault
// allocation history so a device removal can name the faulting command list
// and resource.  The settings are process-global and must be applied before
// D3D12CreateDevice.  Enabled with the debug layer or in debug builds unless
// RIBackendInit::d3d12.dredMode forces it; the overhead is small.
static void ri_d3d12_enable_dred() {
  bool want = g_riD3D12EnableDebugLayer;
#ifdef _DEBUG
  want = true;
#endif
  if (g_riD3D12DredMode != RI_D3D12_DRED_DEFAULT)
    want = g_riD3D12DredMode == RI_D3D12_DRED_ON;
  g_riD3D12DredEnabled = false;
  if (!want)
    return;
  ID3D12DeviceRemovedExtendedDataSettings1 *settings1 = nullptr;
  if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&settings1)))) {
    settings1->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    settings1->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    settings1->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    settings1->Release();
    g_riD3D12DredEnabled = true;
  } else {
    ID3D12DeviceRemovedExtendedDataSettings *settings = nullptr;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&settings)))) {
      settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
      settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
      settings->Release();
      g_riD3D12DredEnabled = true;
    }
  }
  hpl::Log("RI D3D12: DRED %s\n",
           g_riD3D12DredEnabled ? "enabled" : "unavailable");
}

int RID3D12_InitDevice(struct RIDevice &device, const struct RIDeviceDesc *init) {
  memset(&device, 0, sizeof(device));
  if (!init || !init->physicalAdapter ||
      init->physicalAdapter->d3d12.highestFeatureLevelMajor < 11)
    return RI_INCOMPLETE_DEVICE;
  device.physicalAdapter = *init->physicalAdapter;
  D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  if (device.physicalAdapter.d3d12.highestFeatureLevelMajor == 12)
    level = device.physicalAdapter.d3d12.highestFeatureLevelMinor == 2 ? D3D_FEATURE_LEVEL_12_2 :
            device.physicalAdapter.d3d12.highestFeatureLevelMinor == 1 ? D3D_FEATURE_LEVEL_12_1 : D3D_FEATURE_LEVEL_12_0;
  else if (device.physicalAdapter.d3d12.highestFeatureLevelMinor == 1)
    level = D3D_FEATURE_LEVEL_11_1;
  ri_d3d12_enable_dred();
  HRESULT hr = D3D12CreateDevice(device.physicalAdapter.d3d12.adapter, level,
                                 IID_PPV_ARGS(&device.d3d12.device));
  if (!D3D12_WrapResult(hr)) {
    memset(&device.d3d12, 0, sizeof(device.d3d12));
    return RI_FAIL;
  }

  // ID3D12Device5 carries the DXR entry points
  // (GetRaytracingAccelerationStructurePrebuildInfo, CreateStateObject).
  if (FAILED(device.d3d12.device->QueryInterface(
          IID_PPV_ARGS(&device.d3d12.device5))))
    device.d3d12.device5 = nullptr;

  // Publish what this device actually enabled, mirroring what the Vulkan path
  // does after vkCreateDevice (RIRenderer.cpp). A ray-tracing request is
  // serviceable only with ID3D12Device5, which is knowable only from a live
  // device, so that is checked here rather than off the adapter's tier.
  {
    const uint8_t tier = device.physicalAdapter.d3d12.rayTracingTier;
    if (init->requestRayTracing && !device.d3d12.device5) {
      hpl::Log("ERROR: RI D3D12: adapter reports ray tracing tier %u but "
               "ID3D12Device5 is unavailable\n",
               tier);
      RID3D12_DisposeDevice(device);
      return RI_INCOMPLETE_DEVICE;
    }
    device.accelerationStructureEnabled = init->requestRayTracing != 0;
    // Acceleration structures and ray query are implemented; ray-tracing
    // *pipelines* (state objects, SBT, DispatchRays) are gated separately so we
    // never advertise a capability the backend cannot service.
    device.rayTracingPipelineEnabled =
        init->requestRayTracing != 0 && (device.d3d12.device5 != nullptr &&
         device.physicalAdapter.d3d12.rayTracingTier >= 1);
    device.rayQueryEnabled = init->requestRayQuery != 0;
    device.rayTracingEnabled = device.accelerationStructureEnabled &&
                               device.rayTracingPipelineEnabled;
    // D3D12_QUERY_TYPE_OCCLUSION always returns exact sample counts; there is
    // no equivalent of Vulkan's occlusionQueryPrecise feature gate.
    device.occlusionQueryPreciseEnabled = true;
    // Nothing to enable: quad wave intrinsics come with the shader model.
    device.computeShaderDerivativesEnabled = true;
    if (tier >= 1) {
      device.physicalAdapter.rayTracingShaderGroupIdentifierSize =
          D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
      device.physicalAdapter.rayTracingShaderTableMaxStride =
          D3D12_RAYTRACING_MAX_SHADER_RECORD_STRIDE;
      device.physicalAdapter.rayTracingShaderRecursionMaxDepth =
          D3D12_RAYTRACING_MAX_DECLARABLE_TRACE_RECURSION_DEPTH;
      device.physicalAdapter.rayTracingGeometryObjectMaxNum =
          D3D12_RAYTRACING_MAX_GEOMETRIES_PER_BOTTOM_LEVEL_ACCELERATION_STRUCTURE;
    }
  }

  D3D12MA::ALLOCATOR_DESC allocatorDesc = {};
  allocatorDesc.pDevice = device.d3d12.device;
  allocatorDesc.pAdapter = device.physicalAdapter.d3d12.adapter;
  hr = D3D12MA::CreateAllocator(&allocatorDesc, &device.d3d12.allocator);
  if (!D3D12_WrapResult(hr)) {
    RID3D12_DisposeDevice(device);
    return RI_FAIL;
  }

  D3D12_INDIRECT_ARGUMENT_DESC drawArgument = {};
  drawArgument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
  D3D12_COMMAND_SIGNATURE_DESC drawSignature = {};
  drawSignature.ByteStride = sizeof(D3D12_DRAW_ARGUMENTS);
  drawSignature.NumArgumentDescs = 1;
  drawSignature.pArgumentDescs = &drawArgument;
  hr = device.d3d12.device->CreateCommandSignature(
      &drawSignature, nullptr,
      IID_PPV_ARGS(&device.d3d12.drawIndirectSignature));
  if (!D3D12_WrapResult(hr)) {
    RID3D12_DisposeDevice(device);
    return RI_FAIL;
  }
  drawSignature.ByteStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
  hr = device.d3d12.device->CreateCommandSignature(
      &drawSignature, nullptr,
      IID_PPV_ARGS(&device.d3d12.drawIndirectPaddedSignature));
  if (!D3D12_WrapResult(hr)) {
    RID3D12_DisposeDevice(device);
    return RI_FAIL;
  }
  D3D12_INDIRECT_ARGUMENT_DESC indexedArgument = {};
  indexedArgument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
  D3D12_COMMAND_SIGNATURE_DESC indexedSignature = {};
  indexedSignature.ByteStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
  indexedSignature.NumArgumentDescs = 1;
  indexedSignature.pArgumentDescs = &indexedArgument;
  hr = device.d3d12.device->CreateCommandSignature(
      &indexedSignature, nullptr,
      IID_PPV_ARGS(&device.d3d12.drawIndexedIndirectSignature));
  if (!D3D12_WrapResult(hr)) {
    RID3D12_DisposeDevice(device);
    return RI_FAIL;
  }

  if (!initDescriptorArena(&device)) {
    RID3D12_DisposeDevice(device);
    return RI_FAIL;
  }
  device.d3d12.nextGeometrySrvIndex = 0;
  const D3D12_COMMAND_LIST_TYPE types[RI_QUEUE_LEN] = {
      D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_TYPE_COMPUTE,
      D3D12_COMMAND_LIST_TYPE_COPY};
  const uint8_t flags[RI_QUEUE_LEN] = {
      RI_QUEUE_GRAPHICS_BIT | RI_QUEUE_COMPUTE_BIT | RI_QUEUE_TRANSFER_BIT,
      RI_QUEUE_COMPUTE_BIT | RI_QUEUE_TRANSFER_BIT, RI_QUEUE_TRANSFER_BIT};
  for (uint32_t i = 0; i < RI_QUEUE_LEN; ++i) {
    D3D12_COMMAND_QUEUE_DESC desc = {};
    desc.Type = types[i];
    hr = device.d3d12.device->CreateCommandQueue(
        &desc, IID_PPV_ARGS(&device.d3d12.queues[i]));
    if (!D3D12_WrapResult(hr)) {
      RID3D12_DisposeDevice(device);
      return RI_FAIL;
    }
    device.queues[i].d3d12.queue = device.d3d12.queues[i];
    device.queues[i].d3d12.type = uint8_t(types[i]);
    device.queues[i].d3d12.flags = flags[i];
    hr = device.d3d12.device->CreateFence(
        0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&device.queues[i].d3d12.fence));
    if (!D3D12_WrapResult(hr)) {
      RID3D12_DisposeDevice(device);
      return RI_FAIL;
    }
    device.queues[i].d3d12.fenceEvent =
        CreateEventEx(nullptr, nullptr, 0, EVENT_MODIFY_STATE | SYNCHRONIZE);
    if (!device.queues[i].d3d12.fenceEvent) {
      RID3D12_DisposeDevice(device);
      return RI_FAIL;
    }
    device.queues[i].d3d12.nextFenceValue = 0;
  }
  if (g_riD3D12EnableDebugLayer) {
    if (D3D12_WrapResult(device.d3d12.device->QueryInterface(
            IID_PPV_ARGS(&device.d3d12.infoQueue)))) {
      // HPL_D3D12_BREAK_ON_ERROR=1 stops the attached debugger on the API call
      // that produced the error, which is the only way to get a call stack for
      // void-returning calls that remove the device.
      const char *breakEnv = getenv("HPL_D3D12_BREAK_ON_ERROR");
      const BOOL breakOnError = breakEnv && atoi(breakEnv) != 0 ? TRUE : FALSE;
      device.d3d12.infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, breakOnError);
      device.d3d12.infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, breakOnError);
      // Warnings the renderer triggers by design: D3D12MA places several
      // buffers in one heap range, and enhanced-barrier resources ignore the
      // legacy initial state.
      D3D12_MESSAGE_ID denyIds[] = {
          D3D12_MESSAGE_ID_HEAP_ADDRESS_RANGE_INTERSECTS_MULTIPLE_BUFFERS,
          D3D12_MESSAGE_ID_CREATERESOURCE_STATE_IGNORED,
      };
      D3D12_INFO_QUEUE_FILTER filter = {};
      filter.DenyList.NumIDs = UINT(sizeof(denyIds) / sizeof(denyIds[0]));
      filter.DenyList.pIDList = denyIds;
      D3D12_WrapResult(device.d3d12.infoQueue->PushStorageFilter(&filter));
      HRESULT hr1 = device.d3d12.infoQueue->QueryInterface(
          IID_PPV_ARGS(&device.d3d12.infoQueue1));
      if (SUCCEEDED(hr1) && device.d3d12.infoQueue1) {
        if (D3D12_WrapResult(device.d3d12.infoQueue1->RegisterMessageCallback(
                &ri_d3d12_info_queue_callback,
                D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr,
                &device.d3d12.infoQueueCookie))) {
          // The callback already logs every message; stop the debug layer
          // printing each one a second time to the debugger output.
          device.d3d12.infoQueue1->SetMuteDebugOutput(TRUE);
        } else {
          device.d3d12.infoQueue1->Release();
          device.d3d12.infoQueue1 = nullptr;
          device.d3d12.infoQueueCookie = 0;
        }
      }
    } else {
      hpl::Warning("RI D3D12: ID3D12InfoQueue unavailable; debug-layer messages will not be drained\n");
      device.d3d12.infoQueue = nullptr;
    }
  }
  // D3D12 has no equivalent of RIVkDeviceRequirements: there is nothing to
  // negotiate before device creation, so an SDK that needs D3D12 decides on its
  // own whether it can run once this device exists.
  return RI_SUCCESS;
}

void RID3D12_DisposeDevice(struct RIDevice &device) {
  // Drain every queue while its fence objects are still valid.  Descriptor
  // arena ranges and RIBuffer resources can be referenced by submitted work;
  // releasing queue state first would make completion-gated retirement
  // impossible and could leave shader-visible heaps/resources live against a
  // dead device.
  for (uint32_t i = 0; i < RI_QUEUE_LEN; ++i)
    device.queues[i].waitIdle(&device);
  RID3D12_DrainBufferRegistry(device);
  reclaimDescriptorArena(&device);
  freeDescriptorArena(&device);
  for (uint32_t i = 0; i < RI_QUEUE_LEN; ++i) {
    if (device.queues[i].d3d12.fence)
      device.queues[i].d3d12.fence->Release();
    if (device.queues[i].d3d12.fenceEvent)
      CloseHandle(device.queues[i].d3d12.fenceEvent);
    if (device.d3d12.queues[i])
      device.d3d12.queues[i]->Release();
    memset(&device.queues[i].d3d12, 0, sizeof(device.queues[i].d3d12));
  }
  if (device.d3d12.infoQueue1) {
    if (device.d3d12.infoQueueCookie)
      device.d3d12.infoQueue1->UnregisterMessageCallback(device.d3d12.infoQueueCookie);
    device.d3d12.infoQueue1->Release();
    device.d3d12.infoQueue1 = nullptr;
    device.d3d12.infoQueueCookie = 0;
  }
  if (device.d3d12.infoQueue) {
    ri_d3d12_drain_info_queue(device.d3d12.infoQueue);
    device.d3d12.infoQueue->Release();
    device.d3d12.infoQueue = nullptr;
  }
  // The arena was drained before queue teardown.  Keep this second call
  // harmless for partial-init paths and any ranges released during teardown.
  freeDescriptorArena(&device);
  if (device.d3d12.drawIndexedIndirectSignature) {
    device.d3d12.drawIndexedIndirectSignature->Release();
    device.d3d12.drawIndexedIndirectSignature = nullptr;
  }
  if (device.d3d12.drawIndirectPaddedSignature) {
    device.d3d12.drawIndirectPaddedSignature->Release();
    device.d3d12.drawIndirectPaddedSignature = nullptr;
  }
  if (device.d3d12.drawIndirectSignature) {
    device.d3d12.drawIndirectSignature->Release();
    device.d3d12.drawIndirectSignature = nullptr;
  }
  if (device.d3d12.allocator) {
    device.d3d12.allocator->Release();
    device.d3d12.allocator = nullptr;
  }
  // device5 is a QueryInterface of `device`, so it holds its own reference and
  // must be released before the one the create call handed back.
  if (device.d3d12.device5) {
    device.d3d12.device5->Release();
    device.d3d12.device5 = nullptr;
  }
  if (device.d3d12.device)
    device.d3d12.device->Release();
  memset(&device.d3d12, 0, sizeof(device.d3d12));
}

void RID3D12_DrainDeviceMessages(struct RIDevice &device) {
  if (!device.d3d12.infoQueue || device.d3d12.infoQueue1)
    return;
  ri_d3d12_drain_info_queue(device.d3d12.infoQueue);
}

bool RID3D12_DeviceIsValid(const struct RIDevice &device) {
  return device.d3d12.device != nullptr;
}

bool RID3D12_QueryMemoryStats(const struct RIDevice &device,
                              struct RIMemoryStats *out) {
  if (!out || !device.d3d12.allocator)
    return false;
  // GetBudget is the cheap query: D3D12MA refreshes it from
  // QueryVideoMemoryInfo, so usage counts every process-wide resource, not only
  // the allocator's own.
  D3D12MA::Budget local = {};
  D3D12MA::Budget nonLocal = {};
  device.d3d12.allocator->GetBudget(&local, &nonLocal);
  out->localUsage = local.UsageBytes;
  out->localBudget = local.BudgetBytes;
  out->nonLocalUsage = nonLocal.UsageBytes;
  out->nonLocalBudget = nonLocal.BudgetBytes;
  out->allocatorBlockBytes = local.Stats.BlockBytes + nonLocal.Stats.BlockBytes;
  out->allocatorAllocationBytes =
      local.Stats.AllocationBytes + nonLocal.Stats.AllocationBytes;
  RID3D12_BufferRegistryStats(device, &out->registeredBuffers,
                              &out->retiredBufferBytes);
  return true;
}

static const char *ri_d3d12_removed_reason_name(HRESULT hr) {
  switch (hr) {
  case DXGI_ERROR_DEVICE_HUNG: return "DEVICE_HUNG";
  case DXGI_ERROR_DEVICE_REMOVED: return "DEVICE_REMOVED";
  case DXGI_ERROR_DEVICE_RESET: return "DEVICE_RESET";
  case DXGI_ERROR_DRIVER_INTERNAL_ERROR: return "DRIVER_INTERNAL_ERROR";
  case DXGI_ERROR_INVALID_CALL: return "INVALID_CALL";
  case E_OUTOFMEMORY: return "E_OUTOFMEMORY";
  default: return "unknown";
  }
}

static const char *ri_d3d12_breadcrumb_op_name(D3D12_AUTO_BREADCRUMB_OP op) {
  switch (op) {
  case D3D12_AUTO_BREADCRUMB_OP_SETMARKER: return "SetMarker";
  case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT: return "BeginEvent";
  case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT: return "EndEvent";
  case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED: return "DrawInstanced";
  case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED: return "DrawIndexedInstanced";
  case D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT: return "ExecuteIndirect";
  case D3D12_AUTO_BREADCRUMB_OP_DISPATCH: return "Dispatch";
  case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION: return "CopyBufferRegion";
  case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION: return "CopyTextureRegion";
  case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE: return "CopyResource";
  case D3D12_AUTO_BREADCRUMB_OP_COPYTILES: return "CopyTiles";
  case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCE: return "ResolveSubresource";
  case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW: return "ClearRenderTargetView";
  case D3D12_AUTO_BREADCRUMB_OP_CLEARUNORDEREDACCESSVIEW: return "ClearUnorderedAccessView";
  case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW: return "ClearDepthStencilView";
  case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER: return "ResourceBarrier";
  case D3D12_AUTO_BREADCRUMB_OP_EXECUTEBUNDLE: return "ExecuteBundle";
  case D3D12_AUTO_BREADCRUMB_OP_PRESENT: return "Present";
  case D3D12_AUTO_BREADCRUMB_OP_RESOLVEQUERYDATA: return "ResolveQueryData";
  case D3D12_AUTO_BREADCRUMB_OP_BEGINSUBMISSION: return "BeginSubmission";
  case D3D12_AUTO_BREADCRUMB_OP_ENDSUBMISSION: return "EndSubmission";
  case D3D12_AUTO_BREADCRUMB_OP_WRITEBUFFERIMMEDIATE: return "WriteBufferImmediate";
  case D3D12_AUTO_BREADCRUMB_OP_DISPATCHMESH: return "DispatchMesh";
  case D3D12_AUTO_BREADCRUMB_OP_BARRIER: return "Barrier";
  default: return nullptr;
  }
}

static void ri_d3d12_dump_dred(ID3D12Device *device) {
  ID3D12DeviceRemovedExtendedData1 *dred = nullptr;
  if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dred)))) {
    ri_d3d12_diag("RI D3D12 DRED: ID3D12DeviceRemovedExtendedData1 unavailable\n");
    return;
  }
  D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 crumbs = {};
  if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput1(&crumbs))) {
    for (const D3D12_AUTO_BREADCRUMB_NODE1 *node = crumbs.pHeadAutoBreadcrumbNode;
         node; node = node->pNext) {
      const uint32_t count = node->BreadcrumbCount;
      const uint32_t last = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
      // Fully completed lists are noise; the faulting list is the one whose
      // last completed op is short of its recorded op count.
      if (last >= count)
        continue;
      ri_d3d12_diag(
              "RI D3D12 DRED: incomplete command list '%s' on queue '%s' "
              "(completed %u of %u ops)\n",
              node->pCommandListDebugNameA ? node->pCommandListDebugNameA : "<unnamed>",
              node->pCommandQueueDebugNameA ? node->pCommandQueueDebugNameA : "<unnamed>",
              last, count);
      const uint32_t begin = last > 8 ? last - 8 : 0;
      const uint32_t end = std::min(count, last + 4);
      for (uint32_t i = begin; i < end; ++i) {
        const char *name = ri_d3d12_breadcrumb_op_name(node->pCommandHistory[i]);
        const wchar_t *context = nullptr;
        for (uint32_t c = 0; c < node->BreadcrumbContextsCount; ++c)
          if (node->pBreadcrumbContexts[c].BreadcrumbIndex == i)
            context = node->pBreadcrumbContexts[c].pContextString;
        char unknown[16];
        if (!name) {
          snprintf(unknown, sizeof(unknown), "op#%d", int(node->pCommandHistory[i]));
          name = unknown;
        }
        ri_d3d12_diag("RI D3D12 DRED:   %s [%u] %s%s%ls\n",
                i == last ? "=>" : "  ", i, name, context ? " " : "",
                context ? context : L"");
      }
    }
  }
  D3D12_DRED_PAGE_FAULT_OUTPUT1 fault = {};
  if (SUCCEEDED(dred->GetPageFaultAllocationOutput1(&fault)) && fault.PageFaultVA) {
    ri_d3d12_diag("RI D3D12 DRED: page fault at GPU VA 0x%016llx\n",
            static_cast<unsigned long long>(fault.PageFaultVA));
    for (const D3D12_DRED_ALLOCATION_NODE1 *node = fault.pHeadExistingAllocationNode;
         node; node = node->pNext)
      ri_d3d12_diag("RI D3D12 DRED:   existing allocation '%s' (type %d)\n",
              node->ObjectNameA ? node->ObjectNameA : "<unnamed>",
              int(node->AllocationType));
    for (const D3D12_DRED_ALLOCATION_NODE1 *node = fault.pHeadRecentFreedAllocationNode;
         node; node = node->pNext)
      ri_d3d12_diag("RI D3D12 DRED:   recently freed allocation '%s' (type %d)\n",
              node->ObjectNameA ? node->ObjectNameA : "<unnamed>",
              int(node->AllocationType));
  }
  dred->Release();
}

bool RID3D12_CheckDeviceRemoved(struct RIDevice &device, const char *where) {
  if (!device.d3d12.device)
    return false;
  const HRESULT reason = device.d3d12.device->GetDeviceRemovedReason();
  if (reason == S_OK)
    return false;
  if (device.d3d12.infoQueue)
    ri_d3d12_drain_info_queue(device.d3d12.infoQueue);
  ri_d3d12_diag("RI D3D12: device removed, detected at %s (reason 0x%08lX %s)\n",
                where ? where : "<unknown>", static_cast<unsigned long>(reason),
                ri_d3d12_removed_reason_name(reason));
  if (g_riD3D12DredEnabled)
    ri_d3d12_dump_dred(device.d3d12.device);
  else
    ri_d3d12_diag("RI D3D12: rerun with HPL_D3D12_DRED=1 for breadcrumbs and "
                  "page-fault data\n");
  // INVALID_CALL is the runtime rejecting an API call, usually a void one
  // (Create*View, CopyDescriptors*) that cannot return an error. Only the
  // debug layer names it; DRED breadcrumbs cover submitted GPU work only.
  if (!g_riD3D12EnableDebugLayer)
    ri_d3d12_diag("RI D3D12: rerun with HPL_D3D12_VALIDATION=1 (and "
                  "HPL_D3D12_BREAK_ON_ERROR=1 under a debugger) to identify "
                  "the offending API call\n");
  hpl::FatalError("RI D3D12: device removed (detected at %s, reason 0x%08lX %s)\n",
                  where ? where : "<unknown>", static_cast<unsigned long>(reason),
                  ri_d3d12_removed_reason_name(reason));
  return true;
}

#endif // DEVICE_IMPL_D3D12
