// Opt-in GPU smoke test for D3D12 (DXR) acceleration structures.
//
// Builds a single-triangle BLAS and a one-instance TLAS end to end:
//   getMemoryReqs -> allocate storage/scratch -> RIAccelStructure::init
//   -> buildBlas -> barrier -> buildTlas -> submit -> waitIdle
//
// Skips itself cleanly when the selected adapter reports rayTracingTier == 0,
// so it is safe to run on WARP or pre-DXR hardware. Run manually:
//   build-premake\tests\<Config>\RID3D12AccelStructureSmoke.exe
//   build-premake\tests\<Config>\RID3D12AccelStructureSmoke.exe --vulkan
#include "graphics/RIBuffer.h"
#include "graphics/RICommand.h"
#include "graphics/RID3D12.h"
#include "graphics/RIDescriptor.h"
#include "graphics/RIDevice.h"
#include "graphics/RIRenderer.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
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

void Skip(const char *check) { std::printf("SKIP: %s\n", check); }

uint8_t RequestedApi(bool vulkan) {
  return vulkan ? RI_DEVICE_API_VK : RI_DEVICE_API_D3D12;
}

// One CCW triangle in the z = 0 plane, plus its index buffer. Small enough that
// the BLAS is trivially valid but large enough that a zero-primitive build
// would be caught.
struct TriangleGeometry {
  RIBuffer vertices;
  RIBuffer indices;

  void dispose(RIDevice *device) {
    vertices.dispose(device);
    indices.dispose(device);
  }
};

// Uploads through a mapped UPLOAD-heap buffer rather than the resource
// uploader: this test is about acceleration structures, and a host-visible
// build input keeps the fixture to one submit.
RIBuffer CreateHostBuffer(RIDevice *device, const void *data, uint64_t size,
                          uint32_t usage, const char *debugName) {
  RIBufferDesc desc = {};
  desc.size = size;
  desc.usage = usage | RI_BUFFER_USAGE_DEVICE_ADDRESS;
  desc.location = RI_MEMORY_HOST_UPLOAD;
  RIBuffer buffer = RIBuffer::create(device, desc);
  Require(!buffer.isEmpty(), "acceleration-structure build input allocates");
  Require(buffer.mappedAddress != nullptr,
          "acceleration-structure build input is host mapped");
  std::memcpy(buffer.mappedAddress, data, size_t(size));
  buffer.setDebugObjectName(device, debugName);
  return buffer;
}

TriangleGeometry CreateTriangle(RIDevice *device) {
  static const float kVertices[9] = {
      0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
  static const uint32_t kIndices[3] = {0, 1, 2};
  TriangleGeometry geometry;
  geometry.vertices = CreateHostBuffer(
      device, kVertices, sizeof(kVertices),
      RI_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPT,
      "AccelSmoke.Vertices");
  geometry.indices = CreateHostBuffer(
      device, kIndices, sizeof(kIndices),
      RI_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPT, "AccelSmoke.Indices");
  return geometry;
}

void FillTriangleGeometryDesc(RIAccelGeometryDesc *out,
                              TriangleGeometry *geometry) {
  memset(out, 0, sizeof(*out));
  out->type = RI_ACCEL_GEOMETRY_TYPE_TRIANGLES;
  out->flags = RI_ACCEL_GEOMETRY_OPAQUE;
  out->triangles.vertexBuffer = &geometry->vertices;
  out->triangles.vertexOffset = 0;
  out->triangles.vertexNum = 3;
  out->triangles.vertexStride = 12;
  out->triangles.vertexFormat = RI_FORMAT_RGB32_SFLOAT;
  out->triangles.indexBuffer = &geometry->indices;
  out->triangles.indexOffset = 0;
  out->triangles.indexNum = 3;
  out->triangles.indexType = RI_INDEX_TYPE_32;
}

RIBuffer CreateAccelStorage(RIDevice *device, uint64_t size,
                            const char *debugName) {
  RIBufferDesc desc = {};
  desc.size = size;
  desc.usage = RI_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE |
               RI_BUFFER_USAGE_DEVICE_ADDRESS;
  desc.location = RI_MEMORY_DEVICE;
  RIBuffer buffer = RIBuffer::create(device, desc);
  Require(!buffer.isEmpty(), "acceleration-structure storage allocates");
  buffer.setDebugObjectName(device, debugName);
  return buffer;
}

RIBuffer CreateScratch(RIDevice *device, uint64_t size, const char *debugName) {
  RIBufferDesc desc = {};
  desc.size = size;
  desc.usage = RI_BUFFER_USAGE_SCRATCH | RI_BUFFER_USAGE_DEVICE_ADDRESS |
               RI_BUFFER_USAGE_SHADER_RESOURCE_STORAGE;
  desc.location = RI_MEMORY_DEVICE;
  RIBuffer buffer = RIBuffer::create(device, desc);
  Require(!buffer.isEmpty(), "acceleration-structure scratch allocates");
  buffer.setDebugObjectName(device, debugName);
  return buffer;
}

void RunAccelCases(RIDevice *device, bool vulkan) {
  Require(device->accelerationStructureEnabled,
          "device publishes accelerationStructureEnabled for a raytraced request");

  TriangleGeometry geometry = CreateTriangle(device);

  // ---------------- BLAS ----------------
  RIAccelGeometryDesc geometryDesc;
  FillTriangleGeometryDesc(&geometryDesc, &geometry);

  RIAccelStructureDesc blasDesc = {};
  blasDesc.type = RI_ACCEL_STRUCTURE_TYPE_BOTTOM_LEVEL;
  blasDesc.flags = RI_ACCEL_BUILD_PREFER_FAST_TRACE;
  blasDesc.geometryOrInstanceNum = 1;
  blasDesc.geometries = &geometryDesc;

  uint64_t blasStorageSize = 0;
  uint64_t blasScratchSize = 0;
  blasDesc.getMemoryReqs(device, &blasStorageSize, &blasScratchSize, nullptr);
  Require(blasStorageSize > 0, "BLAS prebuild reports a non-zero storage size");
  Require(blasScratchSize > 0, "BLAS prebuild reports a non-zero scratch size");

  RIBuffer blasStorage =
      CreateAccelStorage(device, blasStorageSize, "AccelSmoke.BlasStorage");
  RIBuffer blasScratch =
      CreateScratch(device, blasScratchSize, "AccelSmoke.BlasScratch");
  blasDesc.storage = &blasStorage;
  blasDesc.storageOffset = 0;
  blasDesc.storageSize = blasStorageSize;

  RIAccelStructure blas;
  Require(blas.isEmpty(), "BLAS starts empty");
  Require(blas.init(device, &blasDesc) == RI_SUCCESS, "BLAS init succeeds");
  Require(!blas.isEmpty(), "BLAS is non-empty after init");
  Require(blas.getDeviceAddress(device) != 0,
          "BLAS reports a non-zero device address");
  Require(blas.cookie != 0, "BLAS cookie is populated");
  blas.setDebugObjectName(device, "AccelSmoke.Blas");

  // ---------------- TLAS ----------------
  // One instance referencing the BLAS. The record layout is shared between
  // backends (see the static_asserts in cWorld::BuildTlas).
  VkAccelerationStructureInstanceKHR instance = {};
  instance.transform.matrix[0][0] = 1.0f;
  instance.transform.matrix[1][1] = 1.0f;
  instance.transform.matrix[2][2] = 1.0f;
  instance.instanceCustomIndex = 0;
  instance.mask = 0xff;
  instance.instanceShaderBindingTableRecordOffset = 0;
  instance.flags = 0;
  instance.accelerationStructureReference = blas.getDeviceAddress(device);

  RIBuffer instanceBuffer = CreateHostBuffer(
      device, &instance, sizeof(instance),
      RI_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPT,
      "AccelSmoke.Instances");

  RIAccelStructureDesc tlasDesc = {};
  tlasDesc.type = RI_ACCEL_STRUCTURE_TYPE_TOP_LEVEL;
  tlasDesc.flags = RI_ACCEL_BUILD_PREFER_FAST_TRACE;
  tlasDesc.geometryOrInstanceNum = 1;

  uint64_t tlasStorageSize = 0;
  uint64_t tlasScratchSize = 0;
  tlasDesc.getMemoryReqs(device, &tlasStorageSize, &tlasScratchSize, nullptr);
  Require(tlasStorageSize > 0, "TLAS prebuild reports a non-zero storage size");
  Require(tlasScratchSize > 0, "TLAS prebuild reports a non-zero scratch size");

  RIBuffer tlasStorage =
      CreateAccelStorage(device, tlasStorageSize, "AccelSmoke.TlasStorage");
  RIBuffer tlasScratch =
      CreateScratch(device, tlasScratchSize, "AccelSmoke.TlasScratch");
  tlasDesc.storage = &tlasStorage;
  tlasDesc.storageOffset = 0;
  tlasDesc.storageSize = tlasStorageSize;

  RIAccelStructure tlas;
  Require(tlas.init(device, &tlasDesc) == RI_SUCCESS, "TLAS init succeeds");
  Require(!tlas.isEmpty(), "TLAS is non-empty after init");
  Require(tlas.getDeviceAddress(device) != 0,
          "TLAS reports a non-zero device address");
  Require(tlas.getDeviceAddress(device) != blas.getDeviceAddress(device),
          "TLAS and BLAS occupy distinct storage");
  Require(tlas.cookie != blas.cookie,
          "each acceleration structure gets a unique cookie");
  tlas.setDebugObjectName(device, "AccelSmoke.Tlas");

  // A TLAS binds as a descriptor; confirm the builder resolves it rather than
  // producing an empty descriptor (this is what the RT shaders consume).
  RIDescriptor tlasDescriptor =
      RIDescriptor::accelerationStructure(device, &tlas);
  Require(!tlasDescriptor.isEmpty(),
          "acceleration-structure descriptor resolves");
  Require(tlasDescriptor.payload.accel.gpuVA == tlas.getDeviceAddress(device),
          "acceleration-structure descriptor carries the TLAS address");

  // ---------------- Record and submit ----------------
  RIPool pool;
  pool.init(device, &device->queues[RI_QUEUE_GRAPHICS]);
  RICmd cmd;
  cmd.init(device, &pool);
  cmd.begin(device);

  RIBuildBlasDesc blasBuild = {};
  blasBuild.dst = &blas;
  blasBuild.src = nullptr;
  blasBuild.mode = RI_ACCEL_BUILD_MODE_BUILD;
  blasBuild.geometries = &geometryDesc;
  blasBuild.geometryNum = 1;
  blasBuild.scratchBuffer = &blasScratch;
  blasBuild.scratchOffset = 0;
  cmd.buildBlas(device, &blasBuild, 1);

  // The TLAS build reads the BLAS in the same command list here (unlike the
  // engine, which splits them across submits), so the write->read barrier is
  // required rather than merely tidy.
  cmd.vk_d3d12_memoryBarrier({RI_RESOURCE_STATE_ACCEL_WRITE,
                              RI_RESOURCE_STATE_ACCEL_READ,
                              RI_STAGE_ACCEL_BUILD, RI_STAGE_ACCEL_BUILD});

  RIBuildTlasDesc tlasBuild = {};
  tlasBuild.dst = &tlas;
  tlasBuild.src = nullptr;
  tlasBuild.mode = RI_ACCEL_BUILD_MODE_BUILD;
  tlasBuild.instanceNum = 1;
  tlasBuild.instanceBuffer = &instanceBuffer;
  tlasBuild.instanceOffset = 0;
  tlasBuild.scratchBuffer = &tlasScratch;
  tlasBuild.scratchOffset = 0;
  cmd.buildTlas(device, &tlasBuild, 1);

  cmd.vk_d3d12_memoryBarrier({RI_RESOURCE_STATE_ACCEL_WRITE,
                              RI_RESOURCE_STATE_ACCEL_READ,
                              RI_STAGE_ACCEL_BUILD, RI_STAGE_RAY_TRACING});
  cmd.end(device);

  RICmd *submitList[] = {&cmd};
  RISubmitDesc submit = {};
  submit.cmds = submitList;
  submit.cmdCount = 1;
  Require(device->queues[RI_QUEUE_GRAPHICS].submit(device, submit) ==
              RI_SUCCESS,
          "acceleration-structure build submits");
  device->queues[RI_QUEUE_GRAPHICS].waitIdle(device);
  Require(true, "acceleration-structure build completes without device loss");
  if (!vulkan)
    Require(!RID3D12_CheckDeviceRemoved(*device, "accel_structure_smoke"),
            "D3D12 device survives the acceleration-structure build");

  // A zero-instance TLAS build is legal and must not fault: HybridRenderer
  // emits one for worlds with no ray-traced geometry.
  cmd.begin(device);
  RIBuildTlasDesc emptyBuild = tlasBuild;
  emptyBuild.instanceNum = 0;
  cmd.buildTlas(device, &emptyBuild, 1);
  cmd.end(device);
  Require(device->queues[RI_QUEUE_GRAPHICS].submit(device, submit) ==
              RI_SUCCESS,
          "empty TLAS build submits");
  device->queues[RI_QUEUE_GRAPHICS].waitIdle(device);
  if (!vulkan)
    Require(!RID3D12_CheckDeviceRemoved(*device, "accel_structure_smoke.empty"),
            "D3D12 device survives an empty TLAS build");

  // buildBlas/buildTlas with numDescs == 0 must be a no-op, not a crash.
  cmd.begin(device);
  cmd.buildBlas(device, nullptr, 0);
  cmd.buildTlas(device, nullptr, 0);
  cmd.end(device);
  Require(true, "zero-count builds are a no-op");

  cmd.dispose(device);
  pool.dispose(device);

  // ---------------- Teardown ----------------
  // Disposing an acceleration structure must not release the caller-owned
  // storage buffer it borrows from; the storage dispose below is what frees it.
  tlas.dispose(device);
  Require(tlas.isEmpty(), "TLAS disposes empty");
  blas.dispose(device);
  Require(blas.isEmpty(), "BLAS disposes empty");
  if (!vulkan) {
    Require(tlasStorage.d3d12.resource != nullptr,
            "TLAS dispose leaves the borrowed storage resource alive");
    Require(blasStorage.d3d12.resource != nullptr,
            "BLAS dispose leaves the borrowed storage resource alive");
  }
  Require(!tlasStorage.isEmpty() && !blasStorage.isEmpty(),
          "acceleration-structure storage survives structure disposal");
  tlas.dispose(device);
  blas.dispose(device);
  Require(tlas.isEmpty() && blas.isEmpty(),
          "acceleration-structure double-dispose is safe");

  instanceBuffer.dispose(device);
  tlasScratch.dispose(device);
  tlasStorage.dispose(device);
  blasScratch.dispose(device);
  blasStorage.dispose(device);
  geometry.dispose(device);
}

void RunAccelCycle(bool vulkan) {
  RIBackendInit init = {};
  init.api = RequestedApi(vulkan);
  init.applicationName =
      vulkan ? "RIVulkanAccelStructureSmoke" : "RID3D12AccelStructureSmoke";
  if (!vulkan)
    g_riD3D12EnableDebugLayer = true;
  Require(InitRIRenderer(&init) == RI_SUCCESS,
          vulkan ? "Vulkan InitRIRenderer succeeds"
                 : "D3D12 InitRIRenderer succeeds");

  uint32_t numAdapters = 0;
  Require(EnumerateRIAdapters(nullptr, &numAdapters) == RI_SUCCESS &&
              numAdapters >= 1 && numAdapters <= 8,
          "enumeration reports adapters");
  RIPhysicalAdapter adapters[8];
  uint32_t capacity = numAdapters;
  Require(EnumerateRIAdapters(adapters, &capacity) == RI_SUCCESS &&
              capacity >= 1 && capacity <= 8,
          "enumeration populates adapters");

  // Prefer a hardware adapter: WARP's DXR support varies by Windows build, and
  // this test is about the hardware path.
  uint32_t selected = 0;
  if (!vulkan) {
    for (uint32_t i = 0; i < capacity; ++i) {
      if (!adapters[i].d3d12.isWarp) {
        selected = i;
        break;
      }
    }
    if (adapters[selected].d3d12.rayTracingTier == 0) {
      Skip("selected D3D12 adapter reports no DXR support");
      ShutdownRIRenderer();
      g_riD3D12EnableDebugLayer = false;
      return;
    }
    std::printf("INFO: D3D12 adapter rayTracingTier=%u\n",
                unsigned(adapters[selected].d3d12.rayTracingTier));
  }

  RIDevice device;
  RIDeviceDesc deviceDesc = {};
  deviceDesc.physicalAdapter = &adapters[selected];
  deviceDesc.requestRayTracing = 1;
  Require(device.init(&deviceDesc) == RI_SUCCESS && RIDeviceIsValid(&device),
          "raytraced device initializes and is valid");
  if (!vulkan)
    Require(device.d3d12.device5 != nullptr,
            "D3D12 raytraced device exposes ID3D12Device5");

  if (!device.accelerationStructureEnabled) {
    Skip("device did not enable acceleration structures");
  } else {
    RunAccelCases(&device, vulkan);
  }

  device.dispose();
  if (!vulkan)
    Require(device.d3d12.device == nullptr && device.d3d12.device5 == nullptr,
            "D3D12 device disposal clears device and device5 handles");
  ShutdownRIRenderer();
  if (!vulkan)
    g_riD3D12EnableDebugLayer = false;
}

} // namespace

int main(int argc, char **argv) {
  bool vulkan = false;
  for (int i = 1; i < argc; ++i)
    if (std::strcmp(argv[i], "--vulkan") == 0)
      vulkan = true;
  RunAccelCycle(vulkan);
  return 0;
}
