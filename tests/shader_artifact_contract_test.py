"""Focused tests for the shader artifact loader/build contract."""

from pathlib import Path
import tempfile
import unittest
from unittest import mock

from tests import shader_inventory_validation as inventory


ROOT = Path(__file__).resolve().parents[1]
RI_PROGRAM = ROOT / "HPL2/core/sources/graphics/RIProgram.cpp"
RI_PROGRAM_HEADER = ROOT / "HPL2/core/include/graphics/RIProgram.h"
GRAPHICS = ROOT / "HPL2/core/sources/graphics/Graphics.cpp"
SLANG_RULES = ROOT / "premake/slang.lua"


class ShaderArtifactContractTests(unittest.TestCase):
    def test_missing_stale_and_mismatched_metadata_fail_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            artifact = Path(directory) / "sample.spv"
            artifact.write_bytes(b"\x03\x02\x23\x07")

            inventory.validate_metadata(artifact)

            metadata = artifact.with_name(artifact.name + ".meta")
            metadata.write_text(
                "HPL2_SHADER_ARTIFACT\nversion=0\nformat=spirv\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ValueError, "stale or mismatched"):
                inventory.validate_metadata(artifact)

            metadata.write_text(
                "HPL2_SHADER_ARTIFACT\nversion=1\nformat=dxil\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ValueError, "stale or mismatched"):
                inventory.validate_metadata(artifact)

            metadata.write_text(
                "HPL2_SHADER_ARTIFACT\nversion=1\nformat=spirv\n",
                encoding="utf-8",
            )
            artifact.write_bytes(b"DXBC")
            with self.assertRaisesRegex(ValueError, "wrong magic"):
                inventory.validate_metadata(artifact)

    def test_inventory_keeps_non_main_and_multi_entry_sources(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "lighting.3d.slang"
            source.write_text(
                "float4 collectVertex() { return 0; }\n"
                "float4 shadePixel() { return 0; }\n",
                encoding="utf-8",
            )
            # The inventory's recognized entry names intentionally reflect the
            # production shader vocabulary, rather than assuming `main`.
            source.write_text(
                "float4 vsMain() { return 0; }\n"
                "float4 psMain() { return 0; }\n",
                encoding="utf-8",
            )
            self.assertEqual(
                inventory.inventory(root),
                [("lighting.3d.slang", "multi-raster", ["vsMain", "psMain"])],
            )

    def test_loader_resolves_backend_artifact_names_and_validates_sidecars(self):
        source = RI_PROGRAM.read_text(encoding="utf-8")
        # Callers pass a backend-neutral name; the active backend supplies the
        # extension. No filename surgery, and no directory scan to undo it.
        self.assertIn('tString artifactName = asName + (d3d12 ? ".dxil" : ".spv")', source)
        self.assertNotIn("ri_resolveD3D12Artifact", source)
        # A multi-entry D3D12 source also emits per-entry executables beside
        # the library; those must win when an entry point was requested.
        self.assertIn('const tString perEntry = asName + "." + entryPoint + ".dxil"', source)
        self.assertIn("ri_readShaderMetadata(searcher, artifactName, result.size()", source)
        self.assertIn("format does not match active backend", source)
        self.assertIn("artifact bytes do not match metadata", source)

    def test_d3d12_runtime_indexes_backend_specific_shader_directories(self):
        source = GRAPHICS.read_text(encoding="utf-8")
        rules = SLANG_RULES.read_text(encoding="utf-8")
        self.assertIn('AddResourceDir(_W("compiled_shaders/vk"), false)', source)
        self.assertIn('AddResourceDir(_W("compiled_shaders/d3d12"), false)', source)
        self.assertNotIn('AddResourceDir(_W("core/shaders/d3d12"), false)', source)
        self.assertNotIn("core\\\\shaders\\\\d3d12", rules)

    def test_vulkan_accepts_raw_spirv_and_honors_non_main_entry_names(self):
        source = RI_PROGRAM.read_text(encoding="utf-8")
        header = RI_PROGRAM_HEADER.read_text(encoding="utf-8")
        self.assertIn("ri_detectShaderFormat(std::span<const char>(init.data.data(), init.data.size()))", source)
        self.assertIn("RIShaderArtifactFormat::Unknown", header)
        self.assertIn('bin->entryPoint = init.entryPoint', source)
        self.assertIn('spvReflectCreateShaderModule(', source)
        self.assertIn("-fvk-use-entrypoint-name", SLANG_RULES.read_text(encoding="utf-8"))

    def test_inventory_artifact_validation_checks_sidecar_before_disassembly(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "src"
            artifacts = Path(directory) / "compiled"
            root.mkdir()
            artifacts.mkdir()
            (root / "sample.comp.slang").write_text("void csMain() {}\n", encoding="utf-8")
            artifact = artifacts / "sample.comp.spv"
            artifact.write_bytes(b"\x03\x02\x23\x07")
            # A sidecar copied from the DXIL build must fail before the tool is
            # consulted; this keeps stale metadata from reaching a backend.
            artifact.with_name(artifact.name + ".meta").write_text(
                "HPL2_SHADER_ARTIFACT\nversion=1\nformat=dxil\n", encoding="utf-8"
            )
            with mock.patch.object(inventory, "spirv_entries") as disassemble:
                with self.assertRaisesRegex(ValueError, "stale or mismatched"):
                    inventory.validate_artifacts(root, artifacts, Path("unused"))
                disassemble.assert_not_called()


if __name__ == "__main__":
    unittest.main()
