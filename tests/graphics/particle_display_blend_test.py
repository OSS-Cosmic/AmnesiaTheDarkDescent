"""Reference arithmetic and integration contracts; not a GPU rendering test."""
from pathlib import Path
import re
import struct
import unittest

ROOT = Path(__file__).resolve().parents[2]


def encode(x):
    return 12.92 * x if x <= 0.0031308 else 1.055 * x ** (1 / 2.4) - 0.055


def decode(x):
    return x / 12.92 if x <= 0.04045 else ((x + 0.055) / 1.055) ** 2.4


def half(x):
    return struct.unpack('e', struct.pack('e', x))[0]


def source(path):
    return re.sub(r'//[^\n]*|/\*.*?\*/', '', (ROOT / path).read_text(), flags=re.S)


class ParticleDisplayBlendTests(unittest.TestCase):
    def test_faint_white_over_black_matches_authored_opacity(self):
        # The reported failure: linear blending lifts a 10% white particle
        # to about 35% display brightness even with identity user gamma.
        alpha = 0.1
        self.assertGreater(encode(alpha), 0.34)
        scene = half(decode(half(alpha)) / 2.5)
        self.assertAlmostEqual(encode(scene * 2.5), alpha, delta=0.0001)

    def test_ordered_overlaps_match_legacy_alpha_composite(self):
        background = 0.03
        particles = [(0.8, 0.15), (0.25, 0.4), (0.9, 0.005)]
        expected = background
        attachment = half(encode(half(decode(background) / 2.5) * 2.5))
        for color, alpha in particles:
            expected = color * alpha + expected * (1 - alpha)
            attachment = half(color * alpha + attachment * (1 - alpha))
        result = encode(half(decode(attachment) / 2.5) * 2.5)
        self.assertAlmostEqual(result, expected, delta=0.0005)

    def test_untouched_pixels_round_trip_without_clipping_hdr(self):
        for radiance in (0, 1e-5, 0.001, 0.1, 1, 4, 100, 1000):
            with self.subTest(radiance=radiance):
                original = half(radiance)
                restored = half(decode(half(encode(original * 2.5))) / 2.5)
                self.assertAlmostEqual(restored, original,
                                       delta=max(1e-7, original * 0.003))

    def test_bias_compensation_keeps_background_and_user_gamma(self):
        for gamma in (0.3, 0.6, 1.0, 1.6, 2.0):
            effective = gamma + 0.4
            ratio = gamma / effective
            for background in (0.0, 0.001, 0.03, 0.25):
                for alpha in (0.0, 0.005, 0.1, 0.5, 1.0):
                    with self.subTest(gamma=gamma, background=background, alpha=alpha):
                        # Authored source should receive user gamma only.
                        # The background retains its original effective gamma.
                        source_color = 0.1
                        boosted = background ** ratio
                        composite = source_color * alpha + boosted * (1 - alpha)
                        restored = composite ** (1 / ratio)
                        actual = restored ** (1 / effective)
                        self.assertAlmostEqual(actual, composite ** (1 / gamma))
                        if alpha == 0:
                            self.assertAlmostEqual(actual, background ** (1 / effective))
                        if alpha == 1:
                            self.assertAlmostEqual(actual, source_color ** (1 / gamma))

    def test_translucent_linear_blend_cancels_only_backend_bias(self):
        for gamma in (0.3, 1.0, 2.0):
            effective = gamma + 0.4
            ratio = gamma / effective
            for alpha in (0, 0.01, 0.5, 1):
                background, source_linear = 0.025, 0.02
                boosted_linear = decode(encode(background * 2.5) ** ratio) / 2.5
                mixed_linear = source_linear * alpha + boosted_linear * (1 - alpha)
                restore_display = encode(mixed_linear * 2.5) ** (1 / ratio)
                actual = restore_display ** (1 / effective)
                expected = encode(mixed_linear * 2.5) ** (1 / gamma)
                self.assertAlmostEqual(actual, expected)
                if alpha == 0:
                    self.assertAlmostEqual(actual, encode(background * 2.5) ** (1 / effective))

    def test_conversion_keeps_alpha_and_hdr_headroom(self):
        shader = source('amnesia/slang/Particle/ParticleColorSpace.cs.slang')
        self.assertIn('sRGBToLinear(display) / kSceneExposure', shader)
        self.assertIn('linearToSRGB(color.rgb * kSceneExposure)', shader)
        self.assertNotIn('saturate(', shader)
        self.assertNotIn('color.a =', shader)
        self.assertIn('id.x >= width || id.y >= height', shader)

    def test_host_brackets_particles_before_translucent_meshes(self):
        host = source('HPL2/core/sources/graphics/HybridRenderer.cpp')
        begin = host.index('convertParticleColorSpace(false);')
        draw = host.index('for (const ParticleDraw &draw : particleDraws)', begin)
        end = host.index('convertParticleColorSpace(true);', draw)
        self.assertLess(begin, draw)
        self.assertLess(draw, end)
        self.assertIn('vk_d3d12_endRendering', host[draw:end])
        self.assertIn('m_translucentMesh.bindPipeline', host[end:])
        self.assertIn('convertParticleColorSpace(false, true);', host[end:])
        self.assertIn('convertParticleColorSpace(true, true);', host[end:])
        self.assertIn('toneMap->IsActive()', host)
        self.assertIn('toneMap->GetParams(&params)', host)
        self.assertIn('particleGammaRatio = 1.0f', host)
        particle = source('amnesia/slang/Particle/Particle.frag.slang')
        self.assertNotIn('encodeStandardBlendSource(', particle)
        self.assertIn('linearToSRGB(sample.rgb)', particle)
        self.assertIn('authored * psIn.color.rgb', particle)


if __name__ == '__main__':
    unittest.main()
