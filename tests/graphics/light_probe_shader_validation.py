"""Compile both probe consumers and validate their host/GPU buffer contracts."""

from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SHADERS = ROOT / "amnesia/slang"


class LightProbeShaderValidation(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compilers = sorted(
            (ROOT / "build-premake/_deps/slang-prebuilt").glob("*/bin/slangc*")
        )
        if not compilers:
            raise unittest.SkipTest("slangc not present; run a build first")
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.outputs = {}
        for product in ("tdd", "amfp"):
            for shader in (
                "Probe/LightProbePass.cs",
                "Probe/TranslucentLightProbe.cs",
                "Translucent/Translucent.frag",
            ):
                output = Path(cls.temp.name) / f"{product}-{Path(shader).name}.spv"
                command = [
                    str(compilers[-1]), str(SHADERS / f"{shader}.slang"),
                    "-target", "spirv", "-profile", "sm_6_6",
                    "-emit-spirv-directly", "-fvk-use-entrypoint-name",
                    "-matrix-layout-column-major", "-fvk-use-scalar-layout",
                    "-I", str(SHADERS), "-o", str(output),
                ]
                if product == "amfp":
                    command.append("-DAMFP")
                result = subprocess.run(command, capture_output=True, text=True)
                if result.returncode:
                    raise AssertionError(f"{product} {shader}:\n{result.stderr}")
                cls.outputs[product, shader] = output

    def disassemble(self, output):
        tool = shutil.which("spirv-dis")
        if not tool:
            self.skipTest("spirv-dis not installed")
        return subprocess.check_output([tool, str(output)], text=True)

    def assert_offsets(self, text, struct, expected):
        names = dict(
            re.findall(rf'OpMemberName %{struct}\w* (\d+) "(\w+)"', text)
        )
        offsets = dict(
            re.findall(rf"OpMemberDecorate %{struct}\w* (\d+) Offset (\d+)", text)
        )
        actual = {names[index]: int(offset) for index, offset in offsets.items()}
        self.assertEqual(actual, expected)

    def test_spirv_validation(self):
        tool = shutil.which("spirv-val")
        if not tool:
            self.skipTest("spirv-val not installed")
        for key, output in self.outputs.items():
            with self.subTest(shader=key):
                result = subprocess.run(
                    [tool, "--scalar-block-layout", str(output)],
                    capture_output=True, text=True,
                )
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_probe_dispatch_and_buffer_layout(self):
        for key, output in self.outputs.items():
            if not key[1].startswith("Probe/"):
                continue
            with self.subTest(shader=key):
                text = self.disassemble(output)
                self.assertIn("OpExecutionMode %csMain LocalSize 64 1 1", text)
                self.assert_offsets(text, "LightProbeRequest", {"posW": 0, "padding": 12})
                self.assert_offsets(text, "LightProbeResult", {"irradiance": 0, "padding": 12})
                for record in ("LightProbeRequest", "LightProbeResult"):
                    self.assertRegex(text, rf"OpDecorate %_runtimearr_{record}\w* ArrayStride 16")
                for binding, name in enumerate(("gProbeRequests", "gProbeResults")):
                    self.assertIn(f"OpDecorate %{name} Binding {binding}", text)
                    self.assertIn(f"OpDecorate %{name} DescriptorSet 2", text)
                # Upload heaps may only be read, whereas results must be writable.
                self.assertIn("OpDecorate %gProbeRequests NonWritable", text)
                self.assertNotIn("OpDecorate %gProbeResults NonWritable", text)
                if "Translucent" in key[1]:
                    self.assert_offsets(text, "TranslucentProbePC", {"probeBase": 0, "probeCount": 4})
                else:
                    self.assert_offsets(text, "LightProbePC", {
                        "probeCount": 0, "excludeCount": 4, "excludeIds": 8,
                    })

    def test_fragment_result_binding_and_push_layout(self):
        for product in ("tdd", "amfp"):
            with self.subTest(product=product):
                text = self.disassemble(self.outputs[product, "Translucent/Translucent.frag"])
                self.assert_offsets(text, "TranslucentPushConstants", {
                    "blendMode": 0, "sceneAlpha": 4, "options": 8, "lightProbeIndex": 12,
                })
                self.assert_offsets(text, "LightProbeResult", {"irradiance": 0, "padding": 12})
                for decoration in ("Binding 3", "DescriptorSet 2", "NonWritable"):
                    self.assertIn(f"OpDecorate %gTranslucentProbeResults {decoration}", text)


if __name__ == "__main__":
    unittest.main()
