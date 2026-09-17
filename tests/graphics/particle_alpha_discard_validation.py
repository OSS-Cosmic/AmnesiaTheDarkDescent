"""Lock the legacy particle alpha test into both particle fragment shaders.

The original engine's translucency_particle.frag ends with
``if(finalColor.a < 0.01) { discard; }``. That test is NOT an optimisation:
``applyBlendModulation`` (BlendModes.slang) scales ``.rgb`` only and leaves
``.a`` untouched for Add/Mul/MulX2, so for those three modes the discard is the
only consumer of texture alpha. It was missing from both particle shaders, and
retail textures that store their cutout in alpha (ps_glass_piece on Add,
ps_glass_shards on MulX2) therefore emitted their transparent-region RGB across
the whole quad and rendered as solid squares on the STUDY map.

These source checks guard the shared threshold and the conditional discard
on the result of blend modulation. They do not execute the GPU shader or
validate the blend equations.
"""

from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
SLANG_ROOT = ROOT / "amnesia/slang"

CONSTANTS = SLANG_ROOT / "Constants.h"
SHADERS = (
    SLANG_ROOT / "Standard/Standard.particle.3d.slang",
    SLANG_ROOT / "Particle/Particle.frag.slang",
)

THRESHOLD_NAME = "kParticleAlphaDiscard"
THRESHOLD_VALUE = 0.01


def without_comments(text):
    return re.sub(r"//[^\n]*|/\*.*?\*/", " ", text, flags=re.DOTALL)


class ParticleAlphaDiscardSource(unittest.TestCase):
    def test_threshold_constant_is_shared_and_legacy_valued(self):
        text = without_comments(CONSTANTS.read_text())
        match = re.search(
            rf"SHARED_CONST\s+float\s+{THRESHOLD_NAME}\s*=\s*([0-9.e-]+)f?\s*;", text
        )
        self.assertIsNotNone(
            match, f"{THRESHOLD_NAME} must live in Constants.h so both shaders share it"
        )
        self.assertAlmostEqual(
            float(match.group(1)),
            THRESHOLD_VALUE,
            msg="threshold must stay at the legacy translucency_particle.frag 0.01",
        )

    def test_both_particle_shaders_discard_low_modulated_alpha(self):
        # Match the actual assignment and conditional statement, ignoring
        # comments and allowing either braced or unbraced discard bodies.
        pattern = (
            r"\bfloat4\s+(?P<color>\w+)\s*=\s*applyBlendModulation\s*\([^;]+\)\s*;"
            r"\s*if\s*\(\s*(?P=color)\s*\.\s*a\s*<\s*"
            + THRESHOLD_NAME
            + r"\s*\)\s*(?:discard\s*;|\{\s*discard\s*;\s*\})\s*return\b"
        )
        for shader in SHADERS:
            with self.subTest(shader=shader.name):
                self.assertRegex(
                    without_comments(shader.read_text()),
                    pattern,
                    "discard must test post-modulation alpha < the shared threshold before return",
                )


if __name__ == "__main__":
    unittest.main()
