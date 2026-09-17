"""Check the shipped hull's silhouette from the authored emitter position."""
import math
from pathlib import Path
import unittest
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / 'amnesia/resources/models/hand_objects/lantern'
NS = {'c': 'http://www.collada.org/2005/11/COLLADASchema'}


def sub(a, b):
    return tuple(x - y for x, y in zip(a, b))


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def cross(a, b):
    return (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])


class LanternShadowHullTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        mesh = ET.parse(ASSETS / 'hand_lantern_shadow.dae')
        values = list(map(float, mesh.find('.//c:source[@id="positions"]/c:float_array', NS).text.split()))
        vertices = list(zip(*[iter(values)] * 3))
        faces = mesh.find('.//c:triangles', NS)
        indices = list(map(int, faces.find('c:p', NS).text.split()))[::3]
        cls.triangles = [tuple(vertices[j] for j in indices[i:i+3]) for i in range(0, len(indices), 3)]
        delta = ET.parse(ASSETS / 'hand_lantern.ent_delta')
        # One element per light: the retail PointLight_1 carries the authored
        # position, and the ray-traced tuning rides on it as Re_-prefixed
        # attributes. A twin <Add><Re_PointLight> here would mean the delta was
        # never migrated, so assert it is gone as well.
        if delta.find('.//Re_PointLight') is not None:
            raise AssertionError('hand_lantern.ent_delta still has a pre-merge Re_PointLight twin')
        light = delta.find('./Modify[@Name="PointLight_1"]/SetAttr')
        if light is None or light.get('WorldPos') is None:
            raise AssertionError('hand_lantern.ent_delta has no emitter light')
        cls.emitter = tuple(map(float, light.get('WorldPos').split()))
        cls.source_radius = float(light.get('Re_SourceRadius'))

    def hit(self, direction, origin=None):
        origin = self.emitter if origin is None else origin
        # Two-sided Moller-Trumbore ray/triangle intersection.
        for a, b, c in self.triangles:
            e1, e2 = sub(b, a), sub(c, a)
            p = cross(direction, e2)
            det = dot(e1, p)
            if abs(det) < 1e-12:
                continue
            t = sub(origin, a)
            u = dot(t, p) / det
            q = cross(t, e1)
            v = dot(direction, q) / det
            distance = dot(e2, q) / det
            if u >= -1e-6 and v >= -1e-6 and u + v <= 1.000001 and distance > 1e-6:
                return True
        return False

    def test_caps_block_above_and_below(self):
        self.assertTrue(self.hit((0, 1, 0)))
        self.assertTrue(self.hit((0, -1, 0)))

    def test_each_post_casts_a_shadow(self):
        for i in range(8):
            angle = i * math.tau / 8
            self.assertTrue(self.hit((math.cos(angle), 0, math.sin(angle))), i)

    def test_windows_stay_open_across_the_light_source(self):
        for i in range(8):
            angle = (i + 0.5) * math.tau / 8
            direction = (math.cos(angle), 0, math.sin(angle))
            for axis in range(3):
                for sign in (-1, 1):
                    origin = list(self.emitter)
                    origin[axis] += sign * self.source_radius
                    self.assertFalse(self.hit(direction, origin), (i, axis, sign))

    def test_no_degenerate_triangles(self):
        self.assertEqual(len(self.triangles), 152)
        for a, b, c in self.triangles:
            normal = cross(sub(b, a), sub(c, a))
            self.assertGreater(dot(normal, normal), 1e-15)


if __name__ == '__main__':
    unittest.main()
