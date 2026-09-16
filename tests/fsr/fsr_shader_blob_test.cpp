// Build-tool validation for the Vulkan FSR3 Upscaler shader accessors.
//
// This test only examines embedded SPIR-V and reflection metadata. It never
// creates a Vulkan object or submits work to a device.

#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <FidelityFX/host/ffx_types.h>
#include "ffx_fsr3upscaler_shaderblobs.h"
#include "fsr3upscaler/ffx_fsr3upscaler_private.h"
#include "spirv_reflect.h"
#include "utest.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

struct PassCase {
    FfxFsr3UpscalerPass pass;
    const char* name;
};

// This is the complete supported switch in
// sdk/src/backends/shared/blob_accessors/ffx_fsr3upscaler_shaderblobs.cpp.
const PassCase kPasses[] = {
    { FFX_FSR3UPSCALER_PASS_PREPARE_INPUTS, "prepare_inputs" },
    { FFX_FSR3UPSCALER_PASS_LUMA_PYRAMID, "luma_pyramid" },
    { FFX_FSR3UPSCALER_PASS_SHADING_CHANGE_PYRAMID, "shading_change_pyramid" },
    { FFX_FSR3UPSCALER_PASS_SHADING_CHANGE, "shading_change" },
    { FFX_FSR3UPSCALER_PASS_PREPARE_REACTIVITY, "prepare_reactivity" },
    { FFX_FSR3UPSCALER_PASS_LUMA_INSTABILITY, "luma_instability" },
    { FFX_FSR3UPSCALER_PASS_ACCUMULATE, "accumulate" },
    { FFX_FSR3UPSCALER_PASS_ACCUMULATE_SHARPEN, "accumulate_sharpen" },
    { FFX_FSR3UPSCALER_PASS_RCAS, "rcas" },
    { FFX_FSR3UPSCALER_PASS_DEBUG_VIEW, "debug_view" },
    { FFX_FSR3UPSCALER_PASS_GENERATE_REACTIVE, "generate_reactive" },
};

enum class ResourceKind {
    Cbv,
    SrvTexture,
    UavTexture,
    SrvBuffer,
    UavBuffer,
    Sampler,
    AccelerationStructure,
};

struct ReflectedResource {
    const char* name;
    uint32_t binding;
    uint32_t set;
    uint32_t count;
    ResourceKind kind;
};

struct ResourceGroup {
    ResourceKind kind;
    uint32_t count;
    const char** names;
    const uint32_t* bindings;
    const uint32_t* counts;
    const uint32_t* sets;
};

std::string pass_prefix(const PassCase& pass, uint32_t options) {
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "%s options=0x%02x", pass.name, options);
    return buffer;
}

std::vector<ResourceGroup> resource_groups(const FfxShaderBlob& blob) {
    return {
        { ResourceKind::Cbv, blob.cbvCount,
          blob.boundConstantBufferNames,
          blob.boundConstantBuffers, blob.boundConstantBufferCounts, blob.boundConstantBufferSpaces },
        { ResourceKind::SrvTexture, blob.srvTextureCount,
          blob.boundSRVTextureNames,
          blob.boundSRVTextures, blob.boundSRVTextureCounts, blob.boundSRVTextureSpaces },
        { ResourceKind::UavTexture, blob.uavTextureCount,
          blob.boundUAVTextureNames,
          blob.boundUAVTextures, blob.boundUAVTextureCounts, blob.boundUAVTextureSpaces },
        { ResourceKind::SrvBuffer, blob.srvBufferCount,
          blob.boundSRVBufferNames,
          blob.boundSRVBuffers, blob.boundSRVBufferCounts, blob.boundSRVBufferSpaces },
        { ResourceKind::UavBuffer, blob.uavBufferCount,
          blob.boundUAVBufferNames,
          blob.boundUAVBuffers, blob.boundUAVBufferCounts, blob.boundUAVBufferSpaces },
        { ResourceKind::Sampler, blob.samplerCount,
          blob.boundSamplerNames,
          blob.boundSamplers, blob.boundSamplerCounts, blob.boundSamplerSpaces },
        { ResourceKind::AccelerationStructure, blob.rtAccelStructCount,
          blob.boundRTAccelerationStructureNames,
          blob.boundRTAccelerationStructures, blob.boundRTAccelerationStructureCounts,
          blob.boundRTAccelerationStructureSpaces },
    };
}

bool descriptor_type_matches(ResourceKind kind, SpvReflectDescriptorType type) {
    switch (kind) {
        case ResourceKind::Cbv:
            return type == SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
                   type == SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        case ResourceKind::SrvTexture:
            return type == SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                   type == SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        case ResourceKind::UavTexture:
            return type == SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        case ResourceKind::SrvBuffer:
            return type == SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
        case ResourceKind::UavBuffer:
            return type == SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER ||
                   type == SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
                   type == SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
        case ResourceKind::Sampler:
            return type == SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLER;
        case ResourceKind::AccelerationStructure:
            return type == SPV_REFLECT_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    }
    return false;
}

class ReflectionModuleGuard {
public:
    explicit ReflectionModuleGuard(SpvReflectShaderModule& module) : module_(module) {}
    ~ReflectionModuleGuard() { spvReflectDestroyShaderModule(&module_); }

    ReflectionModuleGuard(const ReflectionModuleGuard&) = delete;
    ReflectionModuleGuard& operator=(const ReflectionModuleGuard&) = delete;

private:
    SpvReflectShaderModule& module_;
};

void validate_blob(int* utest_result, const PassCase& pass, uint32_t options,
                   const FfxShaderBlob& blob) {
    const std::string prefix = pass_prefix(pass, options);

    ASSERT_TRUE_MSG(blob.data != nullptr && blob.size != 0,
                    (prefix + ": blob is empty").c_str());
    ASSERT_EQ_MSG(blob.size % sizeof(uint32_t), 0u,
                  (prefix + ": SPIR-V size is not word aligned").c_str());
    uint32_t magic = 0;
    std::memcpy(&magic, blob.data, sizeof(magic));
    EXPECT_EQ_MSG(magic, 0x07230203u,
                  (prefix + ": invalid SPIR-V magic").c_str());

    std::vector<ResourceGroup> groups = resource_groups(blob);
    std::set<std::pair<uint32_t, uint32_t>> metadataSlots;
    uint32_t metadataCount = 0;
    std::vector<ReflectedResource> metadata;
    for (const ResourceGroup& group : groups) {
        if (group.count == 0)
            continue;
        ASSERT_TRUE_MSG(group.names != nullptr && group.bindings != nullptr &&
                            group.counts != nullptr && group.sets != nullptr,
                        (prefix + ": reflection arrays are incomplete").c_str());
        for (uint32_t index = 0; index < group.count; ++index) {
            ASSERT_TRUE_MSG(group.names[index] != nullptr && group.names[index][0] != '\0',
                            (prefix + ": reflection resource has no name").c_str());
            EXPECT_NE_MSG(group.counts[index], 0u,
                          (prefix + ": reflection resource has zero count").c_str());
            const auto slot = std::make_pair(group.sets[index], group.bindings[index]);
            EXPECT_TRUE_MSG(metadataSlots.insert(slot).second,
                            (prefix + ": duplicate reflection descriptor slot").c_str());
            metadata.push_back({ group.names[index], group.bindings[index], group.sets[index],
                                 group.counts[index], group.kind });
            ++metadataCount;
        }
    }

    SpvReflectShaderModule module{};
    const SpvReflectResult createResult = spvReflectCreateShaderModule(blob.size, blob.data, &module);
    ASSERT_EQ_MSG(createResult, SPV_REFLECT_RESULT_SUCCESS,
                  (prefix + ": SPIRV-Reflect could not parse blob").c_str());
    ReflectionModuleGuard moduleGuard(module);

    uint32_t reflectedCount = 0;
    SpvReflectResult result = spvReflectEnumerateDescriptorBindings(&module, &reflectedCount, nullptr);
    ASSERT_EQ_MSG(result, SPV_REFLECT_RESULT_SUCCESS,
                  (prefix + ": could not enumerate descriptor bindings").c_str());
    EXPECT_EQ_MSG(reflectedCount, metadataCount,
                  (prefix + ": reflection count does not match SPIR-V").c_str());

    std::vector<SpvReflectDescriptorBinding*> reflected(reflectedCount);
    result = spvReflectEnumerateDescriptorBindings(&module, &reflectedCount, reflected.data());
    ASSERT_EQ_MSG(result, SPV_REFLECT_RESULT_SUCCESS,
                  (prefix + ": descriptor enumeration failed").c_str());
    std::set<std::pair<uint32_t, uint32_t>> spirvSlots;
    for (SpvReflectDescriptorBinding* binding : reflected) {
        ASSERT_TRUE_MSG(binding != nullptr && binding->name != nullptr,
                        (prefix + ": SPIR-V descriptor has no name").c_str());
        EXPECT_TRUE_MSG(spirvSlots.insert({ binding->set, binding->binding }).second,
                        (prefix + ": duplicate SPIR-V descriptor slot").c_str());
        const ReflectedResource* matching = nullptr;
        for (const ReflectedResource& expected : metadata) {
            if (std::strcmp(expected.name, binding->name) == 0 &&
                expected.binding == binding->binding && expected.set == binding->set) {
                matching = &expected;
                break;
            }
        }
        EXPECT_TRUE_MSG(matching != nullptr,
                        (prefix + ": SPIR-V descriptor is absent from blob metadata").c_str());
        if (matching == nullptr)
            continue;
        EXPECT_EQ_MSG(binding->count, matching->count,
                      (prefix + ": descriptor array count disagrees with SPIR-V").c_str());
        EXPECT_TRUE_MSG(descriptor_type_matches(matching->kind, binding->descriptor_type),
                        (prefix + ": descriptor type disagrees with metadata category").c_str());
    }
}

} // namespace

namespace {

constexpr uint32_t kShaderPermutationMask =
    FSR3UPSCALER_SHADER_PERMUTATION_USE_LANCZOS_TYPE |
    FSR3UPSCALER_SHADER_PERMUTATION_HDR_COLOR_INPUT |
    FSR3UPSCALER_SHADER_PERMUTATION_LOW_RES_MOTION_VECTORS |
    FSR3UPSCALER_SHADER_PERMUTATION_JITTER_MOTION_VECTORS |
    FSR3UPSCALER_SHADER_PERMUTATION_DEPTH_INVERTED |
    FSR3UPSCALER_SHADER_PERMUTATION_ENABLE_SHARPENING;

void run_pass(int* utest_result, const PassCase& pass) {
    std::map<std::pair<const uint8_t*, uint32_t>, bool> validated;
    for (uint32_t base = 0; base <= kShaderPermutationMask; ++base) {
        for (uint32_t wave64 = 0; wave64 <= 1; ++wave64) {
            for (uint32_t fp16 = 0; fp16 <= 1; ++fp16) {
                const uint32_t variants =
                    (wave64 != 0 ? FSR3UPSCALER_SHADER_PERMUTATION_FORCE_WAVE64 : 0) |
                    (fp16 != 0 ? FSR3UPSCALER_SHADER_PERMUTATION_ALLOW_FP16 : 0);
                const uint32_t options = base | variants;
                FfxShaderBlob blob{};
                const FfxErrorCode result =
                    fsr3UpscalerGetPermutationBlobByIndex(pass.pass, options, &blob);
                EXPECT_EQ_MSG(result, FFX_OK,
                              (pass_prefix(pass, options) + ": accessor failed").c_str());
                if (result != FFX_OK)
                    continue;

                const auto key = std::make_pair(blob.data, blob.size);
                if (validated.find(key) == validated.end()) {
                    int blob_result = UTEST_TEST_PASSED;
                    validate_blob(&blob_result, pass, options, blob);
                    if (blob_result != UTEST_TEST_PASSED)
                        *utest_result = UTEST_TEST_FAILURE;
                    validated.emplace(key, blob_result == UTEST_TEST_PASSED);
                } else {
                    EXPECT_TRUE_MSG(validated[key],
                                    (pass_prefix(pass, options) + ": shared blob failed validation").c_str());
                }
            }
        }
    }
}

void check_prepare_inputs_fp16(int* utest_result) {
    // The accessor intentionally ignores the FP16 bit for prepare_inputs.
    // Check the easy-to-regress case that prepare_inputs has exactly that behavior.
    const PassCase& pass = kPasses[0];
    for (uint32_t base = 0; base <= kShaderPermutationMask; ++base) {
        for (uint32_t wave64 = 0; wave64 <= 1; ++wave64) {
            const uint32_t wave = wave64 != 0 ? FSR3UPSCALER_SHADER_PERMUTATION_FORCE_WAVE64 : 0;
            FfxShaderBlob withoutFp16{};
            FfxShaderBlob withFp16{};
            const std::string prefix = pass_prefix(pass, base | wave);
            const FfxErrorCode withoutResult = fsr3UpscalerGetPermutationBlobByIndex(
                pass.pass, base | wave, &withoutFp16);
            const FfxErrorCode withResult = fsr3UpscalerGetPermutationBlobByIndex(
                pass.pass, base | wave | FSR3UPSCALER_SHADER_PERMUTATION_ALLOW_FP16, &withFp16);
            EXPECT_EQ_MSG(withoutResult, FFX_OK, (prefix + ": non-FP16 accessor failed").c_str());
            EXPECT_EQ_MSG(withResult, FFX_OK, (prefix + ": FP16 accessor failed").c_str());
            if (withoutResult != FFX_OK || withResult != FFX_OK)
                continue;
            EXPECT_TRUE_MSG(withoutFp16.data == withFp16.data && withoutFp16.size == withFp16.size,
                            (prefix + ": accessor unexpectedly consumes the FP16 permutation bit").c_str());
        }
    }
}

} // namespace

UTEST(FsrShaderBlob, PrepareInputs) { run_pass(utest_result, kPasses[0]); }
UTEST(FsrShaderBlob, LumaPyramid) { run_pass(utest_result, kPasses[1]); }
UTEST(FsrShaderBlob, ShadingChangePyramid) { run_pass(utest_result, kPasses[2]); }
UTEST(FsrShaderBlob, ShadingChange) { run_pass(utest_result, kPasses[3]); }
UTEST(FsrShaderBlob, PrepareReactivity) { run_pass(utest_result, kPasses[4]); }
UTEST(FsrShaderBlob, LumaInstability) { run_pass(utest_result, kPasses[5]); }
UTEST(FsrShaderBlob, Accumulate) { run_pass(utest_result, kPasses[6]); }
UTEST(FsrShaderBlob, AccumulateSharpen) { run_pass(utest_result, kPasses[7]); }
UTEST(FsrShaderBlob, Rcas) { run_pass(utest_result, kPasses[8]); }
UTEST(FsrShaderBlob, DebugView) { run_pass(utest_result, kPasses[9]); }
UTEST(FsrShaderBlob, GenerateReactive) { run_pass(utest_result, kPasses[10]); }

UTEST(FsrShaderBlob, PrepareInputsFp16Ignored) {
    check_prepare_inputs_fp16(utest_result);
}

UTEST_MAIN();
