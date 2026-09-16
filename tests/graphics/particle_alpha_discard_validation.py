"""Lock the legacy particle alpha test into both particle fragment shaders.

The original engine's translucency_particle.frag ends with
``if(finalColor.a < 0.01) { discard; }``. That test is NOT an optimisation:
``applyBlendModulation`` (BlendModes.slang) scales ``.rgb`` only and leaves
``.a`` untouched for Add/Mul/MulX2, so for those three modes the discard is the
only consumer of texture alpha. It was missing from both particle shaders, and
retail textures that store their cutout in alpha (ps_glass_piece on Add,
ps_glass_shards on MulX2) therefore emitted their transparent-region RGB across
the whole quad and rendered as solid squares on the STUDY map.

Two layers of check here:

* a source assertion, which needs no toolchain and runs everywhere, and
* a numeric oracle over the blend math, which quantifies the objection the old
  code comment raised (that a 1% test would cut a contour through smoke and
  through the soft/lifetime fades) and shows it does not hold.

The SPIR-V side is covered by the production build, which compiles both shaders
and runs spirv-val; that is asserted by hand rather than re-run here.
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

# Blend-mode ids, mirroring amnesia/slang/Constants.h.
ADD, MUL, MULX2, ALPHA, PREMUL = range(5)
NO_ALPHA_IN_RGB = (ADD, MUL, MULX2)


def apply_blend_modulation(mode, rgb, a, opacity, light_level):
    """Pure-Python mirror of applyBlendModulation in BlendModes.slang."""
    opacity = min(max(opacity, 0.0), 1.0)
    a = min(max(a, 0.0), 1.0)
    if mode == ADD:
        rgb = [c * opacity * light_level for c in rgb]
    elif mode == MUL:
        rgb = [1.0 + (c - 1.0) * opacity for c in rgb]
    elif mode == MULX2:
        t = min(max(opacity * light_level, 0.0), 1.0)
        rgb = [0.5 + (c - 0.5) * t for c in rgb]
    elif mode == ALPHA:
        rgb = [c * light_level for c in rgb]
        a *= opacity
    elif mode == PREMUL:
        rgb = [c * opacity * light_level for c in rgb]
        a *= opacity
    return rgb, a


def composite(mode, rgb, a, dst):
    """Hardware blend from ParticlePipelineDesc.cpp, after the shader encode."""
    if mode == ADD:
        return [d + c for c, d in zip(rgb, dst)]
    if mode == MUL:
        return [d * c for c, d in zip(rgb, dst)]
    if mode == MULX2:
        return [2.0 * d * c for c, d in zip(rgb, dst)]
    # Alpha premultiplies in encodeBlendOutputLinear; PremulAlpha arrives
    # already premultiplied. Both then use ONE / ONE_MINUS_SRC_ALPHA.
    src = [c * a for c in rgb] if mode == ALPHA else list(rgb)
    return [s + d * (1.0 - a) for s, d in zip(src, dst)]


def draw(mode, rgb, a, opacity, light_level, dst, discard):
    rgb, a = apply_blend_modulation(mode, rgb, a, opacity, light_level)
    if discard and a < THRESHOLD_VALUE:
        return list(dst)
    return composite(mode, rgb, a, dst)


class ParticleAlphaDiscardSource(unittest.TestCase):
    def test_threshold_constant_is_shared_and_legacy_valued(self):
        text = CONSTANTS.read_text()
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

    def test_both_particle_shaders_discard_after_modulation(self):
        for shader in SHADERS:
            with self.subTest(shader=shader.name):
                text = shader.read_text()

                modulation = text.rindex("applyBlendModulation")
                guard = text.index(f"{THRESHOLD_NAME}", modulation)
                discard = text.index("discard", guard)
                returned = text.index("return", discard)

                # Ordering is the point: the discard has to see the
                # post-modulation alpha, exactly where the original had it.
                self.assertLess(modulation, guard)
                self.assertLess(guard, discard)
                self.assertLess(discard, returned)

    def test_retracted_rationale_is_gone(self):
        # Regression lock on the reasoning, not just the code. The old comment
        # asserted no discard was needed; that is what caused the bug.
        for shader in SHADERS:
            with self.subTest(shader=shader.name):
                self.assertNotIn("no discard", shader.read_text().lower())


class ParticleAlphaDiscardNumerics(unittest.TestCase):
    def test_transparent_texel_is_identity_for_non_alpha_modes(self):
        # The actual bug: a fully transparent texel whose RGB is not neutral.
        # ps_metal_piece.tga and ps_glass_shards.dds both look like this.
        dst = [0.25, 0.30, 0.35]
        for mode in NO_ALPHA_IN_RGB:
            with self.subTest(mode=mode):
                broken = draw(mode, [0.72, 0.72, 0.72], 0.0, 1.0, 1.0, dst, False)
                fixed = draw(mode, [0.72, 0.72, 0.72], 0.0, 1.0, 1.0, dst, True)
                self.assertNotAlmostEqual(
                    broken[0], dst[0], msg="without the discard the quad is visible"
                )
                for got, want in zip(fixed, dst):
                    self.assertAlmostEqual(got, want)

    def test_opacity_cannot_substitute_for_the_discard_on_mulx2(self):
        # MulX2's neutral is 0.5, so a kept-but-neutral texel only equals a
        # discarded one at opacity 0. Confirms a lerp-to-neutral workaround
        # is not a substitute.
        dst = [0.25, 0.30, 0.35]
        discarded = draw(MULX2, [0.72] * 3, 0.0, 1.0, 1.0, dst, True)
        for opacity in (0.25, 0.5, 0.75, 1.0):
            kept = draw(MULX2, [0.72] * 3, 0.0, opacity, 1.0, dst, False)
            self.assertNotAlmostEqual(kept[0], discarded[0], places=3)

    def test_discard_boundary_does_not_move_with_fades(self):
        # The retracted comment claimed the test cuts through the soft
        # intersection and lifetime fades. For Add/Mul/MulX2 the modulated
        # alpha is texture*vertex alpha and never sees opacity at all, so the
        # boundary is stationary in texture space.
        for mode in NO_ALPHA_IN_RGB:
            with self.subTest(mode=mode):
                alphas = {
                    apply_blend_modulation(mode, [0.5] * 3, 0.004, opacity, 1.0)[1]
                    for opacity in (0.0, 0.25, 0.5, 0.75, 1.0)
                }
                self.assertEqual(len(alphas), 1)

    def test_alpha_modes_error_stays_under_one_percent_of_a_layer(self):
        # For Alpha/PremulAlpha the discard IS near-a-no-op, and it tracks the
        # fade rather than cutting across it. Bound the worst-case difference.
        dst = [0.25, 0.30, 0.35]
        rgb = [0.8, 0.1, 0.1]
        for mode in (ALPHA, PREMUL):
            with self.subTest(mode=mode):
                worst = 0.0
                for step in range(0, 101):
                    opacity = step / 100.0
                    a, b = (
                        draw(mode, rgb, 1.0, opacity, 1.0, dst, d) for d in (False, True)
                    )
                    worst = max(worst, max(abs(x - y) for x, y in zip(a, b)))
                layer = max(abs(c - d) for c, d in zip(rgb, dst))
                self.assertLess(worst, THRESHOLD_VALUE * layer)


if __name__ == "__main__":
    unittest.main()
