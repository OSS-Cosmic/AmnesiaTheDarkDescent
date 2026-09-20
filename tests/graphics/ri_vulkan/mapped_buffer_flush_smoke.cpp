// Always-available Windows Vulkan smoke test for startup-style VMA mapped buffers.
// Keep this source Vulkan-only so it also compiles in the default Vulkan build.

#include "graphics/RIBuffer.h"
#include "graphics/RIDevice.h"
#include "graphics/RIRenderer.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

int hplMain(const std::string &) { return 0; }

namespace {

[[noreturn]] void Fail(const char *check) {
    std::fprintf(stderr, "RIVulkanMappedBufferFlushSmoke failed: %s\n", check);
    std::exit(EXIT_FAILURE);
}

void Require(bool condition, const char *check) {
    if (!condition)
        Fail(check);
}

void RequireLiveBuffer(RIDevice *device, RIBuffer &buffer, const char *check) {
    Require(!buffer.isEmpty(), check);
    Require(buffer.vk.buffer != VK_NULL_HANDLE, "Vulkan native buffer is live");
    Require(buffer.vk.allocation != nullptr, "Vulkan allocation is live");
    (void)device;
}

void RequireDisposed(RIDevice *device, RIBuffer &buffer, const char *check) {
    Require(buffer.isEmpty(), check);
    Require(buffer.mappedAddress == nullptr, "disposed buffer mapping is null");
    Require(buffer.cookie == 0, "disposed buffer cookie is zero");
    Require(buffer.GetDeviceHandle(device) == 0, "disposed buffer device address is zero");
}

void RunSmoke() {
    RIBackendInit backend = {};
    backend.api = RI_DEVICE_API_VK;
    backend.applicationName = "RIVulkanMappedBufferFlushSmoke";
    Require(InitRIRenderer(&backend) == RI_SUCCESS, "Vulkan renderer initialization");

    uint32_t adapterCount = 0;
    Require(EnumerateRIAdapters(nullptr, &adapterCount) == RI_SUCCESS && adapterCount >= 1 && adapterCount <= 8,
            "Vulkan adapter enumeration");

    RIPhysicalAdapter adapters[8] = {};
    uint32_t adapterCapacity = adapterCount;
    Require(EnumerateRIAdapters(adapters, &adapterCapacity) == RI_SUCCESS && adapterCapacity >= 1,
            "Vulkan adapter query");

    RIDevice device;
    RIDeviceDesc deviceDesc = {};
    deviceDesc.physicalAdapter = &adapters[0];
    Require(device.init(&deviceDesc) == RI_SUCCESS && RIDeviceIsValid(&device), "Vulkan device initialization");

    constexpr size_t bufferSize = 4096;
    RIBuffer upload = RIBuffer::create(
        &device, {bufferSize, RI_BUFFER_USAGE_TRANSFER_SRC, RI_MEMORY_HOST_UPLOAD, 0});
    RequireLiveBuffer(&device, upload, "host-upload buffer creation");
    Require(upload.mappedAddress != nullptr, "host-upload buffer mapping");
    std::memset(upload.mappedAddress, 0x5a, bufferSize);
    // size == 0 intentionally exercises the whole-range startup path to vmaFlushAllocation.
    upload.flushMappedRange(&device, 0, 0);
    upload.dispose(&device);
    RequireDisposed(&device, upload, "host-upload buffer disposal");

    RIBuffer readback = RIBuffer::create(
        &device, {bufferSize, RI_BUFFER_USAGE_TRANSFER_DST, RI_MEMORY_HOST_READBACK, 0});
    RequireLiveBuffer(&device, readback, "host-readback buffer creation");
    Require(readback.mappedAddress != nullptr, "host-readback buffer mapping");
    readback.invalidateMappedRange(&device, 0, 0);
    readback.dispose(&device);
    RequireDisposed(&device, readback, "host-readback buffer disposal");

    RIBuffer first = RIBuffer::create(
        &device, {bufferSize, RI_BUFFER_USAGE_TRANSFER_DST, RI_MEMORY_DEVICE, 0});
    RequireLiveBuffer(&device, first, "first device-local buffer creation");
    hash_t firstCookie = first.cookie;
    first.dispose(&device);
    RequireDisposed(&device, first, "first device-local buffer disposal");

    RIBuffer second = RIBuffer::create(
        &device, {bufferSize, RI_BUFFER_USAGE_TRANSFER_DST, RI_MEMORY_DEVICE, 0});
    RequireLiveBuffer(&device, second, "second device-local buffer creation");
    Require(second.cookie != firstCookie, "recreated buffers have distinct cookies");
    second.dispose(&device);
    RequireDisposed(&device, second, "second device-local buffer disposal");

    device.dispose();
    ShutdownRIRenderer();
}

} // namespace

int main() {
    RunSmoke();
    return 0;
}
