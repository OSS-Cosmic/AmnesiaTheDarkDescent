"""Check source map deltas for obsolete per-backend candle twins."""

import os
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


class BackendEntityTwinTest(unittest.TestCase):
    """A placement must not exist twice, once per backend.

    Those twins only worked because the renderer mask decided whether an object
    was CREATED, so exactly one half ever existed. A live backend switch needs
    both halves resident, which would give duplicate names, duplicate physics
    bodies and independent save state -- so the candle pairs were folded onto
    the lit candle_floor.ent, which now lights BOTH backends.
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
                         'candle twins remain; use one candle_floor.ent placement for both backends')


if __name__ == '__main__':
    unittest.main()
