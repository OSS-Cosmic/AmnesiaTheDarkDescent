"""Validate the shadow-cull compute shader against the host record layout.

The GPU kernel and the host reference (StandardShadowCull.cpp) read the same
storage buffers, so their struct offsets have to agree byte for byte. The C++
suite asserts the host side with offsetof; this asserts the device side by
reading the SPIR-V the production build actually produces, which is the only
check that catches a Slang-side layout drift on a machine with no GPU.

Skips rather than fails when slangc or spirv-tools are absent, matching how the
other optional toolchain checks in this repo behave.
"""

from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SLANG_ROOT = ROOT / "amnesia/slang"
SHADER = SLANG_ROOT / "Standard/Standard.cull.cs.slang"

# Mirrors premake/slang.lua:150-151. If the production flags change, this must
# change with them or the test stops validating what is shipped.
SLANG_FLAGS = [
    "-target", "spirv",
    "-profile", "sm_6_6",
    "-emit-spirv-directly",
    "-fvk-use-entrypoint-name",
    "-matrix-layout-column-major",
    "-fvk-use-scalar-layout",
]

# The single source of truth for the shared ABI, kept in the same order as
# amnesia/slang/StandardCull.h and asserted against C++ offsetof in
# tests/graphics/standard_shadow_cull.cpp.
EXPECTED_OFFSETS = {
    "StandardCullCandidate": {
        "aabbMinX": 0, "aabbMinY": 4, "aabbMinZ": 8, "objectSlot": 12,
        "aabbMaxX": 16, "aabbMaxY": 20, "aabbMaxZ": 24, "vertexCount": 28,
        "renderFlags": 32, "visibilityKey": 36, "cullFlags": 40,
        "commandWordOffset": 44,
    },
    "StandardCullTile": {
        "planes": 0, "candidateBase": 96, "candidateCount": 100,
        "indirectBase": 104, "countIndex": 108, "variabilityMask": 112,
        "planeCount": 116, "cameraIndex": 120,
    },
    "StandardCullGroup": {"tileIndex": 0, "candidateOffset": 4},
    # Camera occlusion parameters. The matrix is 16 floats at offset 0, so the
    # pyramid dimensions start at 64.
    "StandardCullCamera": {
        "viewProjection": 0, "hiZWidth": 64, "hiZHeight": 68,
        "hiZMipCount": 72,
    },
}

EXPECTED_ENTRY_POINTS = {"cullShadowResetCounts", "cullShadowTiles"}


def find_slangc():
    candidates = sorted((ROOT / "build-premake/_deps/slang-prebuilt").glob("*/bin/slangc"))
    return candidates[-1] if candidates else None


class StandardCullShaderValidation(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        slangc = find_slangc()
        if slangc is None:
            raise unittest.SkipTest("slangc not present; run a build first")
        cls.temp = tempfile.TemporaryDirectory()
        cls.spv = Path(cls.temp.name) / "Standard.cull.cs.spv"
        result = subprocess.run(
            [str(slangc), str(SHADER), *SLANG_FLAGS, "-I", str(SLANG_ROOT), "-o", str(cls.spv)],
            capture_output=True, text=True,
        )
        if result.returncode != 0:
            raise AssertionError(f"slangc failed:\n{result.stdout}\n{result.stderr}")
        cls.disassembly = cls._disassemble()

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    @classmethod
    def _disassemble(cls):
        tool = shutil.which("spirv-dis")
        if tool is None:
            return None
        result = subprocess.run([tool, str(cls.spv)], capture_output=True, text=True)
        return result.stdout if result.returncode == 0 else None

    def test_spirv_is_valid_under_scalar_block_layout(self):
        tool = shutil.which("spirv-val")
        if tool is None:
            self.skipTest("spirv-val not installed")
        result = subprocess.run(
            [tool, "--scalar-block-layout", str(self.spv)], capture_output=True, text=True
        )
        self.assertEqual(result.returncode, 0, f"spirv-val:\n{result.stdout}\n{result.stderr}")

    def test_entry_points_and_workgroup_size(self):
        if self.disassembly is None:
            self.skipTest("spirv-dis not installed")
        found = set(re.findall(r'OpEntryPoint GLCompute %\w+ "(\w+)"', self.disassembly))
        self.assertEqual(found, EXPECTED_ENTRY_POINTS)

        # The host derives its dispatch size from this, and the flattened
        # (tile, candidate) work list assumes chunks of exactly this width.
        for entry in EXPECTED_ENTRY_POINTS:
            self.assertRegex(
                self.disassembly,
                rf"OpExecutionMode %{entry} LocalSize 64 1 1",
                f"{entry} must stay at 64 threads to match kStandardCullGroupSize",
            )

    def test_struct_offsets_match_the_host(self):
        if self.disassembly is None:
            self.skipTest("spirv-dis not installed")
        for struct, expected in EXPECTED_OFFSETS.items():
            names = dict(
                (int(index), name)
                for index, name in re.findall(
                    rf"OpMemberName %{struct}\w* (\d+) \"(\w+)\"", self.disassembly
                )
            )
            offsets = dict(
                (int(index), int(offset))
                for index, offset in re.findall(
                    rf"OpMemberDecorate %{struct}\w* (\d+) Offset (\d+)", self.disassembly
                )
            )
            self.assertTrue(names, f"{struct} missing from the SPIR-V")
            actual = dict((names[i], offsets[i]) for i in offsets if i in names)
            for field, offset in expected.items():
                self.assertIn(field, actual, f"{struct}.{field} missing on the device side")
                self.assertEqual(
                    actual[field], offset,
                    f"{struct}.{field} is at {actual[field]} on the GPU but {offset} on the host",
                )


if __name__ == "__main__":
    unittest.main()
