#pragma once

// The per-world fog-area binding is shared by Fog.slang consumers. Keeping it
// separate from bindless.slang lets raster-only passes avoid importing the RT
// declarations that are needed by material/path-tracing consumers.
#include "Constants.h"
import SceneTypes;

[vk::binding(kBindingWorldFogAreas, kWorldSet)]
public RWStructuredBuffer<FogAreaParams> gFogAreas;
