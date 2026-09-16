"""Lock the GPU particle shader <-> C++ descriptor-binding contract.

cGpuParticlePass builds its descriptor bindings by REFLECTED NAME
(RIProgram::bindDescriptors matches on a hash of the name). A typo or a rename
on either side therefore does not fail the build -- it produces a binding that
is never written, which in a release build means the shader reads zeros. That
failure mode is silent and looks like "particles just don't appear", so it is
worth a test.

This parses the compiled SPIR-V directly rather than shelling out to spirv-dis,
so it runs anywhere the build has produced .spv files.
"""

import struct
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]

# Search order matters: the runtime loads out of the Release directory, and a
# stale Debug build is a known way to test the wrong artifact.
SHADER_DIRS = (
    REPO_ROOT / "build-premake" / "amnesia" / "Release" / "compiled_shaders",
    REPO_ROOT / "build-premake" / "amnesia" / "Debug" / "compiled_shaders",
)

SPIRV_MAGIC = 0x07230203
OP_NAME = 5
OP_DECORATE = 71
DECORATION_BINDING = 33
DECORATION_DESCRIPTOR_SET = 34

# The names cGpuParticlePass passes to RIProgram::DescriptorBinding, and the
# (set, binding) each shader declares. Set 2 is the per-pass set by convention;
# set 0 is the engine bindless table and set 1 is gPerFrame.
EXPECTED = {
    "Standard.particleSpawn.cs.spv": {
        "gParticleState": (2, 0),
        "gParticleEmitters": (2, 1),
        "gParticleEmitterParams": (2, 2),
        "gParticleSpawnCmds": (2, 3),
    },
    "Standard.particleSim.cs.spv": {
        "gParticleState": (2, 0),
        "gParticleEmitters": (2, 1),
        "gParticleEmitterParams": (2, 2),
        "gParticleGroups": (2, 3),
        "gParticleCollisionDepth": (2, 4),
    },
    "Standard.particleExpand.cs.spv": {
        "gParticleState": (2, 0),
        "gParticleEmitters": (2, 1),
        "gParticleEmitterParams": (2, 2),
        "gParticleGroups": (2, 3),
        "gParticleSliceOffset": (2, 4),
        "gParticleQuadVerts": (2, 5),
    },
}


def find_shader(name):
    for directory in SHADER_DIRS:
        candidate = directory / name
        if candidate.is_file():
            return candidate
    return None


def parse_spirv(path):
    """Return {name: (set, binding)} for every decorated interface variable."""
    data = path.read_bytes()
    magic = struct.unpack("<I", data[:4])[0]
    if magic != SPIRV_MAGIC:
        raise AssertionError(f"{path} is not little-endian SPIR-V")

    words = struct.unpack(f"<{len(data) // 4}I", data[: (len(data) // 4) * 4])

    names = {}
    sets = {}
    bindings = {}

    index = 5  # skip the five-word header
    while index < len(words):
        instruction = words[index]
        opcode = instruction & 0xFFFF
        length = instruction >> 16
        if length == 0:
            break

        if opcode == OP_NAME:
            target = words[index + 1]
            raw = b"".join(
                struct.pack("<I", w) for w in words[index + 2 : index + length]
            )
            names[target] = raw.split(b"\0", 1)[0].decode("utf-8", "replace")
        elif opcode == OP_DECORATE and length >= 4:
            target = words[index + 1]
            decoration = words[index + 2]
            if decoration == DECORATION_BINDING:
                bindings[target] = words[index + 3]
            elif decoration == DECORATION_DESCRIPTOR_SET:
                sets[target] = words[index + 3]

        index += length

    return {
        names[target]: (sets.get(target), bindings[target])
        for target in bindings
        if target in names and target in sets
    }


class GpuParticleBindingsTest(unittest.TestCase):
    def test_every_shader_declares_the_expected_bindings(self):
        checked = 0
        for shader_name, expected in EXPECTED.items():
            path = find_shader(shader_name)
            if path is None:
                # The build has not produced shaders here; other assertions in
                # this suite still have to run, so record and continue.
                continue
            checked += 1
            actual = parse_spirv(path)
            for binding_name, location in expected.items():
                self.assertIn(
                    binding_name,
                    actual,
                    f"{shader_name} does not declare {binding_name}; "
                    "cGpuParticlePass binds by reflected name, so a mismatch "
                    "silently leaves the descriptor unwritten",
                )
                self.assertEqual(
                    location,
                    actual[binding_name],
                    f"{shader_name}:{binding_name} moved to set/binding "
                    f"{actual[binding_name]}, expected {location}",
                )

        self.assertGreater(
            checked,
            0,
            "no compiled particle shaders found in "
            + ", ".join(str(d) for d in SHADER_DIRS)
            + " -- build first, or this test proves nothing",
        )

    def test_no_shader_declares_an_unexpected_particle_binding(self):
        # Catches the reverse drift: a binding added to a shader that no
        # backend writes. Anything in set 2 must be accounted for above.
        for shader_name, expected in EXPECTED.items():
            path = find_shader(shader_name)
            if path is None:
                continue
            actual = parse_spirv(path)
            for binding_name, (set_index, binding_index) in actual.items():
                if set_index != 2:
                    continue
                self.assertIn(
                    binding_name,
                    expected,
                    f"{shader_name} declares {binding_name} at set 2 binding "
                    f"{binding_index}, which cGpuParticlePass never writes",
                )


if __name__ == "__main__":
    unittest.main()
