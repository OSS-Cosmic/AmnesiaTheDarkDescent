// Opt-in GPU smoke test for D3D12 swapchain frame-in-flight fences and recreate.
#include "graphics/RIDevice.h"
#include "graphics/RIRenderer.h"
#include "graphics/RID3D12.h"
#include "graphics/RICommand.h"
#include "graphics/RISwapchain.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>

int hplMain(const std::string &) { return 0; }

namespace {

[[noreturn]] void Fail(const char *check) {
  std::fprintf(stderr, "FAIL: %s\n", check);
  std::exit(1);
}

void Require(bool condition, const char *check) {
  if (!condition)
    Fail(check);
  std::printf("PASS: %s\n", check);
}

#if DEVICE_IMPL_D3D12
LRESULT CALLBACK SmokeWindowProc(HWND hwnd, UINT message, WPARAM wParam,
                                 LPARAM lParam) {
  return DefWindowProcW(hwnd, message, wParam, lParam);
}

void RunSmoke() {
  g_riD3D12EnableDebugLayer = true;
  RIBackendInit init = {};
  init.api = RI_DEVICE_API_D3D12;
  init.applicationName = "RID3D12SwapchainSmoke";
  Require(InitRIRenderer(&init) == RI_SUCCESS,
          "D3D12 InitRIRenderer succeeds");

  uint32_t numAdapters = 0;
  Require(EnumerateRIAdapters(nullptr, &numAdapters) == RI_SUCCESS &&
              numAdapters >= 1 && numAdapters <= 8,
          "D3D12 enumeration reports adapters");
  RIPhysicalAdapter adapters[8];
  uint32_t capacity = numAdapters;
  Require(EnumerateRIAdapters(adapters, &capacity) == RI_SUCCESS &&
              capacity >= 1 && capacity <= 8,
          "D3D12 enumeration populates adapters");

  uint32_t selected = 0;
  bool foundWarp = false;
  bool foundHardware = false;
  for (uint32_t i = 0; i < capacity; ++i) {
    if (adapters[i].d3d12.isWarp) {
      selected = i;
      foundWarp = true;
      break;
    }
    if (!foundHardware) {
      selected = i;
      foundHardware = true;
    }
  }
  Require(foundWarp || foundHardware,
          "D3D12 selects WARP or the first hardware adapter");

  RIDevice device;
  RIDeviceDesc deviceDesc = {};
  deviceDesc.physicalAdapter = &adapters[selected];
  Require(device.init(&deviceDesc) == RI_SUCCESS && RIDeviceIsValid(&device),
          "D3D12 device initializes and is valid");

  const wchar_t className[] = L"RID3D12SwapchainSmokeWindow";
  HINSTANCE instance = GetModuleHandleW(nullptr);
  WNDCLASSEXW windowClass = {};
  windowClass.cbSize = sizeof(windowClass);
  windowClass.lpfnWndProc = SmokeWindowProc;
  windowClass.hInstance = instance;
  windowClass.lpszClassName = className;
  Require(RegisterClassExW(&windowClass) != 0,
          "hidden HWND window class registers");
  HWND hwnd = CreateWindowExW(0, className, L"RID3D12SwapchainSmoke",
                              WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              320, 240, nullptr, nullptr, instance, nullptr);
  Require(hwnd != nullptr, "hidden HWND creates");

  RIWindowHandle window = {};
  window.type = RI_WINDOW_WIN32;
  window.windows.hwnd = hwnd;
  RISwapchainDesc swapchainDesc = {};
  swapchainDesc.format = RI_SWAPCHAIN_BT709_G22_8BIT;
  swapchainDesc.requestImageCount = 3;
  swapchainDesc.queue = &device.queues[RI_QUEUE_GRAPHICS];
  swapchainDesc.width = 320;
  swapchainDesc.height = 240;
  swapchainDesc.vsync = false;
  swapchainDesc.source = window;
  RISwapchain swapchain = RISwapchain::create(&device, swapchainDesc);
  Require(swapchain.imageCount > 0, "D3D12 swapchain creates images");
  Require(swapchain.imageCount == 3, "D3D12 swapchain has three images");
  for (uint32_t i = 0; i < swapchain.imageCount; ++i) {
    Require(!swapchain.textureView(i)->isEmpty(),
            "D3D12 swapchain creates a view for each image");
    Require(swapchain.textureView(i)->d3d12.resource ==
                swapchain.d3d12.images[i] &&
                swapchain.textureView(i)->d3d12.viewType ==
                    RI_VIEWTYPE_COLOR_ATTACHMENT,
            "D3D12 swapchain view targets its image as a color attachment");
  }

  RIPool pools[4];
  RICmd commands[4];
  uint32_t acquiredIndices[4] = {};
  uint64_t firstSlotZeroFence = 0;
  for (uint32_t cycle = 0; cycle < 4; ++cycle) {
    uint32_t index = 0;
    Require(RISwapchainAcquireNextTexture(&device, &swapchain, &index) ==
                RI_SWAPCHAIN_STATUS_OK,
            "D3D12 swapchain acquire succeeds");
    acquiredIndices[cycle] = index;

    pools[cycle].init(&device, &device.queues[RI_QUEUE_GRAPHICS]);
    commands[cycle].init(&device, &pools[cycle]);
    commands[cycle].begin(&device);
    commands[cycle].end(&device);
    RICmd *commandList[] = {&commands[cycle]};
    RISubmitDesc submit = {};
    submit.cmds = commandList;
    submit.cmdCount = 1;
    Require(device.queues[RI_QUEUE_GRAPHICS].submit(&device, submit) ==
                RI_SUCCESS,
            "D3D12 no-op graphics submit succeeds");

    Require(RISwapchainPresent(&device, &swapchain) ==
                RI_SWAPCHAIN_STATUS_OK,
            "D3D12 swapchain present succeeds");
    Require(swapchain.d3d12.frameFenceValues[index] != 0,
            "presented image receives a frame fence value");
    Require(swapchain.d3d12.frameFenceValues[index] ==
                swapchain.presentQueue->d3d12.nextFenceValue,
            "frame fence value matches the queue monotonic signal");
    if (cycle == 0 && index == 0)
      firstSlotZeroFence = swapchain.d3d12.frameFenceValues[0];
  }

  Require(acquiredIndices[0] != acquiredIndices[1] &&
              acquiredIndices[0] != acquiredIndices[2] &&
              acquiredIndices[1] != acquiredIndices[2] &&
              acquiredIndices[3] == acquiredIndices[0] &&
              acquiredIndices[0] == 0,
          "four cycles reuse swapchain image zero after three distinct images");
  Require(firstSlotZeroFence != 0 &&
              swapchain.d3d12.frameFenceValues[0] != firstSlotZeroFence,
          "reused image zero receives a new frame fence value");

  device.queues[RI_QUEUE_GRAPHICS].waitIdle(&device);
  RISwapchainDesc resizeDesc = swapchainDesc;
  resizeDesc.width = 640;
  resizeDesc.height = 480;
  resizeDesc.source = &swapchain;
  RISwapchain resized = RISwapchain::create(&device, resizeDesc);
  Require(resized.imageCount == 3, "resized swapchain has three images");
  Require(resized.width == 640 && resized.height == 480,
          "resized swapchain reports new dimensions");
  for (uint32_t i = 0; i < resized.imageCount; ++i) {
    Require(!resized.textureView(i)->isEmpty() &&
                resized.textureView(i)->d3d12.resource ==
                    resized.d3d12.images[i] &&
                resized.textureView(i)->d3d12.viewType ==
                    RI_VIEWTYPE_COLOR_ATTACHMENT,
            "resized swapchain recreates its color attachment views");
  }
  Require(swapchain.d3d12.swapchain == nullptr,
          "retired swapchain released its swapchain COM ref");
  Require(swapchain.d3d12.images[0] == nullptr,
          "retired swapchain released its backbuffer COM refs");

  swapchain.dispose(&device);
  RIPool resizePool;
  RICmd resizeCmd;
  uint32_t resizeIndex = 0;
  Require(RISwapchainAcquireNextTexture(&device, &resized, &resizeIndex) ==
              RI_SWAPCHAIN_STATUS_OK,
          "resized swapchain acquire succeeds");
  resizePool.init(&device, &device.queues[RI_QUEUE_GRAPHICS]);
  resizeCmd.init(&device, &resizePool);
  resizeCmd.begin(&device);
  resizeCmd.end(&device);
  RICmd *resizeCommandList[] = {&resizeCmd};
  RISubmitDesc resizeSubmit = {};
  resizeSubmit.cmds = resizeCommandList;
  resizeSubmit.cmdCount = 1;
  Require(device.queues[RI_QUEUE_GRAPHICS].submit(&device, resizeSubmit) ==
              RI_SUCCESS,
          "D3D12 resized no-op graphics submit succeeds");
  Require(RISwapchainPresent(&device, &resized) == RI_SWAPCHAIN_STATUS_OK,
          "D3D12 resized swapchain present succeeds");
  resized.dispose(&device);
  resizeCmd.dispose(&device);
  resizePool.dispose(&device);

  for (uint32_t cycle = 0; cycle < 4; ++cycle) {
    commands[cycle].dispose(&device);
    pools[cycle].dispose(&device);
  }
  DestroyWindow(hwnd);
  UnregisterClassW(className, instance);
  device.dispose();
  ShutdownRIRenderer();
  g_riD3D12EnableDebugLayer = false;
}
#endif

} // namespace

int main(int, char **) {
#if DEVICE_IMPL_D3D12
  RunSmoke();
  return 0;
#else
  std::printf("SKIP: DX12 not compiled in\n");
  return 0;
#endif
}
