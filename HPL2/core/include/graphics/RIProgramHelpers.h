#ifndef HPL_RI_PROGRAM_HELPERS_H
#define HPL_RI_PROGRAM_HELPERS_H

#include "graphics/RIProgram.h"
#include "graphics/RITypes.h"
#include "resources/Resources.h"

#include <span>

namespace hpl {

// Load a single-stage Slang compute program. `name` is the artifact name with
// no extension; the active backend appends .spv or .dxil. `entryPoint` is the
// Slang function name (slangc runs with -fvk-use-entrypoint-name, so it
// survives into the SPIR-V as well as the DXIL).
void LoadSlangCompute(RIDevice *device, RIProgram &prog, cResources *resources,
                      const char *name, const char *entryPoint,
                      std::span<const RIBindlessLayout> externalLayouts = {});

// Load a Slang vert+frag program. `vertName == fragName` reuses one artifact
// for both stages; distinct names pick up separately compiled artifacts, as
// post-effects sharing one fullscreen vert with many frags do.
void LoadSlangGraphics(RIDevice *device, RIProgram &prog, cResources *resources,
                       const char *vertName, const char *fragName,
                       const char *vertEntryPoint = "vsMain",
                       const char *fragEntryPoint = "psMain",
                       std::span<const RIBindlessLayout> externalLayouts = {});

} // namespace hpl

#endif
