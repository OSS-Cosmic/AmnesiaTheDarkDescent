#!/usr/bin/env python3
"""Generate the AMFP editor class definitions (EntityTypes.cfg / AreaTypes.cfg).

The editors drop any map/entity variable, entity type or area type that their
class definitions don't declare (cEditorClassInstance::Load/Save, and
EditorWorld's area loading), and the shared definitions are TDD's. AMFP never
shipped its own, so this derives them from the retail content: the TDD set,
plus every type and variable the AMFP .ent files and maps actually use.

  python3 scripts/gen_amfp_editor_types.py [--game <AMFP install>] [--out <dir>]

Existing TDD definitions are kept as they are (types, defaults, enums). New
variables are String unless every value seen is true/false, so values round
trip byte for byte. TDD's Insanity area type (unused by AMFP) is dropped.
"""
import argparse, collections, glob, os, re, sys
import xml.etree.ElementTree as ET

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TDD_DIR = os.path.join(ROOT, 'HPL2', 'tools', 'resources', 'editor')
DEFAULT_GAME = os.path.expanduser('~/.local/share/Steam/steamapps/common/Machine for Pigs')
DEFAULT_OUT = os.path.join(ROOT, 'HPL2', 'tools', 'resources_amfp', 'editor')

# TDD instance vars to drop. None: even TDD's insanity vars (IsInsanityVision,
# VisionMaxSanity) are on every retail AMFP prop, so AMFP's editor kept them.
DROP_VARS = set()
# TDD area types AMFP doesn't register (amfp/src/game/LuxBase.cpp).
DROP_AREA_TYPES = {'Insanity'}


def read_xml(path):
    data = open(path, 'rb').read()
    # Retail files have stray bytes and unescaped '&' now and then.
    text = data.decode('utf-8', 'replace')
    text = re.sub(r'&(?!amp;|lt;|gt;|quot;|apos;|#)', '&amp;', text)
    text = re.sub(r'"(?=[A-Za-z_]+=")', '" ', text)   # SpecialEventTime="0"Speed=...
    text = re.sub(r'<(?=[0-9 =.-])', '&lt;', text)     # "Must be <0" inside a description
    return ET.fromstring(text)


def collect(game):
    ent_type = {}                                     # ent path (lower) -> (type, subtype)
    type_vars = collections.defaultdict(lambda: collections.defaultdict(collections.Counter))
    for path in glob.glob(os.path.join(game, 'entities', '**', '*.ent'), recursive=True):
        try:
            root = read_xml(path)
        except ET.ParseError as e:
            print('warning: skipping %s: %s' % (path, e), file=sys.stderr)
            continue
        udv = root.find('UserDefinedVariables')
        if udv is None:
            continue
        t = udv.get('EntityType', '')
        rel = os.path.relpath(path, game).replace('\\', '/').lower()
        ent_type[rel] = (t, udv.get('EntitySubType', ''))
        for v in udv.findall('Var'):
            type_vars[t][v.get('Name')][v.get('Value', '')] += 1

    inst_vars = collections.defaultdict(lambda: collections.defaultdict(collections.Counter))
    area_vars = collections.defaultdict(lambda: collections.defaultdict(collections.Counter))
    inst_count = collections.Counter()
    for path in glob.glob(os.path.join(game, 'maps', '**', '*.map'), recursive=True):
        try:
            root = read_xml(path)
        except ET.ParseError as e:
            print('warning: skipping %s: %s' % (path, e), file=sys.stderr)
            continue
        files = {}
        for idx in root.iter('FileIndex_Entities'):
            for f in idx.findall('File'):
                files[f.get('Id')] = f.get('Path', '').replace('\\', '/').lower()
        for e in root.iter('Entity'):
            t = ent_type.get(files.get(e.get('FileIndex'), ''), ('', ''))[0]
            if not t:
                continue
            inst_count[t] += 1
            uv = e.find('UserVariables')
            for v in (uv.findall('Var') if uv is not None else []):
                inst_vars[t][v.get('Name')][v.get('Value', '')] += 1
        for a in root.iter('Area'):
            t = a.get('AreaType', '')
            uv = a.find('UserVariables')
            area_vars[t]  # register the type even without vars
            for v in (uv.findall('Var') if uv is not None else []):
                area_vars[t][v.get('Name')][v.get('Value', '')] += 1
    return type_vars, inst_vars, area_vars, inst_count


def new_var(name, values):
    common = values.most_common(1)[0][0] if values else ''
    vtype = 'Bool' if values and all(x in ('true', 'false') for x in values) else 'String'
    return ET.Element('Var', {'Name': name, 'Type': vtype, 'DefaultValue': common,
                              'Description': 'AMFP (from retail content).'})


def var_names(elem):
    return {v.get('Name') for v in elem.findall('Var')} if elem is not None else set()


def gen_entity_types(tdd_path, type_vars, inst_vars):
    root = read_xml(tdd_path); tree = ET.ElementTree(root)
    bases = {b.get('Name'): b for b in root.iter('BaseClass')}
    types = {t.get('Name'): t for t in root.iter('Type')}
    parent_of_types = next(p for p in root.iter() for c in p if c.tag == 'Type')

    added_types, added = [], collections.Counter()
    for t in sorted(set(type_vars) | set(inst_vars)):
        if not t:
            continue
        if t not in types:
            base = 'Enemy' if t.startswith('Enemy') else 'Prop'
            el = ET.SubElement(parent_of_types, 'Type', {'Name': t, 'BaseClass': base})
            ET.SubElement(el, 'TypeVars'); ET.SubElement(el, 'InstanceVars')
            types[t] = el; added_types.append(t)
        el = types[t]
        base = bases.get(el.get('BaseClass', ''))
        for section, found in (('TypeVars', type_vars.get(t, {})), ('InstanceVars', inst_vars.get(t, {}))):
            sec = el.find(section)
            if sec is None:
                sec = ET.SubElement(el, section)
            known = var_names(sec) | var_names(base.find(section) if base is not None else None)
            for name in sorted(found):
                if name in known:
                    continue
                sec.append(new_var(name, found[name])); added[section] += 1

    removed = 0
    for sec in list(root.iter('InstanceVars')):
        for v in list(sec.findall('Var')):
            if v.get('Name') in DROP_VARS:
                sec.remove(v); removed += 1
    return tree, added_types, added, removed


def gen_area_types(tdd_path, area_vars):
    root = read_xml(tdd_path); tree = ET.ElementTree(root)
    types_parent = root.find('Types')
    types = {t.get('Name'): t for t in types_parent.findall('Type')}
    for name in DROP_AREA_TYPES:
        if name in types:
            types_parent.remove(types.pop(name))
    added_types, added = [], 0
    for t in sorted(area_vars):
        if not t:
            continue
        if t not in types:
            el = ET.SubElement(types_parent, 'Type', {'Name': t})
            setup = ET.SubElement(el, 'EditorSetupVars')
            ET.SubElement(setup, 'Var', {'Name': 'Color', 'Value': '0.5 0.5 1 1'})
            ET.SubElement(el, 'Vars')
            types[t] = el; added_types.append(t)
        vars_el = types[t].find('Vars')
        if vars_el is None:
            vars_el = ET.SubElement(types[t], 'Vars')
        known = var_names(vars_el)
        for name in sorted(area_vars[t]):
            if name not in known:
                vars_el.append(new_var(name, area_vars[t][name])); added += 1
    return tree, added_types, added


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--game', default=DEFAULT_GAME)
    ap.add_argument('--out', default=DEFAULT_OUT)
    args = ap.parse_args()

    type_vars, inst_vars, area_vars, inst_count = collect(args.game)
    os.makedirs(args.out, exist_ok=True)

    tree, new_types, added, removed = gen_entity_types(os.path.join(TDD_DIR, 'EntityTypes.cfg'), type_vars, inst_vars)
    ET.indent(tree, '\t'); tree.write(os.path.join(args.out, 'EntityTypes.cfg'), encoding='utf-8', xml_declaration=False)
    print('EntityTypes.cfg: new types %s; +%d type vars, +%d instance vars; -%d TDD vars'
          % (new_types, added['TypeVars'], added['InstanceVars'], removed))

    tree, new_area_types, added_area = gen_area_types(os.path.join(TDD_DIR, 'AreaTypes.cfg'), area_vars)
    ET.indent(tree, '\t'); tree.write(os.path.join(args.out, 'AreaTypes.cfg'), encoding='utf-8', xml_declaration=False)
    print('AreaTypes.cfg: new types %s; +%d vars; dropped %s' % (new_area_types, added_area, sorted(DROP_AREA_TYPES)))


if __name__ == '__main__':
    main()
