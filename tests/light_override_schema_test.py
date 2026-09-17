"""Schema guard for the merged light element.

One light is one object: the retail attributes drive Standard, and the
ray-traced backend reads the same attribute spelled "Re_<Name>" when the
element authors one. This test walks the shipped Redux resources and asserts
the data keeps that contract:

  * no pre-merge Re_PointLight / Re_SpotLight / Re_AreaLight *elements* remain
    (their tuning belongs on the corresponding light as Re_ attributes);
  * every Re_ attribute names something cEngineFileLoading::LoadLight actually
    reads, so a typo like Re_Castshadows cannot sit in the data doing nothing;
  * nothing tries to override transform or identity, which the loader reads
    outside the light path and can never vary per backend.

A light is ONE object carrying two tunings, so only the per-backend fields may
carry a Re_ spelling: photometry, CastShadows and DiffuseColor. Everything else
(FOV, gobo, falloff, shadow resolution, flicker timings) is shared by both
backends, and a Re_ spelling of one would silently do nothing.

Keep OVERRIDABLE in step with IsOverridableLightAttribute in
HPL2/core/sources/resources/EngineFileLoading.cpp.
"""

import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

RESOURCES = Path(__file__).resolve().parent.parent / 'amnesia' / 'resources'
SUFFIXES = ('.map', '.ent', '.map_delta', '.ent_delta')

TWIN_TAGS = ('Re_PointLight', 'Re_SpotLight', 'Re_AreaLight')

OVERRIDABLE = {
    # photometry (Re_ only on shapes that have a legacy class)
    'Intensity', 'Radius', 'SourceRadius', 'FlickerOffIntensity', 'FlickerOffRadius',
    # the rest of the per-backend tuning
    'CastShadows', 'DiffuseColor',
}

# Read by SetupWorldEntity / the delta matcher, never per backend.
NEVER_OVERRIDABLE = {'WorldPos', 'Rotation', 'Scale', 'ID', 'Name', 'RendererMask'}


def resource_files():
    return sorted(p for p in RESOURCES.rglob('*') if p.suffix in SUFFIXES or
                  p.name.endswith(('.map_delta', '.ent_delta')))


class LightOverrideSchemaTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.documents = []
        for path in resource_files():
            try:
                cls.documents.append((path, ET.parse(path).getroot()))
            except ET.ParseError as error:
                raise AssertionError('%s does not parse: %s' % (path, error))

    def test_resources_are_present(self):
        # A silently empty walk would make every other assertion vacuous.
        self.assertGreater(len(self.documents), 0, 'no resource files found under %s' % RESOURCES)

    def test_no_pre_merge_twin_elements(self):
        offenders = []
        for path, root in self.documents:
            for elem in root.iter():
                if elem.tag in TWIN_TAGS:
                    offenders.append('%s: <%s Name="%s">' % (path, elem.tag, elem.get('Name', '?')))
        self.assertEqual([], offenders,
                         'pre-merge twin elements remain; use one light element with Re_ tuning attributes')

    def test_override_attributes_are_readable_by_the_loader(self):
        offenders = []
        for path, root in self.documents:
            for elem in root.iter():
                for name in elem.attrib:
                    if not name.startswith('Re_'):
                        continue
                    suffix = name[len('Re_'):]
                    if suffix in NEVER_OVERRIDABLE:
                        offenders.append('%s: %s cannot vary per backend' % (path, name))
                    elif suffix not in OVERRIDABLE:
                        offenders.append('%s: %s is not an attribute the light loader reads' % (path, name))
        self.assertEqual([], offenders)


if __name__ == '__main__':
    unittest.main()
