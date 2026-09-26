#include "graphics/RICommand.h"
#include "graphics/RID3D12.h"
#include "graphics/RIDevice.h"
#include "graphics/RIProgram.h"
#include "system/Platform.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#if DEVICE_IMPL_D3D12

namespace hpl {


void RIProgram::bindD3D12Pipeline(struct RIDevice *device, struct RICmd *cmd,
                                  hash_t pipelineHash, const char *debugName,
                                  const RIGraphicsPipelineDesc &pipelineDesc) {
  if (!device || !cmd || !cmd->d3d12.cmdList)
    FatalError("RIProgram: invalid D3D12 graphics pipeline bind arguments\n");
  if (!impl.d3d12.rootSignature)
    FatalError("RIProgram: D3D12 graphics pipeline has no initialized root "
               "signature\n");

  const RIRenderTargetDesc &rt = pipelineDesc.renderTarget;
  if (rt.colorCount > RI_MAX_COLOR_ATTACHMENTS)
    FatalError("RIProgram: too many color attachments for D3D12 PSO\n");
  if (pipelineDesc.blendCount != rt.colorCount)
    FatalError("RIProgram: blend attachment count (%u) does not match "
               "render-target count (%u)\n",
               pipelineDesc.blendCount, rt.colorCount);

  // The caller-supplied hash is only a variant tag; RIProgram::bindPipeline has
  // already folded the whole pipeline state into it via
  // RIHashGraphicsPipelineDesc, so it identifies this PSO on its own. The
  // shader bytes deliberately stay out of the key: `pipeline` is a per-program
  // member map that dispose() clears, so a reload cannot leave a stale PSO
  // behind for a rebuilt program to find, and folding half a megabyte of DXIL
  // and reflection JSON in here would run once per draw.
  const hash_t cacheKey = pipelineHash;
  auto it = pipeline.find(cacheKey);
  if (it == pipeline.end()) {
    const auto &vs = shaderBin[PROGRAM_STAGE_VERTEX];
    const auto &ps = shaderBin[PROGRAM_STAGE_FRAGMENT];
    if (vs.buf.empty() || ps.buf.empty())
      FatalError(
          "RIProgram: D3D12 graphics PSO requires vertex and fragment DXIL\n");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    PipelineSlot slot = {};
    desc.pRootSignature = impl.d3d12.rootSignature;
    desc.VS = {vs.buf.data(), vs.buf.size()};
    desc.PS = {ps.buf.data(), ps.buf.size()};
    desc.PrimitiveTopologyType =
        ri_d3d12_RITopologyTypeToD3D12(pipelineDesc.topology);
    slot.d3d12.topology = ri_d3d12_RITopologyToD3D12(pipelineDesc.topology);
    slot.d3d12.stencilRef = pipelineDesc.depthStencil.stencilReference;

    // Input layout. D3D12 identifies vertex inputs by HLSL semantic, so each
    // RI attribute location is resolved through the vertex shader's
    // reflection -- the same contract as the legacy path.
    std::vector<D3D12_INPUT_ELEMENT_DESC> inputs;
    {
      const RIVertexInputDesc &in = pipelineDesc.vertexInput;
      if (in.bindingCount > RI_MAX_VERTEX_BINDINGS ||
          in.attributeCount > RI_MAX_VERTEX_ATTRIBUTES)
        FatalError("RIProgram: D3D12 vertex input exceeds RI capacity\n");
      for (uint32_t b = 0; b < in.bindingCount; ++b) {
        const RIVertexBindingDesc &binding = in.bindings[b];
        if (binding.binding >= MAX_VERTEX_ATTRIBUTES)
          FatalError("RIProgram: D3D12 vertex binding index is out of range\n");
        slot.d3d12.vertexBindingStrides[binding.binding] = binding.stride;
        slot.d3d12.vertexBindingCount =
            std::max(slot.d3d12.vertexBindingCount, binding.binding + 1);
      }
      inputs.resize(in.attributeCount);
      for (uint32_t i = 0; i < in.attributeCount; ++i) {
        const RIVertexAttributeDesc &a = in.attributes[i];
        const RIVertexBindingDesc *binding = nullptr;
        for (uint32_t b = 0; b < in.bindingCount; ++b)
          if (in.bindings[b].binding == a.binding)
            binding = &in.bindings[b];
        if (!binding)
          FatalError(
              "RIProgram: D3D12 vertex attribute references missing binding\n");
        if (!vs.reflection)
          FatalError(
              "RIProgram: D3D12 vertex shader has no reflection metadata\n");
        const auto semantic = std::find_if(
            vs.reflection->vertexInputs.begin(),
            vs.reflection->vertexInputs.end(),
            [&](const auto &input) { return input.location == a.location; });
        if (semantic == vs.reflection->vertexInputs.end() ||
            semantic->semanticName.empty() ||
            semantic->semanticName.rfind("SV_", 0) == 0)
          FatalError("RIProgram: D3D12 vertex attribute location %u has no "
                     "reflected user semantic\n",
                     a.location);
        const DXGI_FORMAT format = RIFormatToD3D12(a.format);
        if (format == DXGI_FORMAT_UNKNOWN)
          FatalError("RIProgram: unsupported RI vertex format %u for D3D12\n",
                     static_cast<unsigned>(a.format));
        const bool perInstance =
            binding->inputRate == RI_VERTEX_INPUT_RATE_INSTANCE;
        D3D12_INPUT_ELEMENT_DESC &out = inputs[i];
        out.SemanticName = semantic->semanticName.c_str();
        out.SemanticIndex = semantic->semanticIndex;
        out.Format = format;
        out.InputSlot = a.binding;
        out.AlignedByteOffset = a.offset;
        out.InputSlotClass = ri_d3d12_RIVertexInputRateToD3D12(binding->inputRate);
        out.InstanceDataStepRate = perInstance ? 1 : 0;
      }
      // D3D12 requires an input-layout element for every entry in the vertex
      // shader's input signature, including one the shader never reads
      // (ReadWriteMask == 0) -- DXC keeps such an input in the signature where
      // Slang's SPIR-V backend prunes it, so a layout that satisfies Vulkan can
      // be short here. Unchecked, that surfaces only as a bare E_INVALIDARG
      // from CreateGraphicsPipelineState; name the missing semantic instead.
      if (vs.reflection) {
        for (const auto &input : vs.reflection->vertexInputs) {
          if (input.semanticName.empty() ||
              input.semanticName.rfind("SV_", 0) == 0)
            continue;
          const bool covered =
              std::any_of(in.attributes, in.attributes + in.attributeCount,
                          [&](const RIVertexAttributeDesc &a) {
                            return a.location == input.location;
                          });
          if (!covered)
            FatalError("RIProgram: D3D12 input layout for '%s' omits vertex "
                       "shader input '%s%u' (location %u); D3D12 requires an "
                       "element for every input-signature entry, including one "
                       "the shader never reads\n",
                       debugName ? debugName : "<unnamed>",
                       input.semanticName.c_str(), input.semanticIndex,
                       input.location);
        }
      }
    }
    desc.InputLayout = {inputs.data(), static_cast<UINT>(inputs.size())};

    const RIRasterizationDesc &r = pipelineDesc.raster;
    desc.RasterizerState.FillMode = ri_d3d12_RIPolygonModeToD3D12(r.polygonMode);
    desc.RasterizerState.CullMode = ri_d3d12_RICullModeToD3D12(r.cullMode);
    desc.RasterizerState.FrontCounterClockwise =
        r.frontFace == RI_FRONT_FACE_COUNTER_CLOCKWISE;
    desc.RasterizerState.DepthBias =
        r.depthBiasEnable ? static_cast<INT>(r.depthBiasConstant) : 0;
    desc.RasterizerState.DepthBiasClamp = r.depthBiasClamp;
    desc.RasterizerState.SlopeScaledDepthBias = r.depthBiasSlope;
    desc.RasterizerState.DepthClipEnable = !r.depthClamp;
    desc.RasterizerState.AntialiasedLineEnable = false;

    const uint32_t sampleCount =
        pipelineDesc.sampleCount ? pipelineDesc.sampleCount : 1u;
    if ((sampleCount & (sampleCount - 1)) || sampleCount > 16)
      FatalError("RIProgram: unsupported sample count %u for D3D12\n",
                 sampleCount);
    desc.RasterizerState.MultisampleEnable = sampleCount != 1;
    desc.SampleDesc.Count = sampleCount;
    desc.SampleMask = 0xffffffffu;

    desc.BlendState.AlphaToCoverageEnable = pipelineDesc.alphaToCoverage;
    desc.BlendState.IndependentBlendEnable = pipelineDesc.blendCount > 1;
    for (uint32_t i = 0; i < pipelineDesc.blendCount; ++i) {
      const RIBlendAttachmentDesc &a = pipelineDesc.blend[i];
      D3D12_RENDER_TARGET_BLEND_DESC &o = desc.BlendState.RenderTarget[i];
      o.BlendEnable = a.blendEnable;
      o.SrcBlend = ri_d3d12_RIBlendFactorToD3D12(a.srcColor);
      o.DestBlend = ri_d3d12_RIBlendFactorToD3D12(a.dstColor);
      o.BlendOp = ri_d3d12_RIBlendOpToD3D12(a.colorOp);
      o.SrcBlendAlpha = ri_d3d12_RIBlendFactorToD3D12Alpha(a.srcAlpha);
      o.DestBlendAlpha = ri_d3d12_RIBlendFactorToD3D12Alpha(a.dstAlpha);
      o.BlendOpAlpha = ri_d3d12_RIBlendOpToD3D12(a.alphaOp);
      o.RenderTargetWriteMask = ri_d3d12_RIColorWriteMaskToD3D12(a.writeMask);
    }

    desc.NumRenderTargets = rt.colorCount;
    for (uint32_t i = 0; i < rt.colorCount; ++i) {
      desc.RTVFormats[i] = RIFormatToD3D12(rt.colorFormats[i]);
      if (desc.RTVFormats[i] == DXGI_FORMAT_UNKNOWN)
        FatalError("RIProgram: unsupported D3D12 render-target format %u\n",
                   static_cast<unsigned>(rt.colorFormats[i]));
    }
    // D3D12 has one DSV format for both aspects; a depth format wins when
    // present, matching the legacy path's dsvFormat resolution.
    const RI_Format_e dsFormat = rt.depthFormat != RI_FORMAT_UNKNOWN
                                     ? rt.depthFormat
                                     : rt.stencilFormat;
    desc.DSVFormat = dsFormat == RI_FORMAT_UNKNOWN
                         ? DXGI_FORMAT_UNKNOWN
                         : RIFormatToD3D12(dsFormat);
    if (dsFormat != RI_FORMAT_UNKNOWN && desc.DSVFormat == DXGI_FORMAT_UNKNOWN)
      FatalError("RIProgram: unsupported D3D12 depth/stencil format %u\n",
                 static_cast<unsigned>(dsFormat));

    const RIDepthStencilDesc &d = pipelineDesc.depthStencil;
    desc.DepthStencilState.DepthEnable = d.depthTest;
    desc.DepthStencilState.DepthWriteMask = d.depthWrite
                                                ? D3D12_DEPTH_WRITE_MASK_ALL
                                                : D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.DepthStencilState.DepthFunc =
        ri_d3d12_RICompareFuncToD3D12(d.depthCompare);
    desc.DepthStencilState.StencilEnable = d.stencilTest;
    // D3D12 has a single read/write mask pair shared by both faces; the front
    // face supplies it, as it does on the legacy path.
    desc.DepthStencilState.StencilReadMask =
        static_cast<UINT8>(d.front.compareMask);
    desc.DepthStencilState.StencilWriteMask =
        static_cast<UINT8>(d.front.writeMask);
    desc.DepthStencilState.FrontFace.StencilFailOp =
        ri_d3d12_RIStencilOpToD3D12(d.front.failOp);
    desc.DepthStencilState.FrontFace.StencilDepthFailOp =
        ri_d3d12_RIStencilOpToD3D12(d.front.depthFailOp);
    desc.DepthStencilState.FrontFace.StencilPassOp =
        ri_d3d12_RIStencilOpToD3D12(d.front.passOp);
    desc.DepthStencilState.FrontFace.StencilFunc =
        ri_d3d12_RICompareFuncToD3D12(d.front.compareFunc);
    desc.DepthStencilState.BackFace.StencilFailOp =
        ri_d3d12_RIStencilOpToD3D12(d.back.failOp);
    desc.DepthStencilState.BackFace.StencilDepthFailOp =
        ri_d3d12_RIStencilOpToD3D12(d.back.depthFailOp);
    desc.DepthStencilState.BackFace.StencilPassOp =
        ri_d3d12_RIStencilOpToD3D12(d.back.passOp);
    desc.DepthStencilState.BackFace.StencilFunc =
        ri_d3d12_RICompareFuncToD3D12(d.back.compareFunc);

    createD3D12GraphicsPipeline(device, cacheKey, debugName, desc, slot);
    it = pipeline.find(cacheKey);
  }
  applyD3D12GraphicsPipeline(cmd, it->second, debugName);
}

void RIProgram::createD3D12GraphicsPipeline(
    struct RIDevice *device, hash_t cacheKey, const char *debugName,
    D3D12_GRAPHICS_PIPELINE_STATE_DESC &desc, PipelineSlot &slot) {
  const HRESULT psoResult = device->d3d12.device->CreateGraphicsPipelineState(
      &desc, IID_PPV_ARGS(&slot.d3d12.handle));
  if (!D3D12_WrapResult(psoResult)) {
    // The debug layer reports the field that D3D12 rejected.  Drain it
    // before FatalError aborts so a scene-specific PSO failure is
    // actionable even when the callback interface is unavailable.
    RID3D12_DrainDeviceMessages(*device);
    // A device lost earlier fails every PSO creation; report the loss
    // rather than blaming this pipeline's description.
    RID3D12_CheckDeviceRemoved(
        *device, debugName ? debugName : "CreateGraphicsPipelineState");
    // The render-target formats and the input layout are the two parts D3D12
    // rejects most often and the two the caller cannot infer from the counts
    // alone, so spell them out: a mismatch against the shader signatures is
    // otherwise invisible without the debug layer.
    std::string rtvFormats;
    for (UINT i = 0; i < desc.NumRenderTargets; ++i) {
      if (i)
        rtvFormats += ' ';
      rtvFormats += std::to_string(static_cast<unsigned>(desc.RTVFormats[i]));
    }
    std::string layout;
    for (UINT i = 0; i < desc.InputLayout.NumElements; ++i) {
      const D3D12_INPUT_ELEMENT_DESC &e = desc.InputLayout.pInputElementDescs[i];
      if (i)
        layout += ' ';
      layout += e.SemanticName ? e.SemanticName : "<null>";
      layout += std::to_string(e.SemanticIndex);
      layout += "@slot" + std::to_string(e.InputSlot);
    }
    FatalError("RIProgram: CreateGraphicsPipelineState failed for '%s' "
               "(HRESULT 0x%08lX, RTs=%u [%s], DSV=%u, samples=%u, "
               "topology=%u, inputs=%u [%s])\n"
               "RI D3D12: rerun with HPL_D3D12_VALIDATION=1 for the "
               "debug-layer message naming the rejected field\n",
               debugName ? debugName : "<unnamed>",
               static_cast<unsigned long>(psoResult), desc.NumRenderTargets,
               rtvFormats.c_str(), static_cast<unsigned>(desc.DSVFormat),
               desc.SampleDesc.Count,
               static_cast<unsigned>(desc.PrimitiveTopologyType),
               desc.InputLayout.NumElements, layout.c_str());
  }
  if (debugName && slot.d3d12.handle) {
    const size_t n = strlen(debugName);
    std::wstring name(n, L' ');
    for (size_t i = 0; i < n; ++i)
      name[i] = static_cast<wchar_t>(debugName[i]);
    slot.d3d12.handle->SetName(name.c_str());
  }
  pipeline.emplace(cacheKey, slot);
}

void RIProgram::applyD3D12GraphicsPipeline(struct RICmd *cmd,
                                           const PipelineSlot &slot,
                                           const char *debugName) {
  cmd->d3d12.cmdList->SetPipelineState(slot.d3d12.handle);
  RID3D12_SetGraphicsRootSignature(*cmd, impl.d3d12.rootSignature);
  RID3D12_NoteBoundProgramRootArgs(*cmd, false, impl.d3d12.rootArgumentMask,
                                   debugName);
  cmd->d3d12.vertexBindingCount = slot.d3d12.vertexBindingCount;
  memcpy(cmd->d3d12.vertexBindingStrides, slot.d3d12.vertexBindingStrides,
         sizeof(cmd->d3d12.vertexBindingStrides));
  d3d12VertexBindingCount = slot.d3d12.vertexBindingCount;
  memcpy(d3d12VertexBindingStrides.data(), slot.d3d12.vertexBindingStrides,
         sizeof(slot.d3d12.vertexBindingStrides));
  RID3D12_RebindCachedVertexBuffers(*cmd);
  cmd->d3d12.computePipelineBound = false;
  cmd->d3d12.cmdList->IASetPrimitiveTopology(
      static_cast<D3D12_PRIMITIVE_TOPOLOGY>(slot.d3d12.topology));
  // Not PSO state in D3D12; set unconditionally so a pipeline that does not
  // use stencil resets the reference a previous one left behind.
  cmd->d3d12.cmdList->OMSetStencilRef(slot.d3d12.stencilRef);
}

void RIProgram::bindD3D12ComputePipeline(struct RIDevice *device,
                                         struct RICmd *cmd,
                                         hash_t pipelineHash,
                                         const char *debugName) {
  if (!device || !cmd || !cmd->d3d12.cmdList || !impl.d3d12.rootSignature)
    FatalError("RIProgram: invalid D3D12 compute pipeline bind state\n");
  // The shader size is folded in purely as a domain tag, so a compute key
  // cannot collide with a graphics one in this program's shared pipeline map.
  // The DXIL itself stays out for the same reason as the graphics path: the
  // map is per-program and dispose() clears it.
  const hash_t cacheKey =
      hash_u64(pipelineHash, shaderBin[PROGRAM_STAGE_COMPUTE].buf.size());
  auto it = pipeline.find(cacheKey);
  if (it == pipeline.end()) {
    const auto &cs = shaderBin[PROGRAM_STAGE_COMPUTE];
    if (cs.buf.empty())
      FatalError("RIProgram: D3D12 compute PSO requires DXIL\n");
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = impl.d3d12.rootSignature;
    desc.CS = {cs.buf.data(), cs.buf.size()};
    PipelineSlot slot = {};
    if (!D3D12_WrapResult(device->d3d12.device->CreateComputePipelineState(
            &desc, IID_PPV_ARGS(&slot.d3d12.handle)))) {
      RID3D12_DrainDeviceMessages(*device);
      RID3D12_CheckDeviceRemoved(
          *device, debugName ? debugName : "CreateComputePipelineState");
      FatalError("RIProgram: CreateComputePipelineState failed for '%s'\n",
                 debugName ? debugName : "<unnamed>");
    }
    if (debugName && slot.d3d12.handle) {
      const size_t n = strlen(debugName);
      std::wstring name(n, L' ');
      for (size_t i = 0; i < n; ++i)
        name[i] = static_cast<wchar_t>(debugName[i]);
      slot.d3d12.handle->SetName(name.c_str());
    }
    pipeline.emplace(cacheKey, slot);
    it = pipeline.find(cacheKey);
  }
  cmd->d3d12.cmdList->SetPipelineState(it->second.d3d12.handle);
  RID3D12_SetComputeRootSignature(*cmd, impl.d3d12.rootSignature);
  RID3D12_NoteBoundProgramRootArgs(*cmd, true, impl.d3d12.rootArgumentMask,
                                   debugName);
  cmd->d3d12.computePipelineBound = true;
  return;
}

namespace {

// The ray-tracing stages a state object can carry, in SBT region order:
// raygen, then miss, then the three that make up the hit group, then
// callable. Parallel to the populated shaderBin[] slots, and to the Vulkan
// path's kRTStages in RIProgram.cpp.
constexpr RIProgram::ProgramStages kRtStages[] = {
    RIProgram::PROGRAM_STAGE_RAYGEN,      RIProgram::PROGRAM_STAGE_MISS,
    RIProgram::PROGRAM_STAGE_CLOSEST_HIT, RIProgram::PROGRAM_STAGE_ANY_HIT,
    RIProgram::PROGRAM_STAGE_INTERSECTION, RIProgram::PROGRAM_STAGE_CALLABLE,
};
constexpr size_t kRtStageCount = sizeof(kRtStages) / sizeof(kRtStages[0]);

// Index into kRtStages, named so the group assembly below reads as intent
// rather than as subscripts.
enum RtStageIndex {
  kRtRaygen = 0,
  kRtMiss = 1,
  kRtClosestHit = 2,
  kRtAnyHit = 3,
  kRtIntersection = 4,
  kRtCallable = 5,
};

// DXR names exports in wide characters while the artifact metadata carries
// them as ASCII entry points. Every name involved is an HLSL/Slang
// identifier, so the widening is a straight per-character copy -- the same
// one the PSO paths above use for debug names.
std::wstring ri_d3d12_widen(const char *text) {
  const size_t n = text ? strlen(text) : 0;
  std::wstring wide(n, L' ');
  for (size_t i = 0; i < n; ++i)
    wide[i] = static_cast<wchar_t>(text[i]);
  return wide;
}

// The hit group needs a name of its own to be referenced from the SBT. It
// must not collide with a shader export, and a Slang identifier cannot
// contain '.', so this cannot clash with an entry point.
constexpr const wchar_t *kRtHitGroupName = L"ri.hitGroup";

uint64_t ri_d3d12_alignUp(uint64_t value, uint64_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

} // namespace

void RIProgram::createD3D12RayTracingPipeline(
    struct RIDevice *device, const char *debugName,
    const RIRayTracingPipelineDesc &desc, RTPipelineSlot &slot) {
  // Every ray-tracing stage of a Slang source compiles to one lib_6_8 DXIL
  // library, and initialize() copies that same library into each stage's
  // shaderBin. Emitting one library subobject per distinct blob therefore
  // normally yields exactly one, with all the entry points as its exports --
  // but the grouping is done by content rather than assumed, so a program
  // assembled from separate libraries still builds.
  std::wstring exportNames[kRtStageCount];
  D3D12_EXPORT_DESC exportDescs[kRtStageCount] = {};
  D3D12_DXIL_LIBRARY_DESC libraries[kRtStageCount] = {};
  // Which library each stage's exports landed in, and how many exports that
  // library has so far. Exports for one library must be contiguous, so they
  // are gathered per library first and flattened afterwards.
  uint32_t exportsPerLibrary[kRtStageCount] = {};
  uint32_t libraryOfStage[kRtStageCount] = {};
  int32_t stagePresent[kRtStageCount];
  uint32_t libraryCount = 0;
  for (size_t i = 0; i < kRtStageCount; ++i)
    stagePresent[i] = -1;

  for (size_t i = 0; i < kRtStageCount; ++i) {
    const auto &bin = shaderBin[kRtStages[i]];
    if (bin.buf.empty())
      continue;
    uint32_t library = UINT32_MAX;
    for (uint32_t existing = 0; existing < libraryCount; ++existing) {
      const auto &blob = libraries[existing].DXILLibrary;
      if (blob.BytecodeLength == bin.buf.size() &&
          memcmp(blob.pShaderBytecode, bin.buf.data(), bin.buf.size()) == 0) {
        library = existing;
        break;
      }
    }
    if (library == UINT32_MAX) {
      library = libraryCount++;
      libraries[library].DXILLibrary = {bin.buf.data(), bin.buf.size()};
    }
    exportNames[i] = ri_d3d12_widen(bin.entryPoint.c_str());
    if (exportNames[i].empty())
      FatalError("RIProgram: D3D12 ray-tracing stage of '%s' has no entry "
                 "point name to export\n",
                 debugName ? debugName : "<unnamed>");
    libraryOfStage[i] = library;
    exportsPerLibrary[library]++;
    stagePresent[i] = static_cast<int32_t>(i);
  }
  if (stagePresent[kRtRaygen] < 0)
    FatalError("RIProgram: D3D12 ray-tracing pipeline '%s' requires a raygen "
               "shader\n",
               debugName ? debugName : "<unnamed>");

  // Flatten the exports so each library points at a contiguous run.
  uint32_t exportCursor = 0;
  for (uint32_t library = 0; library < libraryCount; ++library) {
    const uint32_t first = exportCursor;
    for (size_t i = 0; i < kRtStageCount; ++i) {
      if (stagePresent[i] < 0 || libraryOfStage[i] != library)
        continue;
      exportDescs[exportCursor].Name = exportNames[i].c_str();
      // No renaming and no flags: the export keeps the entry point's own name,
      // which is what the SBT lookups below ask for.
      exportDescs[exportCursor].ExportToRename = nullptr;
      exportDescs[exportCursor].Flags = D3D12_EXPORT_FLAG_NONE;
      exportCursor++;
    }
    libraries[library].NumExports = exportsPerLibrary[library];
    libraries[library].pExports = &exportDescs[first];
  }

  // One triangles hit group, matching the Vulkan path's single hit group.
  // None of the engine's ray-tracing shaders declare an intersection shader --
  // shadow rays use inline RayQuery -- so a procedural group never arises, but
  // an intersection export would make this one procedural by construction.
  D3D12_HIT_GROUP_DESC hitGroup = {};
  const bool hasHitGroup = stagePresent[kRtClosestHit] >= 0 ||
                           stagePresent[kRtAnyHit] >= 0 ||
                           stagePresent[kRtIntersection] >= 0;
  if (hasHitGroup) {
    hitGroup.HitGroupExport = kRtHitGroupName;
    hitGroup.Type = stagePresent[kRtIntersection] >= 0
                        ? D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE
                        : D3D12_HIT_GROUP_TYPE_TRIANGLES;
    if (stagePresent[kRtClosestHit] >= 0)
      hitGroup.ClosestHitShaderImport = exportNames[kRtClosestHit].c_str();
    if (stagePresent[kRtAnyHit] >= 0)
      hitGroup.AnyHitShaderImport = exportNames[kRtAnyHit].c_str();
    if (stagePresent[kRtIntersection] >= 0)
      hitGroup.IntersectionShaderImport =
          exportNames[kRtIntersection].c_str();
  }

  // Payload and attribute sizes cannot be recovered from the DXIL the way
  // Vulkan recovers them from SPIR-V, so they come from the caller's desc.
  D3D12_RAYTRACING_SHADER_CONFIG shaderConfig = {};
  shaderConfig.MaxPayloadSizeInBytes = desc.maxPayloadSize;
  shaderConfig.MaxAttributeSizeInBytes =
      desc.maxAttributeSize ? desc.maxAttributeSize
                            : D3D12_RAYTRACING_MAX_ATTRIBUTE_SIZE_IN_BYTES;
  if (shaderConfig.MaxPayloadSizeInBytes == 0)
    FatalError("RIProgram: D3D12 ray-tracing pipeline '%s' declares a zero "
               "payload size; set RIRayTracingPipelineDesc::maxPayloadSize to "
               "the size of the payload struct the shaders use\n",
               debugName ? debugName : "<unnamed>");

  D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig = {};
  pipelineConfig.MaxTraceRecursionDepth =
      desc.maxRecursionDepth ? desc.maxRecursionDepth : 1;

  // The program's root signature is built by the reflected path precisely so
  // it can serve as the global root signature here -- see the ray-tracing
  // branch in ri_create_reflected_d3d12_root_signature's caller. There is no
  // local root signature: the SBT carries shader identifiers only, with no
  // per-record data.
  D3D12_GLOBAL_ROOT_SIGNATURE globalRootSignature = {};
  globalRootSignature.pGlobalRootSignature = impl.d3d12.rootSignature;

  // Subobjects are left unassociated, which in DXR means they apply to every
  // export in the state object. With exactly one shader config, one pipeline
  // config and one global root signature, that default is what we want, and
  // it saves the explicit association subobjects.
  std::vector<D3D12_STATE_SUBOBJECT> subobjects;
  subobjects.reserve(libraryCount + 4);
  for (uint32_t library = 0; library < libraryCount; ++library)
    subobjects.push_back(
        {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &libraries[library]});
  if (hasHitGroup)
    subobjects.push_back({D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hitGroup});
  subobjects.push_back(
      {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shaderConfig});
  subobjects.push_back(
      {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG,
       &pipelineConfig});
  subobjects.push_back({D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE,
                        &globalRootSignature});

  D3D12_STATE_OBJECT_DESC stateObjectDesc = {};
  stateObjectDesc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
  stateObjectDesc.NumSubobjects = static_cast<UINT>(subobjects.size());
  stateObjectDesc.pSubobjects = subobjects.data();

  const HRESULT stateObjectResult = device->d3d12.device5->CreateStateObject(
      &stateObjectDesc, IID_PPV_ARGS(&slot.d3d12.handle));
  if (!D3D12_WrapResult(stateObjectResult)) {
    // Same diagnostic ordering as the PSO paths: the debug layer names the
    // rejected subobject, and a device already lost fails every creation.
    RID3D12_DrainDeviceMessages(*device);
    RID3D12_CheckDeviceRemoved(*device,
                               debugName ? debugName : "CreateStateObject");
    FatalError("RIProgram: CreateStateObject failed for '%s' (HRESULT "
               "0x%08lX, libraries=%u, exports=%u, payload=%u, attribs=%u, "
               "recursion=%u)\n",
               debugName ? debugName : "<unnamed>",
               static_cast<unsigned long>(stateObjectResult), libraryCount,
               exportCursor, shaderConfig.MaxPayloadSizeInBytes,
               shaderConfig.MaxAttributeSizeInBytes,
               pipelineConfig.MaxTraceRecursionDepth);
  }
  if (debugName && slot.d3d12.handle)
    slot.d3d12.handle->SetName(ri_d3d12_widen(debugName).c_str());

  // Shader identifiers stand in for Vulkan's shader group handles.
  ID3D12StateObjectProperties *properties = nullptr;
  if (!D3D12_WrapResult(
          slot.d3d12.handle->QueryInterface(IID_PPV_ARGS(&properties))) ||
      !properties)
    FatalError("RIProgram: ID3D12StateObjectProperties unavailable for '%s'\n",
               debugName ? debugName : "<unnamed>");

  const void *raygenId =
      properties->GetShaderIdentifier(exportNames[kRtRaygen].c_str());
  const void *missId =
      stagePresent[kRtMiss] >= 0
          ? properties->GetShaderIdentifier(exportNames[kRtMiss].c_str())
          : nullptr;
  const void *hitGroupId =
      hasHitGroup ? properties->GetShaderIdentifier(kRtHitGroupName) : nullptr;
  const void *callableId =
      stagePresent[kRtCallable] >= 0
          ? properties->GetShaderIdentifier(exportNames[kRtCallable].c_str())
          : nullptr;
  properties->Release();

  // A null identifier means the export never made it into the state object,
  // which CreateStateObject does not itself reject. Catching it here beats a
  // DispatchRays that reads a zeroed record.
  if (!raygenId || (stagePresent[kRtMiss] >= 0 && !missId) ||
      (hasHitGroup && !hitGroupId) ||
      (stagePresent[kRtCallable] >= 0 && !callableId))
    FatalError("RIProgram: state object '%s' is missing a shader identifier; "
               "an entry point name does not match the compiled library\n",
               debugName ? debugName : "<unnamed>");

  // SBT layout: one record per group, each padded to the record alignment,
  // and each region starting on the table alignment. Mirrors the Vulkan SBT,
  // with DXR's fixed constants in place of the device-queried ones.
  constexpr uint64_t kIdentifierSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
  constexpr uint64_t kRecordAlign =
      D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT;
  constexpr uint64_t kTableAlign =
      D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;
  const uint64_t recordStride = ri_d3d12_alignUp(kIdentifierSize, kRecordAlign);
  const uint64_t raygenRegionSize = ri_d3d12_alignUp(recordStride, kTableAlign);
  const uint64_t missRegionSize =
      missId ? ri_d3d12_alignUp(recordStride, kTableAlign) : 0;
  const uint64_t hitRegionSize =
      hitGroupId ? ri_d3d12_alignUp(recordStride, kTableAlign) : 0;
  const uint64_t callableRegionSize =
      callableId ? ri_d3d12_alignUp(recordStride, kTableAlign) : 0;
  const uint64_t sbtSize =
      raygenRegionSize + missRegionSize + hitRegionSize + callableRegionSize;

  // Host-upload so the identifiers can be memcpy'd straight in: an upload-heap
  // resource is readable by DispatchRays as-is, so there is no staging copy
  // and no barrier. RI_BUFFER_USAGE_DEVICE_ADDRESS is deliberately absent --
  // on D3D12 it would register the buffer in the shared geometry raw-SRV
  // arena, and GetDeviceHandle already returns the GPU VA without it.
  slot.sbt = RIBuffer::create(
      device, {sbtSize, RI_BUFFER_USAGE_BINDING_TABLE, RI_MEMORY_HOST_UPLOAD,
               kTableAlign});
  if (slot.sbt.isEmpty() || !slot.sbt.mappedAddress)
    FatalError("RIProgram: failed to allocate a %llu-byte shader binding "
               "table for '%s'\n",
               static_cast<unsigned long long>(sbtSize),
               debugName ? debugName : "<unnamed>");
  if (debugName)
    slot.sbt.setDebugObjectName(device, debugName);

  uint8_t *mapped = static_cast<uint8_t *>(slot.sbt.mappedAddress);
  memset(mapped, 0, static_cast<size_t>(sbtSize));
  uint64_t regionOffset = 0;
  const auto writeRecord = [&](const void *identifier, uint64_t regionSize) {
    if (identifier)
      memcpy(mapped + regionOffset, identifier,
             static_cast<size_t>(kIdentifierSize));
    regionOffset += regionSize;
  };
  const D3D12_GPU_VIRTUAL_ADDRESS sbtBase = slot.sbt.GetDeviceHandle(device);
  writeRecord(raygenId, raygenRegionSize);
  writeRecord(missId, missRegionSize);
  writeRecord(hitGroupId, hitRegionSize);
  writeRecord(callableId, callableRegionSize);

  // An unused table is left as a null range; DXR reads a table only when a
  // trace actually references it, and a zero-size range says so explicitly.
  slot.d3d12.raygenRange.StartAddress = sbtBase;
  slot.d3d12.raygenRange.SizeInBytes = raygenRegionSize;
  slot.d3d12.missRange.StartAddress = missId ? sbtBase + raygenRegionSize : 0;
  slot.d3d12.missRange.SizeInBytes = missRegionSize;
  slot.d3d12.missRange.StrideInBytes = missId ? recordStride : 0;
  slot.d3d12.hitRange.StartAddress =
      hitGroupId ? sbtBase + raygenRegionSize + missRegionSize : 0;
  slot.d3d12.hitRange.SizeInBytes = hitRegionSize;
  slot.d3d12.hitRange.StrideInBytes = hitGroupId ? recordStride : 0;
  slot.d3d12.callableRange.StartAddress =
      callableId ? sbtBase + raygenRegionSize + missRegionSize + hitRegionSize
                 : 0;
  slot.d3d12.callableRange.SizeInBytes = callableRegionSize;
  slot.d3d12.callableRange.StrideInBytes = callableId ? recordStride : 0;
}

void RIProgram::bindD3D12RayTracingPipeline(
    struct RIDevice *device, struct RICmd *cmd, hash_t pipelineHash,
    const char *debugName, const RIRayTracingPipelineDesc &desc) {
  if (!device || !cmd || !cmd->d3d12.cmdList || !impl.d3d12.rootSignature)
    FatalError("RIProgram: invalid D3D12 ray-tracing pipeline bind state\n");
  // SetPipelineState1 and DispatchRays arrived with ID3D12GraphicsCommandList4,
  // which cmdList7 subsumes -- the same handle the acceleration-structure
  // builds use.
  if (!ri_d3d12_dxrAvailable(*device) || !cmd->d3d12.cmdList7)
    FatalError("RIProgram: ray-tracing pipeline '%s' bound on a device without "
               "DXR support\n",
               debugName ? debugName : "<unnamed>");

  // What the state object is actually built from. The map stays keyed by the
  // caller's variant tag, because traceRays is handed nothing else to look the
  // slot up with; this is compared against the cached slot instead, so a desc
  // change behind a reused tag rebuilds in place. Only the desc fields and the
  // entry-point identifiers go in -- they are the parts that genuinely vary
  // under one tag, and they are tiny. The DXIL itself is excluded: rtPipeline
  // is a per-program member map that dispose() clears, so a reload cannot
  // leave a stale state object behind for a rebuilt program to find.
  hash_t contentKey = hash_u64(pipelineHash, desc.maxRecursionDepth);
  contentKey = hash_u64(contentKey, desc.maxPayloadSize);
  contentKey = hash_u64(contentKey, desc.maxAttributeSize);
  for (size_t i = 0; i < kRtStageCount; ++i) {
    const auto &bin = shaderBin[kRtStages[i]];
    if (bin.buf.empty())
      continue;
    contentKey =
        hash_data(contentKey, bin.entryPoint.data(), bin.entryPoint.size());
  }

  auto it = rtPipeline.find(pipelineHash);
  if (it != rtPipeline.end() && it->second.d3d12.contentKey != contentKey) {
    if (it->second.d3d12.handle)
      it->second.d3d12.handle->Release();
    it->second.sbt.dispose(device);
    rtPipeline.erase(it);
    it = rtPipeline.end();
  }
  if (it == rtPipeline.end()) {
    RTPipelineSlot slot = {};
    createD3D12RayTracingPipeline(device, debugName, desc, slot);
    slot.d3d12.contentKey = contentKey;
    rtPipeline.emplace(pipelineHash, slot);
    it = rtPipeline.find(pipelineHash);
  }

  cmd->d3d12.cmdList7->SetPipelineState1(it->second.d3d12.handle);
  RID3D12_SetComputeRootSignature(*cmd, impl.d3d12.rootSignature);
  RID3D12_NoteBoundProgramRootArgs(*cmd, true, impl.d3d12.rootArgumentMask,
                                   debugName);
  // DispatchRays consumes the compute root arguments, so the descriptor and
  // push-constant paths must take their compute branch from here on.
  cmd->d3d12.computePipelineBound = true;
}

void RIProgram::traceD3D12Rays(struct RICmd *cmd, hash_t pipelineHash,
                               uint32_t width, uint32_t height,
                               uint32_t depth) {
  auto it = rtPipeline.find(pipelineHash);
  assert(it != rtPipeline.end() &&
         "traceRays called before bindRayTracingPipeline");
  if (it == rtPipeline.end())
    FatalError("RIProgram: traceRays called before bindRayTracingPipeline\n");
  if (!cmd->d3d12.cmdList7)
    FatalError("RIProgram: traceRays requires a DXR-capable command list\n");
  const auto &slot = it->second.d3d12;
  D3D12_DISPATCH_RAYS_DESC dispatch = {};
  dispatch.RayGenerationShaderRecord = slot.raygenRange;
  dispatch.MissShaderTable = slot.missRange;
  dispatch.HitGroupTable = slot.hitRange;
  dispatch.CallableShaderTable = slot.callableRange;
  dispatch.Width = width;
  dispatch.Height = height;
  dispatch.Depth = depth;
  cmd->d3d12.cmdList7->DispatchRays(&dispatch);
}

} // namespace hpl

#endif
