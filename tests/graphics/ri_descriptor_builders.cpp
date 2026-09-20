// Focused, device-free coverage for the RIDescriptor builders.
#include "graphics/RIDescriptor.h"
#include "system/Hasher.h"

#include <cstdint>
#include <cstdio>
#include <string>

int hplMain(const std::string &) { return 0; }

namespace {

bool Check(bool condition, const char *what) {
  if (!condition)
    std::fprintf(stderr, "failed: %s\n", what);
  return condition;
}

hash_t BufferCookie(hash_t resource, uint8_t type, uint64_t offset,
                    uint64_t range, uint32_t stride, bool raw,
                    bool structured) {
  hash_t h = hash_u64(hash_u64(hash_u64(resource, type), offset), range);
  h = hash_u64(h, stride);
  h = hash_u64(h, raw ? 1u : 0u);
  return hash_u64(h, structured ? 1u : 0u);
}

hash_t ResourceCookie(hash_t resource, uint8_t type) {
  return resource ? hash_u64(resource, type) : 0;
}

bool CheckEmptyInputs() {
  bool ok = true;
  ok &= Check(RIDescriptor::uniformBuffer(nullptr, nullptr, 1, 2).isEmpty(), "null uniform buffer is empty");
  ok &= Check(RIDescriptor::storageBuffer(nullptr, nullptr, 1, 2).isEmpty(), "null storage buffer is empty");
  ok &= Check(RIDescriptor::sampledImage(nullptr, nullptr).isEmpty(), "null sampled image is empty");
  ok &= Check(RIDescriptor::storageImage(nullptr, nullptr).isEmpty(), "null storage image is empty");
  ok &= Check(RIDescriptor::sampler(nullptr, nullptr).isEmpty(), "null sampler is empty");
  ok &= Check(RIDescriptor::accelerationStructure(nullptr, nullptr).isEmpty(), "null acceleration structure is empty");
  return ok;
}

bool CheckBuffers() {
  RIBuffer buffer{};
  buffer.cookie = 0x10203040;
  buffer.vk.buffer = reinterpret_cast<VkBuffer>(0x1234);
  const uint64_t uniformOffset = 0x180, uniformRange = 0x240;
  const uint64_t storageOffset = 0x1f0, storageRange = 0x260;
  const RIDescriptor uniform = RIDescriptor::uniformBuffer(
      nullptr, &buffer, uniformOffset, uniformRange, 16, false, true);
  const RIDescriptor storage = RIDescriptor::storageBuffer(
      nullptr, &buffer, storageOffset, storageRange, 20, true, false);

  bool ok = true;
  ok &= Check(uniform.type == RI_DESCRIPTOR_TYPE_UNIFORM_BUFFER, "uniform buffer type");
  ok &= Check(uniform.payload.buffer.offset == uniformOffset &&
                  uniform.payload.buffer.range == uniformRange &&
                  uniform.payload.buffer.stride == 16 &&
                  uniform.payload.buffer.raw == 0 &&
                  uniform.payload.buffer.structured == 1,
              "uniform neutral buffer fields");
  ok &= Check(uniform.vk.buffer.buffer == buffer.vk.buffer, "uniform buffer handle");
  ok &= Check(uniform.cookie == BufferCookie(buffer.cookie, uniform.type,
                                             uniformOffset, uniformRange, 16,
                                             false, true),
              "uniform buffer cookie includes descriptor metadata");
  ok &= Check(storage.type == RI_DESCRIPTOR_TYPE_STORAGE_BUFFER &&
                  storage.payload.buffer.offset == storageOffset &&
                  storage.payload.buffer.range == storageRange &&
                  storage.payload.buffer.stride == 20 &&
                  storage.payload.buffer.raw == 1 &&
                  storage.payload.buffer.structured == 0,
              "storage neutral buffer fields");
  ok &= Check(storage.vk.buffer.offset == storageOffset &&
                  storage.vk.buffer.range == storageRange,
              "storage buffer offset and range");
  ok &= Check(storage.cookie == BufferCookie(buffer.cookie, storage.type,
                                             storageOffset, storageRange, 20,
                                             true, false),
              "storage buffer cookie includes descriptor metadata");
  const RIDescriptor differentMetadata = RIDescriptor::storageBuffer(
      nullptr, &buffer, storageOffset, storageRange, 21, true, false);
  ok &= Check(differentMetadata.cookie != storage.cookie,
              "buffer cookie distinguishes stride");
  return ok;
}

bool CheckTextureAndSampler() {
  RITexture texture{};
  RITextureView view{};
  view.resource = &texture;
  view.dimension = RI_TEXTURE_2D;
  view.format = RI_FORMAT_RGBA8_UNORM;
  view.viewType = RI_VIEWTYPE_SHADER_RESOURCE_2D_ARRAY;
  view.baseMip = 2;
  view.mipNum = 3;
  view.baseLayer = 4;
  view.layerNum = 5;
  view.cookie = 0x55667788;
  view.vk.image = reinterpret_cast<VkImageView>(0x5678);

  const RIDescriptor sampled = RIDescriptor::sampledImage(
      nullptr, &view, RI_RESOURCE_STATE_SHADER_RESOURCE);
  const RIDescriptor storage = RIDescriptor::storageImage(nullptr, &view);
  RISampler sampler{};
  sampler.cookie = 0xabcdef01;
  sampler.wrapS = 1;
  sampler.wrapT = 2;
  sampler.wrapR = 3;
  sampler.filter = 4;
  sampler.vk.sampler = reinterpret_cast<VkSampler>(0x9abc);
  const RIDescriptor samplerDescriptor = RIDescriptor::sampler(nullptr, &sampler);

  bool ok = true;
  ok &= Check(sampled.payload.texture.dimension == RI_TEXTURE_2D &&
                  sampled.payload.texture.viewType == RI_VIEWTYPE_SHADER_RESOURCE_2D_ARRAY &&
                  sampled.payload.texture.format == RI_FORMAT_RGBA8_UNORM &&
                  sampled.payload.texture.baseMip == 2 &&
                  sampled.payload.texture.mipNum == 3 &&
                  sampled.payload.texture.baseLayer == 4 &&
                  sampled.payload.texture.layerNum == 5,
              "sampled texture view dimension format and subresources");
  view.viewType = RI_VIEWTYPE_SHADER_RESOURCE_CUBE_ARRAY;
  const RIDescriptor cube = RIDescriptor::sampledImage(nullptr, &view);
  ok &= Check(sampled.payload.texture.viewType == RI_VIEWTYPE_SHADER_RESOURCE_2D_ARRAY &&
                  cube.payload.texture.viewType == RI_VIEWTYPE_SHADER_RESOURCE_CUBE_ARRAY,
              "texture payload snapshots array and cube view types");
  ok &= Check(sampled.vk.image.imageView == view.vk.image &&
                  sampled.vk.image.imageLayout == ri_vk_RIResourceStateToImageLayout(
                      RI_RESOURCE_STATE_SHADER_RESOURCE),
              "sampled image handle and layout");
  ok &= Check(storage.payload.texture.dimension == RI_TEXTURE_2D &&
                  storage.payload.texture.format == RI_FORMAT_RGBA8_UNORM &&
                  storage.vk.image.imageView == view.vk.image &&
                  storage.vk.image.imageLayout == VK_IMAGE_LAYOUT_GENERAL,
              "storage texture view and layout");
  ok &= Check(sampled.cookie ==
                  hash_u64(hash_u64(view.cookie, sampled.type),
                           RI_RESOURCE_STATE_SHADER_RESOURCE),
              "sampled image cookie includes state");
  ok &= Check(storage.cookie == ResourceCookie(view.cookie, storage.type), "storage image cookie");
  ok &= Check(samplerDescriptor.payload.sampler.wrapS == 1 &&
                  samplerDescriptor.payload.sampler.wrapT == 2 &&
                  samplerDescriptor.payload.sampler.wrapR == 3 &&
                  samplerDescriptor.payload.sampler.filter == 4,
              "sampler description");
  ok &= Check(samplerDescriptor.vk.image.sampler == sampler.vk.sampler, "sampler handle");
  ok &= Check(samplerDescriptor.cookie == ResourceCookie(sampler.cookie, samplerDescriptor.type), "sampler cookie");
  return ok;
}

bool CheckAccelerationStructure() {
  RIAccelStructure as{};
  as.cookie = 0x13579bdf;
  as.vk.handle = reinterpret_cast<VkAccelerationStructureKHR>(0x2468);
  as.vk.deviceAddress = 0x123456789abcdef0ull;
  const RIDescriptor descriptor = RIDescriptor::accelerationStructure(nullptr, &as);
  bool ok = Check(descriptor.payload.accel.gpuVA == as.vk.deviceAddress,
                  "acceleration structure GPU VA");
  ok &= Check(descriptor.vk.accelStructure == as.vk.handle, "acceleration structure handle");
  ok &= Check(descriptor.cookie == ResourceCookie(as.cookie, descriptor.type), "acceleration structure cookie");
  return ok;
}

} // namespace

int main() {
  bool ok = true;
  ok &= CheckEmptyInputs();
  ok &= CheckBuffers();
  ok &= CheckTextureAndSampler();
  ok &= CheckAccelerationStructure();
  std::printf("RIDescriptor builder tests %s\n", ok ? "passed" : "failed");
  return ok ? 0 : 1;
}
