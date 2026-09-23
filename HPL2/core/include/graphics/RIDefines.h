#ifndef RI_DEFINES_H
#define RI_DEFINES_H

/* Vulkan is unconditionally enabled; D3D12 is opt-in through premake on Windows. */
#define DEVICE_SUPPORT_VULKAN

// Compile-time maximums for templated/array-sized backing storage.
#define RI_MAX_SWAPCHAIN_IMAGES 8

enum RIDeviceAPI_e {
  RI_DEVICE_API_UNKNOWN,
  RI_DEVICE_API_VK,
  RI_DEVICE_API_D3D11,
  RI_DEVICE_API_D3D12,
  RI_DEVICE_API_MTL
};

#ifdef DEVICE_SUPPORT_VULKAN
#define VK_NO_PROPERTIES
#define DEVICE_IMPL_VULKAN 1
#else
#define DEVICE_IMPL_VULKAN 0
#endif

#ifdef DEVICE_SUPPORT_MTL
#define DEVICE_IMPL_MTL 1
#else
#define DEVICE_IMPL_MTL 0
#endif

#ifdef DEVICE_SUPPORT_D3D11
#define DEVICE_IMPL_D3D11 1
#else
#define DEVICE_IMPL_D3D11 0
#endif

#ifdef DEVICE_SUPPORT_D3D12
#define DEVICE_IMPL_D3D12 1
#else
#define DEVICE_IMPL_D3D12 0
#endif

// True when more than one backend is compiled in. RIIsTargetSelected uses it to
// decide whether to runtime-dispatch; in single-backend builds it is 0, so that
// check reduces to an assert.
#define DEVICE_MULTI_BACKEND ( ( DEVICE_IMPL_D3D12 + DEVICE_IMPL_D3D11 + DEVICE_IMPL_MTL + DEVICE_IMPL_VULKAN ) > 1 )

#endif // RI_DEFINES_H
