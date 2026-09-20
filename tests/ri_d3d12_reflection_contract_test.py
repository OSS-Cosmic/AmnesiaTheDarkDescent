"""Contract tests for the DXIL artifact + Slang reflection-v1 envelope."""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path


MAGIC = "HPL2_SHADER_ARTIFACT"


def read_artifact(root: Path, name: str, entry: str, stage: str) -> dict:
    """Load the entry-specific reflection document associated with an artifact."""
    artifact = root / name
    metadata_path = artifact.with_name(artifact.name + ".meta")
    if not metadata_path.is_file() or not metadata_path.read_text(encoding="utf-8").strip():
        raise ValueError("missing or empty metadata")

    lines = metadata_path.read_text(encoding="utf-8").splitlines()
    fields = dict(line.split("=", 1) for line in lines[1:] if "=" in line)
    if lines[:3] != [MAGIC, "version=1", "format=dxil"]:
        raise ValueError("malformed metadata")
    reflection_name = fields.get("reflection")
    if not reflection_name:
        raise ValueError("missing reflection linkage")
    if fields.get("entry") != f"{stage}:{entry}":
        raise ValueError("metadata entry does not match request")

    reflection_path = root / reflection_name
    if not reflection_path.is_file() or not reflection_path.read_text(encoding="utf-8").strip():
        raise ValueError("missing or empty reflection")
    try:
        document = json.loads(reflection_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise ValueError("malformed reflection") from exc

    entries = document.get("entryPoints")
    if not isinstance(entries, list):
        raise ValueError("reflection has no entry points")
    matches = [item for item in entries if item.get("name") == entry and item.get("stage") == stage]
    if len(matches) != 1:
        raise ValueError("entry-specific reflection is missing or ambiguous")
    return matches[0]


def write_artifact(root: Path, name: str, reflection_name: str, entry: str, stage: str, reflection: dict, blob: bytes) -> None:
    artifact = root / name
    artifact.write_bytes(blob)
    (root / (name + ".meta")).write_text(
        f"{MAGIC}\nversion=1\nformat=dxil\nsource=fixture.slang\n"
        f"stage={stage}\nentry={stage}:{entry}\nreflection={reflection_name}\n",
        encoding="utf-8",
    )
    (root / reflection_name).write_text(json.dumps(reflection), encoding="utf-8")


class D3D12ReflectionArtifactContractTests(unittest.TestCase):
    def test_cpp_parser_preserves_v1_binding_and_layout_contract(self):
        source = (Path(__file__).parents[1] / "HPL2/core/sources/graphics/RIProgram.cpp").read_text(encoding="utf-8")
        self.assertIn('ri_member(binding, "format")', source)
        self.assertIn('std::string_view(v->GetString()) == "unbounded"', source)
        self.assertIn('if (v.IsArray())', source)
        self.assertIn('std::string_view(name->GetString()) == "gPushConstants"', source)
        self.assertIn('ri_find_push_constant(*parameters, result->pushConstants)', source)
        self.assertIn('ri_parseShaderReflection(*bin->reflection->json,', source)
        self.assertIn('ri_stageName(init.stage)', source)
        self.assertIn('!cPlatform::CopyFileToBuffer(sPath', source)
        self.assertNotIn('ri_parseShaderReflection(reflectionJson, nullptr)', source)
        self.assertIn(
            'std::make_shared<const std::string>(std::move(reflectionJson))', source
        )

    def test_identical_blobs_keep_distinct_entry_and_reflection_selection(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            reflection = {
                "entryPoints": [
                    {"name": "VSMain", "stage": "vertex", "bindings": [{"name": "vs", "binding": {"kind": "constantBuffer", "index": 2}}]},
                    {"name": "PSMain", "stage": "fragment", "bindings": [{"name": "ps", "binding": {"kind": "shaderResource", "index": 7}}]},
                ]
            }
            blob = b"DXBC" + b"same-bytecode"
            write_artifact(root, "triangle.vert.dxil", "triangle.vert.reflection.json", "VSMain", "vertex", reflection, blob)
            write_artifact(root, "triangle.frag.dxil", "triangle.frag.reflection.json", "PSMain", "fragment", reflection, blob)

            vertex = read_artifact(root, "triangle.vert.dxil", "VSMain", "vertex")
            fragment = read_artifact(root, "triangle.frag.dxil", "PSMain", "fragment")
            self.assertEqual(vertex["bindings"][0]["binding"]["kind"], "constantBuffer")
            self.assertEqual(fragment["bindings"][0]["binding"]["kind"], "shaderResource")
            self.assertEqual((root / "triangle.vert.dxil").read_bytes(), (root / "triangle.frag.dxil").read_bytes())

    def test_missing_empty_and_malformed_metadata_or_reflection_fail_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            write_artifact(root, "shader.dxil", "shader.reflection.json", "CSMain", "compute", {"entryPoints": []}, b"DXBC")
            metadata = root / "shader.dxil.meta"
            reflection = root / "shader.reflection.json"

            metadata.unlink()
            with self.assertRaisesRegex(ValueError, "metadata"):
                read_artifact(root, "shader.dxil", "CSMain", "compute")
            write_artifact(root, "shader.dxil", "shader.reflection.json", "CSMain", "compute", {"entryPoints": []}, b"DXBC")
            reflection.write_text("", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "reflection"):
                read_artifact(root, "shader.dxil", "CSMain", "compute")
            reflection.write_text("{not-json", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "malformed reflection"):
                read_artifact(root, "shader.dxil", "CSMain", "compute")
            metadata.write_text("HPL2_SHADER_ARTIFACT\nversion=1\nformat=dxil\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "linkage"):
                read_artifact(root, "shader.dxil", "CSMain", "compute")

    def test_reflection_retains_register_layout_types_arrays_push_constants_format_and_stage(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            entry = {
                "name": "CSMain",
                "stage": "compute",
                "bindings": [
                    {"name": "gTextures", "binding": {"kind": "shaderResource", "index": 3, "space": 2, "count": 4}, "type": {"kind": "resource", "baseShape": "texture2D"}, "format": "rgba16f"},
                    {"name": "gConstants", "binding": {"kind": "constantBuffer", "index": 1, "space": 4}, "type": {"kind": "constantBuffer", "elementType": {"kind": "struct", "name": "Constants"}, "elementStride": 64}},
                ],
                "pushConstants": [{"name": "Push", "binding": {"kind": "uniform", "offset": 0, "size": 32, "elementStride": 32}, "type": {"kind": "struct", "name": "Push"}}],
            }
            write_artifact(root, "compute.dxil", "compute.reflection.json", "CSMain", "compute", {"entryPoints": [entry]}, b"DXBC")
            selected = read_artifact(root, "compute.dxil", "CSMain", "compute")
            texture, constants = selected["bindings"]
            self.assertEqual(selected["stage"], "compute")
            self.assertEqual(texture["binding"], {"kind": "shaderResource", "index": 3, "space": 2, "count": 4})
            self.assertEqual(texture["format"], "rgba16f")
            self.assertEqual(constants["type"]["elementStride"], 64)
            self.assertEqual(selected["pushConstants"][0]["binding"]["size"], 32)


if __name__ == "__main__":
    unittest.main()
