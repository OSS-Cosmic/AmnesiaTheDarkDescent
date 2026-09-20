#include "graphics/RIProgramHelpers.h"

#include <array>

namespace hpl {

void LoadSlangCompute(RIDevice *device, RIProgram &prog,
                      cResources *resources, const char *name,
                      const char *entryPoint,
                      std::span<const RIBindlessLayout> externalLayouts) {
  auto bin = RIProgram::loadShaderStage(resources->GetFileSearcher(), name,
                                        entryPoint);
  std::array<RIProgram::ModuleStage, 1> stages = {RIProgram::ModuleStage{
      RIProgram::PROGRAM_STAGE_COMPUTE, bin, entryPoint}};
  prog.initialize(device, stages, externalLayouts, name);
}

void LoadSlangGraphics(RIDevice *device, RIProgram &prog,
                       cResources *resources, const char *vertName,
                       const char *fragName, const char *vertEntryPoint,
                       const char *fragEntryPoint,
                       std::span<const RIBindlessLayout> externalLayouts) {
  if (vertName && fragName && std::string_view(vertName) == fragName) {
    auto vsBin = RIProgram::loadShaderStage(resources->GetFileSearcher(), vertName,
                                            vertEntryPoint);
    auto fsBin = RIProgram::loadShaderStage(resources->GetFileSearcher(), fragName,
                                            fragEntryPoint);
    std::array<RIProgram::ModuleStage, 2> stages = {
        RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, vsBin,
                               vertEntryPoint},
        RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, fsBin,
                               fragEntryPoint}};
    prog.initialize(device, stages, externalLayouts, fragName);
    return;
  }
  auto vsBin = RIProgram::loadShaderStage(resources->GetFileSearcher(), vertName,
                                          vertEntryPoint);
  auto fsBin = RIProgram::loadShaderStage(resources->GetFileSearcher(), fragName,
                                          fragEntryPoint);
  std::array<RIProgram::ModuleStage, 2> stages = {
      RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, vsBin,
                             vertEntryPoint},
      RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, fsBin,
                             fragEntryPoint}};
  prog.initialize(device, stages, externalLayouts, fragName);
}

} // namespace hpl
