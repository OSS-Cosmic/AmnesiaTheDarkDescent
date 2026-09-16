#include "volk.h"
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include "utest.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <vector>

uint32_t findMemoryTypeIndex(VkPhysicalDevice physicalDevice, VkMemoryRequirements memRequirements,
                             VkMemoryPropertyFlags requestedProperties,
                             VkMemoryPropertyFlags& outProperties);

namespace {

struct FakePhysicalDeviceToken {};
struct FakeVulkanState;

struct FakeDeviceToken {
    FakeVulkanState* state = nullptr;
};

struct FakeEnumerationState {
    VkPhysicalDevice lastPhysicalDevice = VK_NULL_HANDLE;
    const char* lastLayerName = nullptr;
    uint32_t callCount = 0;
    uint32_t extensionCount = 0;
    uint32_t nonNullPropertiesCallCount = 0;
    uint32_t populatedEntryCount = 0;
};

FakeEnumerationState g_fakeEnumeration;

struct FakePropertiesState {
    VkPhysicalDeviceProperties properties = {};
    VkPhysicalDevice lastPhysicalDevice = VK_NULL_HANDLE;
    uint32_t callCount = 0;
};

FakePropertiesState g_fakeProperties;

struct FakeMemoryState {
    VkPhysicalDeviceMemoryProperties properties = {};
    VkPhysicalDevice lastPhysicalDevice = VK_NULL_HANDLE;
    uint32_t callCount = 0;
};

FakeMemoryState g_fakeMemory;

struct FakeBufferToken {
    bool alive = false;
    VkDeviceSize size = 0;
    VkDeviceMemory boundMemory = VK_NULL_HANDLE;
};

struct FakeMemoryToken {
    bool alive = false;
    bool mapped = false;
    std::vector<uint8_t> storage;
};

struct FakeDescriptorPoolToken {
    bool alive = false;
};

struct FakeVulkanState {
    std::array<FakeBufferToken, 16> buffers = {};
    std::array<FakeMemoryToken, 8> memories = {};
    std::array<FakeDescriptorPoolToken, 8> descriptorPools = {};

    uint32_t deviceBufferCreateCalls = 0;
    uint32_t directBufferCreateCalls = 0;
    uint32_t bufferDestroyCalls = 0;
    uint32_t descriptorPoolCreateCalls = 0;
    uint32_t descriptorPoolDestroyCalls = 0;
    uint32_t allocateMemoryCalls = 0;
    uint32_t freeMemoryCalls = 0;
    uint32_t mapMemoryCalls = 0;
    uint32_t unmapMemoryCalls = 0;
    uint32_t bindBufferMemoryCalls = 0;
    uint32_t deviceProcAddrCalls = 0;
    bool callbackError = false;

    void reset() {
        *this = {};
    }
};

FakeVulkanState g_fakeVulkan;

FakeVulkanState* fake_state(VkDevice device) {
    if (device == VK_NULL_HANDLE)
        return nullptr;
    return reinterpret_cast<FakeDeviceToken*>(device)->state;
}

FakeBufferToken* allocate_fake_buffer(FakeVulkanState* state) {
    if (!state)
        return nullptr;
    for (FakeBufferToken& buffer : state->buffers) {
        if (!buffer.alive)
            return &buffer;
    }
    state->callbackError = true;
    return nullptr;
}

FakeMemoryToken* allocate_fake_memory(FakeVulkanState* state) {
    if (!state)
        return nullptr;
    for (FakeMemoryToken& memory : state->memories) {
        if (!memory.alive)
            return &memory;
    }
    state->callbackError = true;
    return nullptr;
}

FakeDescriptorPoolToken* allocate_fake_descriptor_pool(FakeVulkanState* state) {
    if (!state)
        return nullptr;
    for (FakeDescriptorPoolToken& descriptorPool : state->descriptorPools) {
        if (!descriptorPool.alive)
            return &descriptorPool;
    }
    state->callbackError = true;
    return nullptr;
}

VKAPI_ATTR VkResult VKAPI_CALL fake_vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice physicalDevice,
    const char* pLayerName,
    uint32_t* pPropertyCount,
    VkExtensionProperties* pProperties) {
    ++g_fakeEnumeration.callCount;
    g_fakeEnumeration.lastPhysicalDevice = physicalDevice;
    g_fakeEnumeration.lastLayerName = pLayerName;
    if (pPropertyCount) {
        const uint32_t requestedCount = *pPropertyCount;
        *pPropertyCount = g_fakeEnumeration.extensionCount;
        if (pProperties) {
            ++g_fakeEnumeration.nonNullPropertiesCallCount;
            const uint32_t countToPopulate = requestedCount < g_fakeEnumeration.extensionCount
                ? requestedCount
                : g_fakeEnumeration.extensionCount;
            for (uint32_t i = 0; i < countToPopulate; ++i) {
                pProperties[i] = {};
                std::snprintf(pProperties[i].extensionName, VK_MAX_EXTENSION_NAME_SIZE,
                              "FAKE_inert_extension_%u", i);
                pProperties[i].specVersion = 1;
                ++g_fakeEnumeration.populatedEntryCount;
            }
        }
    }
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL fake_vkGetPhysicalDeviceMemoryProperties(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties* pMemoryProperties) {
    ++g_fakeMemory.callCount;
    g_fakeMemory.lastPhysicalDevice = physicalDevice;
    if (pMemoryProperties)
        *pMemoryProperties = g_fakeMemory.properties;
}

VKAPI_ATTR void VKAPI_CALL fake_vkGetPhysicalDeviceProperties(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties* pProperties) {
    ++g_fakeProperties.callCount;
    g_fakeProperties.lastPhysicalDevice = physicalDevice;
    if (pProperties)
        *pProperties = g_fakeProperties.properties;
}

VKAPI_ATTR VkResult VKAPI_CALL fake_vkCreateDescriptorPool(
    VkDevice device, const VkDescriptorPoolCreateInfo*, const VkAllocationCallbacks*,
    VkDescriptorPool* pDescriptorPool) {
    FakeVulkanState* state = fake_state(device);
    FakeDescriptorPoolToken* descriptorPool = allocate_fake_descriptor_pool(state);
    if (!descriptorPool || !pDescriptorPool)
        return VK_ERROR_INITIALIZATION_FAILED;

    descriptorPool->alive = true;
    ++state->descriptorPoolCreateCalls;
    *pDescriptorPool = reinterpret_cast<VkDescriptorPool>(descriptorPool);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL fake_vkDestroyDescriptorPool(
    VkDevice device, VkDescriptorPool descriptorPool, const VkAllocationCallbacks*) {
    FakeVulkanState* state = fake_state(device);
    if (!state || descriptorPool == VK_NULL_HANDLE)
        return;

    FakeDescriptorPoolToken* token = reinterpret_cast<FakeDescriptorPoolToken*>(descriptorPool);
    if (!token->alive) {
        state->callbackError = true;
        return;
    }
    token->alive = false;
    ++state->descriptorPoolDestroyCalls;
}

VkResult fake_vkCreateBufferCommon(VkDevice device, const VkBufferCreateInfo* pCreateInfo,
                                   VkBuffer* pBuffer, bool directCall) {
    FakeVulkanState* state = fake_state(device);
    FakeBufferToken* buffer = allocate_fake_buffer(state);
    if (!buffer || !pCreateInfo || !pBuffer)
        return VK_ERROR_INITIALIZATION_FAILED;

    buffer->alive = true;
    buffer->size = pCreateInfo->size;
    buffer->boundMemory = VK_NULL_HANDLE;
    if (directCall)
        ++state->directBufferCreateCalls;
    else
        ++state->deviceBufferCreateCalls;
    *pBuffer = reinterpret_cast<VkBuffer>(buffer);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL fake_vkCreateBufferDevice(
    VkDevice device, const VkBufferCreateInfo* pCreateInfo, const VkAllocationCallbacks*,
    VkBuffer* pBuffer) {
    return fake_vkCreateBufferCommon(device, pCreateInfo, pBuffer, false);
}

VKAPI_ATTR VkResult VKAPI_CALL fake_vkCreateBufferDirect(
    VkDevice device, const VkBufferCreateInfo* pCreateInfo, const VkAllocationCallbacks*,
    VkBuffer* pBuffer) {
    return fake_vkCreateBufferCommon(device, pCreateInfo, pBuffer, true);
}

VKAPI_ATTR void VKAPI_CALL fake_vkGetBufferMemoryRequirements(
    VkDevice device, VkBuffer buffer, VkMemoryRequirements* pMemoryRequirements) {
    FakeVulkanState* state = fake_state(device);
    if (!state || buffer == VK_NULL_HANDLE || !pMemoryRequirements) {
        if (state)
            state->callbackError = true;
        return;
    }

    FakeBufferToken* token = reinterpret_cast<FakeBufferToken*>(buffer);
    if (!token->alive) {
        state->callbackError = true;
        return;
    }
    pMemoryRequirements->size = token->size < 256 ? 256 : token->size;
    pMemoryRequirements->alignment = 256;
    pMemoryRequirements->memoryTypeBits = 0x3u;
}

VKAPI_ATTR VkResult VKAPI_CALL fake_vkAllocateMemory(
    VkDevice device, const VkMemoryAllocateInfo* pAllocateInfo, const VkAllocationCallbacks*,
    VkDeviceMemory* pMemory) {
    FakeVulkanState* state = fake_state(device);
    FakeMemoryToken* memory = allocate_fake_memory(state);
    if (!memory || !pAllocateInfo || !pMemory)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (pAllocateInfo->allocationSize > std::numeric_limits<size_t>::max())
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    try {
        memory->storage.resize(static_cast<size_t>(pAllocateInfo->allocationSize));
    } catch (...) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    memory->alive = true;
    memory->mapped = false;
    ++state->allocateMemoryCalls;
    *pMemory = reinterpret_cast<VkDeviceMemory>(memory);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL fake_vkMapMemory(
    VkDevice device, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size,
    VkMemoryMapFlags, void** ppData) {
    FakeVulkanState* state = fake_state(device);
    if (!state || memory == VK_NULL_HANDLE || !ppData) {
        if (state)
            state->callbackError = true;
        return VK_ERROR_MEMORY_MAP_FAILED;
    }

    FakeMemoryToken* token = reinterpret_cast<FakeMemoryToken*>(memory);
    const VkDeviceSize storageSize = static_cast<VkDeviceSize>(token->storage.size());
    const VkDeviceSize mappedSize = size == VK_WHOLE_SIZE ? storageSize - offset : size;
    if (!token->alive || token->mapped || offset > storageSize || mappedSize > storageSize - offset) {
        state->callbackError = true;
        return VK_ERROR_MEMORY_MAP_FAILED;
    }
    token->mapped = true;
    ++state->mapMemoryCalls;
    *ppData = token->storage.data() + static_cast<size_t>(offset);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL fake_vkUnmapMemory(VkDevice device, VkDeviceMemory memory) {
    FakeVulkanState* state = fake_state(device);
    if (!state || memory == VK_NULL_HANDLE)
        return;

    FakeMemoryToken* token = reinterpret_cast<FakeMemoryToken*>(memory);
    if (!token->alive || !token->mapped) {
        state->callbackError = true;
        return;
    }
    token->mapped = false;
    ++state->unmapMemoryCalls;
}

VKAPI_ATTR VkResult VKAPI_CALL fake_vkBindBufferMemory(
    VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize) {
    FakeVulkanState* state = fake_state(device);
    if (!state || buffer == VK_NULL_HANDLE || memory == VK_NULL_HANDLE)
        return VK_ERROR_INITIALIZATION_FAILED;

    FakeBufferToken* bufferToken = reinterpret_cast<FakeBufferToken*>(buffer);
    FakeMemoryToken* memoryToken = reinterpret_cast<FakeMemoryToken*>(memory);
    if (!bufferToken->alive || !memoryToken->alive) {
        state->callbackError = true;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    bufferToken->boundMemory = memory;
    ++state->bindBufferMemoryCalls;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL fake_vkDestroyBuffer(
    VkDevice device, VkBuffer buffer, const VkAllocationCallbacks*) {
    FakeVulkanState* state = fake_state(device);
    if (!state || buffer == VK_NULL_HANDLE)
        return;

    FakeBufferToken* token = reinterpret_cast<FakeBufferToken*>(buffer);
    if (!token->alive) {
        state->callbackError = true;
        return;
    }
    token->alive = false;
    ++state->bufferDestroyCalls;
}

VKAPI_ATTR void VKAPI_CALL fake_vkFreeMemory(
    VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks*) {
    FakeVulkanState* state = fake_state(device);
    if (!state || memory == VK_NULL_HANDLE)
        return;

    FakeMemoryToken* token = reinterpret_cast<FakeMemoryToken*>(memory);
    if (!token->alive || token->mapped) {
        state->callbackError = true;
        return;
    }
    token->alive = false;
    token->storage.clear();
    ++state->freeMemoryCalls;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL fake_vkGetDeviceProcAddr(
    VkDevice device, const char* pName) {
    FakeVulkanState* state = fake_state(device);
    if (state)
        ++state->deviceProcAddrCalls;
    if (!pName)
        return nullptr;

    if (std::strcmp(pName, "vkCreateDescriptorPool") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(fake_vkCreateDescriptorPool);
    if (std::strcmp(pName, "vkCreateBuffer") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(fake_vkCreateBufferDevice);
    if (std::strcmp(pName, "vkGetBufferMemoryRequirements") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(fake_vkGetBufferMemoryRequirements);
    if (std::strcmp(pName, "vkAllocateMemory") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(fake_vkAllocateMemory);
    if (std::strcmp(pName, "vkMapMemory") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(fake_vkMapMemory);
    if (std::strcmp(pName, "vkBindBufferMemory") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(fake_vkBindBufferMemory);
    if (std::strcmp(pName, "vkDestroyBuffer") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(fake_vkDestroyBuffer);
    if (std::strcmp(pName, "vkUnmapMemory") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(fake_vkUnmapMemory);
    if (std::strcmp(pName, "vkFreeMemory") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(fake_vkFreeMemory);
    if (std::strcmp(pName, "vkDestroyDescriptorPool") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(fake_vkDestroyDescriptorPool);
    return nullptr;
}

void set_fake_memory_properties(std::initializer_list<VkMemoryPropertyFlags> propertyFlags) {
    g_fakeMemory = {};
    g_fakeMemory.properties.memoryTypeCount = static_cast<uint32_t>(propertyFlags.size());
    g_fakeMemory.properties.memoryHeapCount = propertyFlags.size() == 0 ? 0 : 1;
    if (propertyFlags.size() != 0)
        g_fakeMemory.properties.memoryHeaps[0].size = 1;

    uint32_t memoryTypeIndex = 0;
    for (const VkMemoryPropertyFlags flags : propertyFlags) {
        g_fakeMemory.properties.memoryTypes[memoryTypeIndex].heapIndex = 0;
        g_fakeMemory.properties.memoryTypes[memoryTypeIndex].propertyFlags = flags;
        ++memoryTypeIndex;
    }
}

void reset_fake(uint32_t extensionCount) {
    g_fakeEnumeration = {};
    g_fakeEnumeration.extensionCount = extensionCount;
}

void expect_lifecycle(int* utest_result, const FakeVulkanState& state,
                      uint32_t expectedInitializations) {
    EXPECT_FALSE_MSG(state.callbackError, "fake Vulkan callback lifecycle");
    EXPECT_EQ_MSG(state.descriptorPoolCreateCalls, expectedInitializations,
                  "descriptor-pool creates");
    EXPECT_EQ_MSG(state.descriptorPoolDestroyCalls, expectedInitializations,
                  "descriptor-pool destroys");
    EXPECT_EQ_MSG(state.deviceBufferCreateCalls, expectedInitializations,
                  "device buffer creates");
    EXPECT_EQ_MSG(state.directBufferCreateCalls, expectedInitializations,
                  "direct buffer creates");
    EXPECT_EQ_MSG(state.bufferDestroyCalls, expectedInitializations * 2,
                  "buffer destroys");
    EXPECT_EQ_MSG(state.allocateMemoryCalls, expectedInitializations,
                  "memory allocations");
    EXPECT_EQ_MSG(state.freeMemoryCalls, expectedInitializations,
                  "memory frees");
    EXPECT_EQ_MSG(state.mapMemoryCalls, expectedInitializations,
                  "memory maps");
    EXPECT_EQ_MSG(state.unmapMemoryCalls, expectedInitializations,
                  "memory unmaps");
    EXPECT_EQ_MSG(state.bindBufferMemoryCalls, expectedInitializations,
                  "memory binds");
}

void expect_guards(int* utest_result, const char* label, const uint8_t* scratch,
                   size_t scratchSize) {
    constexpr size_t kGuardBytes = 64;
    constexpr uint8_t kGuardByte = 0xa5;

    for (size_t i = 0; i < kGuardBytes; ++i) {
        EXPECT_EQ_MSG(scratch[-static_cast<ptrdiff_t>(kGuardBytes) +
                             static_cast<ptrdiff_t>(i)],
                      kGuardByte, label);
        EXPECT_EQ_MSG(scratch[scratchSize + i], kGuardByte, label);
    }
}

struct BackendContextsGuard {
    FfxInterface* backend = nullptr;
    int* utest_result = nullptr;
    FfxUInt32 ids[2] = {UINT32_MAX, UINT32_MAX};
    bool active[2] = {false, false};

    ~BackendContextsGuard() {
        if (!backend || !backend->fpDestroyBackendContext)
            return;
        for (size_t i = 0; i < 2; ++i) {
            if (active[i]) {
                EXPECT_EQ_MSG(backend->fpDestroyBackendContext(backend, ids[i]),
                              FFX_OK, "early-return context destruction");
                active[i] = false;
            }
        }
    }
};

void run_backend_context_case(int* utest_result, const char* label,
                              VkPhysicalDevice physicalDevice,
                              VkDeviceContext* deviceContext, uint32_t extensionCount,
                              size_t maxContexts, uintptr_t requestedBaseAddressMod32) {
    constexpr size_t kGuardBytes = 64;
    constexpr uint8_t kGuardByte = 0xa5;

    reset_fake(extensionCount);
    const size_t scratchSize = ffxGetScratchMemorySizeVK(physicalDevice, maxContexts);
    ASSERT_GT_MSG(scratchSize, size_t(0), label);
    ASSERT_LE_MSG(scratchSize, std::numeric_limits<size_t>::max() -
                                   (kGuardBytes * 2) - 32,
                  "guarded scratch storage size cannot overflow");
    EXPECT_EQ_MSG(g_fakeEnumeration.callCount, 1u, "scratch-size enumeration count");
    EXPECT_EQ_MSG(g_fakeEnumeration.lastPhysicalDevice, physicalDevice,
                  "scratch-size physical device");
    EXPECT_EQ_MSG(g_fakeEnumeration.lastLayerName, nullptr,
                  "scratch-size layer name");

    std::vector<uint8_t> rawStorage(scratchSize + (kGuardBytes * 2) + 32, kGuardByte);
    const uintptr_t rawAddress = reinterpret_cast<uintptr_t>(rawStorage.data());
    ASSERT_EQ_MSG(rawAddress % alignof(std::max_align_t), uintptr_t(0),
                  "raw storage alignment");
    const uintptr_t guardAddress = rawAddress + kGuardBytes;
    const size_t baseOffset = kGuardBytes + static_cast<size_t>(
        (requestedBaseAddressMod32 + 32 - (guardAddress % 32)) % 32);
    uint8_t* scratch = rawStorage.data() + baseOffset;
    const uintptr_t scratchAddress = reinterpret_cast<uintptr_t>(scratch);
    ASSERT_EQ_MSG(scratchAddress % alignof(std::max_align_t), uintptr_t(0),
                  "scratch alignment");
    EXPECT_EQ_MSG(scratchAddress % 32, requestedBaseAddressMod32,
                  "scratch base address modulo 32");
    std::memset(scratch, 0, scratchSize);

    set_fake_memory_properties({VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT});
    reset_fake(extensionCount);

    const FfxDevice device = ffxGetDeviceVK(deviceContext);
    ASSERT_TRUE_MSG(device != nullptr, "ffxGetDeviceVK result");
    FfxInterface backend = {};
    ASSERT_EQ_MSG(ffxGetInterfaceVK(&backend, device, scratch, scratchSize, maxContexts),
                  FFX_OK, "ffxGetInterfaceVK result");
    EXPECT_EQ_MSG(g_fakeEnumeration.callCount, 1u,
                  "interface enumeration count");
    EXPECT_EQ_MSG(g_fakeEnumeration.lastPhysicalDevice, physicalDevice,
                  "interface physical device");
    EXPECT_EQ_MSG(g_fakeEnumeration.lastLayerName, nullptr,
                  "interface layer name");

    ASSERT_TRUE_MSG(backend.fpCreateBackendContext != nullptr, "create callback before use");
    ASSERT_TRUE_MSG(backend.fpDestroyBackendContext != nullptr, "destroy callback before use");
    // Declared after scratch storage so active contexts are destroyed first.
    BackendContextsGuard contexts = {&backend, utest_result};
    reset_fake(extensionCount);
    const FfxErrorCode firstResult = backend.fpCreateBackendContext(
        &backend, FFX_EFFECT_FSR3UPSCALER, nullptr, &contexts.ids[0]);
    contexts.active[0] = firstResult == FFX_OK;
    ASSERT_EQ_MSG(firstResult, FFX_OK, "first context create");
    EXPECT_EQ_MSG(contexts.ids[0], 0u, "first context id");
    EXPECT_EQ_MSG(g_fakeEnumeration.callCount, 2u, "create enumeration count");
    EXPECT_EQ_MSG(g_fakeEnumeration.lastPhysicalDevice, physicalDevice,
                  "create physical device");
    EXPECT_EQ_MSG(g_fakeEnumeration.lastLayerName, nullptr, "create layer name");
    EXPECT_EQ_MSG(g_fakeEnumeration.nonNullPropertiesCallCount, 1u,
                  "non-null enumeration call count");
    EXPECT_EQ_MSG(g_fakeEnumeration.populatedEntryCount, extensionCount,
                  "enumerated extension entries");
    expect_guards(utest_result, "guard after first create", scratch, scratchSize);

    if (maxContexts == 2) {
        const FfxErrorCode secondResult = backend.fpCreateBackendContext(
            &backend, FFX_EFFECT_FSR3UPSCALER, nullptr, &contexts.ids[1]);
        contexts.active[1] = secondResult == FFX_OK;
        ASSERT_EQ_MSG(secondResult, FFX_OK, "second context create");
        EXPECT_NE_MSG(contexts.ids[1], contexts.ids[0], "active context ids distinct");
        const FfxErrorCode destroyFirstResult = backend.fpDestroyBackendContext(
            &backend, contexts.ids[0]);
        contexts.active[0] = destroyFirstResult != FFX_OK;
        ASSERT_EQ_MSG(destroyFirstResult, FFX_OK, "destroy before reuse");
        const FfxErrorCode reuseResult = backend.fpCreateBackendContext(
            &backend, FFX_EFFECT_FSR3UPSCALER, nullptr, &contexts.ids[0]);
        contexts.active[0] = reuseResult == FFX_OK;
        ASSERT_EQ_MSG(reuseResult, FFX_OK, "reused context create");
        EXPECT_EQ_MSG(contexts.ids[0], 0u, "reused context id");
        const FfxErrorCode destroyReuseResult = backend.fpDestroyBackendContext(
            &backend, contexts.ids[0]);
        contexts.active[0] = destroyReuseResult != FFX_OK;
        ASSERT_EQ_MSG(destroyReuseResult, FFX_OK, "destroy reused context");
        const FfxErrorCode destroySecondResult = backend.fpDestroyBackendContext(
            &backend, contexts.ids[1]);
        contexts.active[1] = destroySecondResult != FFX_OK;
        ASSERT_EQ_MSG(destroySecondResult, FFX_OK, "destroy second context");
    } else {
        const FfxErrorCode destroyFirstResult = backend.fpDestroyBackendContext(
            &backend, contexts.ids[0]);
        contexts.active[0] = destroyFirstResult != FFX_OK;
        ASSERT_EQ_MSG(destroyFirstResult, FFX_OK, "destroy first context");
        const FfxErrorCode reuseResult = backend.fpCreateBackendContext(
            &backend, FFX_EFFECT_FSR3UPSCALER, nullptr, &contexts.ids[0]);
        contexts.active[0] = reuseResult == FFX_OK;
        ASSERT_EQ_MSG(reuseResult, FFX_OK, "reused context create");
        EXPECT_EQ_MSG(contexts.ids[0], 0u, "reused context id");
        const FfxErrorCode destroyReuseResult = backend.fpDestroyBackendContext(
            &backend, contexts.ids[0]);
        contexts.active[0] = destroyReuseResult != FFX_OK;
        ASSERT_EQ_MSG(destroyReuseResult, FFX_OK, "destroy reused context");
    }

    expect_guards(utest_result, "guard after destroys", scratch, scratchSize);
    const uint32_t expectedInitializations = maxContexts == 1 ? 2 : 1;
    expect_lifecycle(utest_result, g_fakeVulkan, expectedInitializations);
    EXPECT_EQ_MSG(g_fakeProperties.callCount, expectedInitializations,
                  "physical-properties query count");
    EXPECT_EQ_MSG(g_fakeProperties.lastPhysicalDevice, physicalDevice,
                  "physical-properties device");
    EXPECT_EQ_MSG(g_fakeMemory.callCount, expectedInitializations * 2,
                  "memory-properties query count");
    EXPECT_EQ_MSG(g_fakeMemory.lastPhysicalDevice, physicalDevice,
                  "memory-properties device");
}

} // namespace


struct VulkanLoaderFixture {
    PFN_vkEnumerateDeviceExtensionProperties enumeration;
    PFN_vkGetPhysicalDeviceMemoryProperties memoryProperties;
    PFN_vkGetPhysicalDeviceProperties properties;
    PFN_vkCreateBuffer createBuffer;
    FakePhysicalDeviceToken physicalToken;
    FakeDeviceToken deviceToken;
    VkPhysicalDevice physicalDevice;
    VkDeviceContext deviceContext;
};

UTEST_F_SETUP(VulkanLoaderFixture) {
    utest_fixture->enumeration = vkEnumerateDeviceExtensionProperties;
    utest_fixture->memoryProperties = vkGetPhysicalDeviceMemoryProperties;
    utest_fixture->properties = vkGetPhysicalDeviceProperties;
    utest_fixture->createBuffer = vkCreateBuffer;
    utest_fixture->physicalDevice = reinterpret_cast<VkPhysicalDevice>(&utest_fixture->physicalToken);
    utest_fixture->deviceToken.state = &g_fakeVulkan;
    utest_fixture->deviceContext.vkDevice = reinterpret_cast<VkDevice>(&utest_fixture->deviceToken);
    utest_fixture->deviceContext.vkPhysicalDevice = utest_fixture->physicalDevice;
    utest_fixture->deviceContext.vkDeviceProcAddr = fake_vkGetDeviceProcAddr;
    g_fakeEnumeration = {};
    g_fakeProperties = {};
    g_fakeMemory = {};
    g_fakeVulkan.reset();
    vkEnumerateDeviceExtensionProperties = fake_vkEnumerateDeviceExtensionProperties;
    vkGetPhysicalDeviceMemoryProperties = fake_vkGetPhysicalDeviceMemoryProperties;
    vkGetPhysicalDeviceProperties = fake_vkGetPhysicalDeviceProperties;
    vkCreateBuffer = fake_vkCreateBufferDirect;
    g_fakeProperties.properties.limits.minUniformBufferOffsetAlignment = 256;
}

UTEST_F_TEARDOWN(VulkanLoaderFixture) {
    EXPECT_FALSE_MSG(g_fakeVulkan.callbackError, "callbackError before reset");
    for (const auto& b : g_fakeVulkan.buffers)
        EXPECT_FALSE_MSG(b.alive, "outstanding buffer before reset");
    for (const auto& m : g_fakeVulkan.memories) {
        EXPECT_FALSE_MSG(m.alive, "outstanding memory before reset");
        EXPECT_FALSE_MSG(m.mapped, "mapped memory before reset");
    }
    for (const auto& p : g_fakeVulkan.descriptorPools)
        EXPECT_FALSE_MSG(p.alive, "outstanding descriptor pool before reset");
    g_fakeVulkan.reset();
    g_fakeEnumeration = {};
    g_fakeProperties = {};
    g_fakeMemory = {};
    vkEnumerateDeviceExtensionProperties = utest_fixture->enumeration;
    vkGetPhysicalDeviceMemoryProperties = utest_fixture->memoryProperties;
    vkGetPhysicalDeviceProperties = utest_fixture->properties;
    vkCreateBuffer = utest_fixture->createBuffer;
}

UTEST_F(VulkanLoaderFixture, ScratchExtensionEnumeration) {
    reset_fake(0);
    const size_t ext0 = ffxGetScratchMemorySizeVK(utest_fixture->physicalDevice, 1);
    EXPECT_EQ_MSG(g_fakeEnumeration.callCount, 1u, "ext0 call count");
    EXPECT_EQ_MSG(g_fakeEnumeration.lastPhysicalDevice, utest_fixture->physicalDevice, "ext0 physical device");
    EXPECT_EQ_MSG(g_fakeEnumeration.lastLayerName, nullptr, "ext0 layer name");
    EXPECT_GT_MSG(ext0, size_t(0), "ext0 scratch size");
    reset_fake(7);
    const size_t ext7 = ffxGetScratchMemorySizeVK(utest_fixture->physicalDevice, 1);
    EXPECT_EQ_MSG(g_fakeEnumeration.callCount, 1u, "ext7 call count");
    EXPECT_EQ_MSG(g_fakeEnumeration.lastPhysicalDevice, utest_fixture->physicalDevice,
                  "ext7 physical device");
    EXPECT_EQ_MSG(g_fakeEnumeration.lastLayerName, nullptr, "ext7 layer name");
    EXPECT_GT_MSG(ext7, size_t(0), "ext7 scratch size");
    EXPECT_NE_MSG(ext7, ext0, "extension count changes size");
}

UTEST_F(VulkanLoaderFixture, NullPhysicalDevice) {
    reset_fake(7);
    const size_t nullScratch = ffxGetScratchMemorySizeVK(VK_NULL_HANDLE, 1);
    EXPECT_GT_MSG(nullScratch, size_t(0), "null physical device scratch size");
    EXPECT_EQ_MSG(g_fakeEnumeration.callCount, 0u, "null physical device call count");
}

UTEST_F(VulkanLoaderFixture, InsufficientScratch) {
    reset_fake(0);
    const size_t required = ffxGetScratchMemorySizeVK(utest_fixture->physicalDevice, 1);
    ASSERT_GT_MSG(required, size_t(0), "required scratch size");
    std::vector<uint8_t> scratch(required, 0);
    reset_fake(0);
    const FfxDevice device = ffxGetDeviceVK(&utest_fixture->deviceContext);
    ASSERT_TRUE_MSG(device != nullptr, "device result");
    FfxInterface backend = {};
    EXPECT_EQ_MSG(ffxGetInterfaceVK(&backend, device, scratch.data(), required - 1, 1),
                  FFX_ERROR_INSUFFICIENT_MEMORY, "insufficient scratch result");
    EXPECT_EQ_MSG(g_fakeEnumeration.callCount, 1u, "insufficient scratch enumeration count");
    EXPECT_EQ_MSG(g_fakeEnumeration.lastPhysicalDevice, utest_fixture->physicalDevice,
                  "insufficient scratch physical device");
    EXPECT_EQ_MSG(g_fakeEnumeration.lastLayerName, nullptr,
                  "insufficient scratch layer name");
}

UTEST_F(VulkanLoaderFixture, InterfaceCallbacks) {
    reset_fake(7);
    const size_t scratchSize = ffxGetScratchMemorySizeVK(utest_fixture->physicalDevice, 1);
    ASSERT_GT_MSG(scratchSize, size_t(0), "advertised scratch size");
    ASSERT_LE_MSG(scratchSize,
                  std::numeric_limits<size_t>::max() - (alignof(std::max_align_t) - 1),
                  "advertised scratch size overflow bound");
    const size_t scratchUnits =
        (scratchSize + alignof(std::max_align_t) - 1) / alignof(std::max_align_t);
    std::vector<std::max_align_t> scratch(scratchUnits);
    std::memset(scratch.data(), 0, scratchSize);

    reset_fake(7);
    const FfxDevice device = ffxGetDeviceVK(&utest_fixture->deviceContext);
    ASSERT_TRUE_MSG(device != nullptr, "device result");
    FfxInterface backend = {};
    EXPECT_EQ_MSG(ffxGetInterfaceVK(&backend, device, scratch.data(),
                                    scratchSize, 1), FFX_OK,
                  "interface result");
    EXPECT_EQ_MSG(g_fakeEnumeration.callCount, 1u, "interface enumeration count");
    EXPECT_EQ_MSG(g_fakeEnumeration.lastPhysicalDevice, utest_fixture->physicalDevice,
                  "interface physical device");
    EXPECT_EQ_MSG(g_fakeEnumeration.lastLayerName, nullptr, "interface layer name");
    ASSERT_TRUE_MSG(backend.fpCreateBackendContext != nullptr, "create callback");
    ASSERT_TRUE_MSG(backend.fpGetDeviceCapabilities != nullptr, "capabilities callback");
    ASSERT_TRUE_MSG(backend.fpDestroyBackendContext != nullptr, "destroy callback");
    ASSERT_TRUE_MSG(backend.fpCreateResource != nullptr, "resource callback");
    ASSERT_TRUE_MSG(backend.fpScheduleGpuJob != nullptr, "schedule callback");
}

UTEST_F(VulkanLoaderFixture, BackendLifetimesExt0Max1) {
    run_backend_context_case(utest_result, "ext0/max1", utest_fixture->physicalDevice,
                             &utest_fixture->deviceContext, 0, 1, 16);
}

UTEST_F(VulkanLoaderFixture, BackendLifetimesExt7Max2) {
    run_backend_context_case(utest_result, "ext7/max2", utest_fixture->physicalDevice,
                             &utest_fixture->deviceContext, 7, 2, 0);
}

UTEST_F(VulkanLoaderFixture, MemoryAmdCoherentExclusion) {
    set_fake_memory_properties({VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 0, 0, 0, 0, 0,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD});
    VkMemoryRequirements req = {}; req.memoryTypeBits = UINT32_MAX;
    VkMemoryPropertyFlags out = 0xdeadbeefu;
    const uint32_t index = findMemoryTypeIndex(utest_fixture->physicalDevice, req,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, out);
    EXPECT_EQ_MSG(index, 0u, "AMD coherent index");
    EXPECT_EQ_MSG(out, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "AMD coherent flags");
    EXPECT_EQ_MSG(g_fakeMemory.callCount, 1u, "AMD coherent query");
    EXPECT_EQ_MSG(g_fakeMemory.lastPhysicalDevice, utest_fixture->physicalDevice, "AMD coherent physical device");
}

UTEST_F(VulkanLoaderFixture, MemoryAmdUncachedExclusion) {
    set_fake_memory_properties({VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 0, 0, 0, 0, 0,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD});
    VkMemoryRequirements req = {}; req.memoryTypeBits = UINT32_MAX;
    VkMemoryPropertyFlags out = 0xdeadbeefu;
    const uint32_t index = findMemoryTypeIndex(utest_fixture->physicalDevice, req,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, out);
    EXPECT_EQ_MSG(index, 0u, "AMD uncached index");
    EXPECT_EQ_MSG(out, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "AMD uncached flags");
    EXPECT_EQ_MSG(g_fakeMemory.callCount, 1u, "AMD uncached query");
    EXPECT_EQ_MSG(g_fakeMemory.lastPhysicalDevice, utest_fixture->physicalDevice, "AMD uncached physical device");
}

UTEST_F(VulkanLoaderFixture, MemoryCombinedPartialRejection) {
    set_fake_memory_properties({VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT});
    VkMemoryRequirements req = {}; req.memoryTypeBits = 3;
    VkMemoryPropertyFlags out = 0xdeadbeefu;
    const uint32_t index = findMemoryTypeIndex(utest_fixture->physicalDevice, req,
                                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, out);
    EXPECT_EQ_MSG(index, 1u, "combined partial rejection index");
    EXPECT_EQ_MSG(out, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "combined partial rejection flags");
    EXPECT_EQ_MSG(g_fakeMemory.callCount, 1u, "combined partial rejection query");
    EXPECT_EQ_MSG(g_fakeMemory.lastPhysicalDevice, utest_fixture->physicalDevice, "combined partial rejection physical device");
}

UTEST_F(VulkanLoaderFixture, MemoryOnlyAmdCoherentNoMatch) {
    set_fake_memory_properties({VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD});
    VkMemoryRequirements req = {}; req.memoryTypeBits = 1;
    VkMemoryPropertyFlags out = 0xdeadbeefu;
    const uint32_t index = findMemoryTypeIndex(utest_fixture->physicalDevice, req,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, out);
    EXPECT_EQ_MSG(index, UINT32_MAX, "only coherent index");
    EXPECT_EQ_MSG(out, 0u, "only coherent flags");
    EXPECT_EQ_MSG(g_fakeMemory.callCount, 1u, "only coherent query");
    EXPECT_EQ_MSG(g_fakeMemory.lastPhysicalDevice, utest_fixture->physicalDevice, "only coherent physical device");
}

UTEST_F(VulkanLoaderFixture, MemoryOnlyAmdUncachedNoMatch) {
    set_fake_memory_properties({VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD});
    VkMemoryRequirements req = {}; req.memoryTypeBits = 1;
    VkMemoryPropertyFlags out = 0xdeadbeefu;
    const uint32_t index = findMemoryTypeIndex(utest_fixture->physicalDevice, req,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, out);
    EXPECT_EQ_MSG(index, UINT32_MAX, "only uncached index");
    EXPECT_EQ_MSG(out, 0u, "only uncached flags");
    EXPECT_EQ_MSG(g_fakeMemory.callCount, 1u, "only uncached query");
    EXPECT_EQ_MSG(g_fakeMemory.lastPhysicalDevice, utest_fixture->physicalDevice, "only uncached physical device");
}

UTEST_F(VulkanLoaderFixture, MemoryTypeBitsExclusion) {
    set_fake_memory_properties({VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0});
    VkMemoryRequirements req = {}; req.memoryTypeBits = 2;
    VkMemoryPropertyFlags out = 0xdeadbeefu;
    const uint32_t index = findMemoryTypeIndex(utest_fixture->physicalDevice, req,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, out);
    EXPECT_EQ_MSG(index, UINT32_MAX, "bits exclusion index");
    EXPECT_EQ_MSG(out, 0u, "bits exclusion flags");
    EXPECT_EQ_MSG(g_fakeMemory.callCount, 1u, "bits exclusion query");
    EXPECT_EQ_MSG(g_fakeMemory.lastPhysicalDevice, utest_fixture->physicalDevice, "bits exclusion physical device");
}

UTEST_F(VulkanLoaderFixture, MemoryTypeIndex31) {
    set_fake_memory_properties({});
    g_fakeMemory.properties.memoryTypeCount = 32;
    g_fakeMemory.properties.memoryHeapCount = 1;
    g_fakeMemory.properties.memoryHeaps[0].size = 1;
    g_fakeMemory.properties.memoryTypes[31].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VkMemoryRequirements req = {}; req.memoryTypeBits = 1u << 31;
    VkMemoryPropertyFlags out = 0xdeadbeefu;
    const uint32_t index = findMemoryTypeIndex(utest_fixture->physicalDevice, req,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, out);
    EXPECT_EQ_MSG(index, 31u, "memory type index 31 index");
    EXPECT_EQ_MSG(out, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "memory type index 31 flags");
    EXPECT_EQ_MSG(g_fakeMemory.callCount, 1u, "memory type index 31 query");
    EXPECT_EQ_MSG(g_fakeMemory.lastPhysicalDevice, utest_fixture->physicalDevice, "memory type index 31 physical device");
}

UTEST_F(VulkanLoaderFixture, MemoryMissingCombinedNoMatch) {
    set_fake_memory_properties({VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT});
    VkMemoryRequirements req = {}; req.memoryTypeBits = 3;
    VkMemoryPropertyFlags out = 0xdeadbeefu;
    const uint32_t index = findMemoryTypeIndex(utest_fixture->physicalDevice, req,
                                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, out);
    EXPECT_EQ_MSG(index, UINT32_MAX, "missing combined index");
    EXPECT_EQ_MSG(out, 0u, "missing combined flags");
    EXPECT_EQ_MSG(g_fakeMemory.callCount, 1u, "missing combined query");
    EXPECT_EQ_MSG(g_fakeMemory.lastPhysicalDevice, utest_fixture->physicalDevice, "missing combined physical device");
}

UTEST_F(VulkanLoaderFixture, MemoryHostVisibleFallback) {
    set_fake_memory_properties({VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT});
    VkMemoryRequirements req = {}; req.memoryTypeBits = 3;
    VkMemoryPropertyFlags out = 0xdeadbeefu;
    const uint32_t index = findMemoryTypeIndex(utest_fixture->physicalDevice, req,
                                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, out);
    EXPECT_EQ_MSG(index, 0u, "host fallback index");
    EXPECT_EQ_MSG(out, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, "host fallback flags");
    EXPECT_EQ_MSG(g_fakeMemory.callCount, 1u, "host fallback query");
    EXPECT_EQ_MSG(g_fakeMemory.lastPhysicalDevice, utest_fixture->physicalDevice, "host fallback physical device");
}

UTEST_F(VulkanLoaderFixture, MemoryInvisibleDeviceLocalPreference) {
    set_fake_memory_properties({VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT});
    VkMemoryRequirements req = {}; req.memoryTypeBits = 3;
    VkMemoryPropertyFlags out = 0xdeadbeefu;
    const uint32_t index = findMemoryTypeIndex(utest_fixture->physicalDevice, req,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, out);
    EXPECT_EQ_MSG(index, 1u, "device preference index");
    EXPECT_EQ_MSG(out, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "device preference flags");
    EXPECT_EQ_MSG(g_fakeMemory.callCount, 1u, "device preference query");
    EXPECT_EQ_MSG(g_fakeMemory.lastPhysicalDevice, utest_fixture->physicalDevice, "device preference physical device");
}

UTEST_F(VulkanLoaderFixture, MemoryCoherentPreference) {
    set_fake_memory_properties({VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT});
    VkMemoryRequirements req = {}; req.memoryTypeBits = 3;
    VkMemoryPropertyFlags out = 0xdeadbeefu;
    const uint32_t index = findMemoryTypeIndex(utest_fixture->physicalDevice, req,
                                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, out);
    EXPECT_EQ_MSG(index, 0u, "coherent preference index");
    EXPECT_EQ_MSG(out, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, "coherent preference flags");
    EXPECT_EQ_MSG(g_fakeMemory.callCount, 1u, "coherent preference query");
    EXPECT_EQ_MSG(g_fakeMemory.lastPhysicalDevice, utest_fixture->physicalDevice, "coherent preference physical device");
}

UTEST_MAIN();
