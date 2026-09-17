"""Census guards for the two-tuning light model.

A light is one object carrying a Standard tuning and a ray-traced one, resolved
per query from the world's backend (see notes/light-state-model.md). Three
properties of the shipped data are worth pinning, because each one is a cost or
a limitation the model accepted deliberately:

  * every shape populates the tunings it has, and only those -- an area light
    has no Standard tuning, a box light no ray-traced one;
  * the renderer-mask census, because the mask now gates *rendering* rather
    than creation, so every mask=1 and mask=2 light exists on both backends.
    A data edit that swells these numbers swells the per-frame light arrays;
  * how many lights have a flicker whose depth AGREES between the two tunings.
    Fades and flicker drive one shared level, so the passive tuning follows the
    active one in proportion; for the lights below that disagree, the passive
    tuning's own authored flicker depth only takes effect once that backend is
    the active one. That is the honest cost of the shared level, and this test
    stops it widening silently.

This reads the DEPLOYED data (deltas applied), so it is skipped when the game
has not been deployed yet.
"""

import os
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEPLOYED = ROOT / 'build-premake' / 'amnesia' / 'Release'

# tag -> (shape, ray-traced only)
SHAPES = {
    'PointLight': ('Point', False),
    'SpotLight': ('Spot', False),
    'BoxLight': ('Box', False),
    'AreaLight': ('Area', True),
}

EXPECTED_SHAPES = {'Point': 1544, 'Spot': 351, 'Box': 85, 'Area': 59}
# mask 3 loads on both backends, 1 renders only on Standard, 2 only ray-traced.
EXPECTED_MASKS = {3: 1458, 1: 489, 2: 92}
EXPECTED_FLICKER = {'agree': 1819, 'differ': 44, 'degenerate': 32}


def light_elements():
    for base, _, files in os.walk(DEPLOYED):
        for name in sorted(files):
            if not name.endswith(('.map', '.ent')):
                continue
            try:
                root = ET.parse(os.path.join(base, name)).getroot()
            except ET.ParseError:
                continue
            for elem in root.iter():
                if elem.tag in SHAPES:
                    yield elem


def number(elem, name, default=0.0):
    raw = elem.get(name)
    if raw is None:
        return float(default)
    try:
        return float(raw.split()[0])
    except (ValueError, IndexError):
        return float(default)


@unittest.skipUnless(DEPLOYED.is_dir(), 'game data has not been deployed')
class LightDualTuningCensusTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lights = list(light_elements())

    def test_data_is_present(self):
        self.assertGreater(len(self.lights), 0, 'no lights found under %s' % DEPLOYED)

    def test_shape_census(self):
        census = {}
        for elem in self.lights:
            shape = SHAPES[elem.tag][0]
            census[shape] = census.get(shape, 0) + 1
        self.assertEqual(EXPECTED_SHAPES, census)

    def test_every_shape_populates_the_tunings_it_has(self):
        # An area light is ray-traced only and a box light Standard only; every
        # other shape must carry both.
        for elem in self.lights:
            shape, ray_traced_only = SHAPES[elem.tag]
            if shape == 'Area':
                self.assertTrue(ray_traced_only)
                # Its photometry is spelled WITHOUT the prefix, because it has
                # no retail meaning to collide with.
                self.assertIsNone(elem.get('Re_Intensity'),
                                  '%s authors Re_Intensity but has no Standard tuning' % elem.get('Name'))
            elif shape == 'Box':
                for name in ('Re_Intensity', 'Re_Radius', 'Re_SourceRadius'):
                    self.assertIsNone(elem.get(name),
                                      '%s is a box light and has no ray-traced tuning' % elem.get('Name'))

    def test_renderer_mask_census(self):
        census = {}
        for elem in self.lights:
            shape, ray_traced_only = SHAPES[elem.tag]
            raw = elem.get('RendererMask')
            mask = int(float(raw)) & 3 if raw is not None else (2 if ray_traced_only else 3)
            if ray_traced_only:
                mask &= 2
            census[mask] = census.get(mask, 0) + 1
        self.assertEqual(EXPECTED_MASKS, census)

    def test_flicker_depth_agreement(self):
        # The ratio between a tuning's flicker-off value and its on value is
        # what the shared level preserves. Where the two tunings disagree, only
        # the active backend's depth is honoured.
        census = {'agree': 0, 'differ': 0, 'degenerate': 0}
        for elem in self.lights:
            shape, ray_traced_only = SHAPES[elem.tag]
            if ray_traced_only or shape == 'Box':
                continue

            standard_on = number(elem, 'Radius', 1.0)
            standard_off = number(elem, 'FlickerOffRadius', 0.0)
            if elem.get('Re_Intensity') is not None:
                ray_on = number(elem, 'Re_Intensity', 1.0)
                ray_off = number(elem, 'Re_FlickerOffIntensity', 0.0)
            else:
                # Promoted: the retail radius IS the intensity, so the depths
                # agree by construction.
                ray_on, ray_off = standard_on, standard_off

            if standard_on <= 0 or ray_on <= 0:
                census['degenerate'] += 1
            elif abs(standard_off / standard_on - ray_off / ray_on) <= 1e-3:
                census['agree'] += 1
            else:
                census['differ'] += 1
        self.assertEqual(EXPECTED_FLICKER, census)


@unittest.skipUnless(DEPLOYED.is_dir(), 'game data has not been deployed')
class BackendEntityTwinTest(unittest.TestCase):
    """A placement must not exist twice, once per backend.

    Those twins only worked because the renderer mask decided whether an object
    was CREATED, so exactly one half ever existed. A live backend switch needs
    both halves resident, which would give duplicate names, duplicate physics
    bodies and independent save state -- so the candle pairs were folded onto
    the lit candle_floor.ent, which now lights BOTH backends
    (scripts/merge_backend_entity_twins.py).
    """

    def test_no_candle_twins_remain(self):
        offenders = []
        for base, _, files in os.walk(ROOT / 'amnesia' / 'resources'):
            for name in sorted(files):
                if not name.endswith('.map_delta'):
                    continue
                path = os.path.join(base, name)
                try:
                    root = ET.parse(path).getroot()
                except ET.ParseError:
                    continue
                for add in root.iter('Add'):
                    for elem in add:
                        if elem.tag != 'Entity':
                            continue
                        if (elem.get('RendererMask') or '') != '2':
                            continue
                        if elem.get('Filename', '').endswith('candle_floor.ent'):
                            offenders.append('%s: %s' % (name, elem.get('Name')))
        self.assertEqual([], offenders,
                         'candle twins remain; run scripts/merge_backend_entity_twins.py')

    def test_no_placement_is_split_across_backends(self):
        # The twin pattern specifically: two placements sharing a name whose
        # renderer masks are disjoint, i.e. one per backend. Retail data does
        # reuse a few names within one backend (29_orb_chamber), which is a
        # separate quirk and not what this guards.
        offenders = []
        for base, _, files in os.walk(DEPLOYED / 'maps'):
            for name in sorted(files):
                if not name.endswith('.map'):
                    continue
                try:
                    root = ET.parse(os.path.join(base, name)).getroot()
                except ET.ParseError:
                    continue
                by_name = {}
                for elem in root.iter('Entity'):
                    key = (elem.get('Name') or '').lower()
                    if not key:
                        continue
                    raw = elem.get('RendererMask')
                    mask = int(float(raw)) & 3 if raw is not None else 3
                    by_name.setdefault(key, []).append(mask)
                for key, masks in by_name.items():
                    for i, left in enumerate(masks):
                        for right in masks[i + 1:]:
                            if (left & right) == 0:
                                offenders.append('%s: %s masks %d/%d' % (name, key, left, right))
        self.assertEqual([], offenders,
                         'a placement is still split per backend; it cannot survive a live switch')

    def test_remaining_per_backend_props_are_the_known_few(self):
        # Genuinely per-backend whole props, which the render/activity mask gate
        # handles. Pinned so a data edit that adds more is a visible decision.
        adds = 0
        for base, _, files in os.walk(ROOT / 'amnesia' / 'resources'):
            for name in sorted(files):
                if not name.endswith(('.map_delta', '.ent_delta')):
                    continue
                try:
                    root = ET.parse(os.path.join(base, name)).getroot()
                except ET.ParseError:
                    continue
                for add in root.iter('Add'):
                    for elem in add:
                        if elem.tag in ('Entity', 'Area') and (elem.get('RendererMask') or '') == '2':
                            adds += 1
        self.assertEqual(7, adds, 'expected only the rt_prison torches to be ray-traced-only')


if __name__ == '__main__':
    unittest.main()
