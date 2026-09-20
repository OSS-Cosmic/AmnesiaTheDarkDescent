#ifndef RI_SHADER_ARTIFACT_H
#define RI_SHADER_ARTIFACT_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hpl {

// Metadata is a text sidecar rather than an extension convention, so an
// artifact copied or renamed incorrectly fails at the loading boundary instead
// of reaching a backend with the wrong bytecode.
static constexpr const char RI_SHADER_ARTIFACT_MAGIC[] =
    "HPL2_SHADER_ARTIFACT";
static constexpr uint32_t RI_SHADER_ARTIFACT_VERSION = 1;

// Line-oriented envelope keys. Kept in the public contract so producers and
// ModuleStage consumers share one spelling instead of inferring from filenames.
static constexpr const char RI_SHADER_ARTIFACT_SOURCE_KEY[] = "source";
static constexpr const char RI_SHADER_ARTIFACT_STAGE_KEY[] = "stage";
static constexpr const char RI_SHADER_ARTIFACT_ENTRY_KEY[] = "entry";
static constexpr const char RI_SHADER_ARTIFACT_FORMAT_KEY[] = "format";

// The sidecar is only an envelope; the Slang reflection document it points at
// is authoritative for resources, registers, spaces, layouts and semantics.
static constexpr const char RI_SHADER_ARTIFACT_REFLECTION_KEY[] =
    "reflection";

// Slang reflection-v1 names consumed by the D3D12 ModuleStage path. These are
// schema names, not backend handles: register/space, array cardinality,
// resource shape/format, layout stride/size, and entry-point stage remain
// valid even when the artifact is loaded on a different backend.
static constexpr const char RI_SHADER_REFLECTION_ENTRY_POINTS_KEY[] =
    "entryPoints";
static constexpr const char RI_SHADER_REFLECTION_BINDINGS_KEY[] = "bindings";
static constexpr const char RI_SHADER_REFLECTION_PUSH_CONSTANTS_KEY[] =
    "pushConstants";
static constexpr const char RI_SHADER_REFLECTION_UNBOUNDED[] = "unbounded";

enum class RIShaderArtifactFormat : uint8_t {
  Unknown = 0,
  Spirv = 1,
  Dxil = 2,
};

// Sidecar value marking a multi-entry DXIL library rather than a single-stage
// artifact. Slang compiles any source with more than one [shader("...")] entry
// as lib_6_8, and its reflection document omits "stage" on ray-tracing entry
// points -- so for those the sidecar's entry map is the only stage record.
static constexpr const char RI_SHADER_ARTIFACT_STAGE_LIBRARY[] = "library";

// One `entry=` element: the stage a given entry point was declared with.
struct RIShaderArtifactEntry {
  std::string stage; // normalized; see ri_normalizeShaderStageName
  std::string entry;
};

// Parsed form of the sidecar's stage/entry envelope.
struct RIShaderArtifactMeta {
  std::string stage; // verbatim sidecar value ("compute", "library", ...)
  std::vector<RIShaderArtifactEntry> entries;

  bool isLibrary() const { return stage == RI_SHADER_ARTIFACT_STAGE_LIBRARY; }

  // The stage declared for `entryName`, or nullptr when absent.
  const char *stageForEntry(std::string_view entryName) const {
    for (const auto &item : entries)
      if (item.entry == entryName) return item.stage.c_str();
    return nullptr;
  }
};

// Maps a producer-side stage spelling onto the engine's internal stage names.
// The returned alphabet must stay identical to ri_stageName()'s table in
// RIProgram.cpp -- that function is the consumer of every value produced here.
// Returns nullptr for an unrecognized stage so callers can fail loudly.
inline const char *ri_normalizeShaderStageName(std::string_view stage) {
  // Slang spells these differently from the engine's ProgramStages names.
  if (stage == "raygeneration") return "raygen";
  if (stage == "pixel") return "fragment";
  for (const char *known :
       {"vertex", "fragment", "compute", "raygen", "miss", "closesthit",
        "anyhit", "intersection", "callable"})
    if (stage == known) return known;
  return nullptr;
}

// Parses the sidecar's `entry=` field into `out.entries`.
//
// Two producer spellings are accepted, both emitted by premake/slang.lua:
//   "miss:ptMiss,raygeneration:rayGen"  -- production, one pair per entry
//   "csMain"                            -- test fixtures, stage comes from
//                                          `stageField` instead
// Returns false when a token is malformed or names an unknown stage; the
// caller is expected to treat that as a fatal artifact error.
inline bool ri_parseShaderArtifactEntries(std::string_view entryField,
                                          std::string_view stageField,
                                          RIShaderArtifactMeta &out) {
  out.stage.assign(stageField.begin(), stageField.end());
  out.entries.clear();
  size_t pos = 0;
  while (pos <= entryField.size()) {
    const size_t comma = entryField.find(',', pos);
    std::string_view token = entryField.substr(
        pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos);
    // A token carries its own stage only in the "stage:entry" form; otherwise
    // the artifact is single-stage and `stage=` applies.
    const size_t colon = token.find(':');
    std::string_view stage =
        colon == std::string_view::npos ? stageField : token.substr(0, colon);
    std::string_view name =
        colon == std::string_view::npos ? token : token.substr(colon + 1);
    if (!name.empty()) {
      const char *normalized = ri_normalizeShaderStageName(stage);
      if (!normalized) return false;
      out.entries.push_back({normalized, std::string(name)});
    }
    if (comma == std::string_view::npos) break;
    pos = comma + 1;
  }
  return !out.entries.empty();
}

} // namespace hpl

#endif
