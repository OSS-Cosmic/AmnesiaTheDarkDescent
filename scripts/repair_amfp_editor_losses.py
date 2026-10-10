#!/usr/bin/env python3
"""Put back AMFP data that editor re-saves stripped from the Release tree.

Before the AMFP editor ports (FBX loader, AMFP light/occluder properties,
AMFP class definitions), a LevelEditor/ModelEditor save silently dropped:
  - entities whose .ent uses an .fbx mesh (every enemy), and ColorGrading /
    Infection areas;
  - the IsOccluder / Brightness / Falloff / Priority attributes;
  - every instance or type variable the TDD class definitions don't declare;
and `mapdelta diff` then recorded all of it as <Remove>/<RemoveAttr>/<RemoveVar> ops.

This compares each Release .map/.ent with the retail file and restores exactly
those losses in place, leaving everything else (your edits) byte-identical.
Afterwards, re-run `mapdelta diff ... --prune` to rebuild the deltas.

  python3 scripts/repair_amfp_editor_losses.py [--dry-run] [--game DIR] [--release DIR]
"""
import argparse, glob, os, re, sys
import xml.etree.ElementTree as ET

DEFAULT_GAME = os.path.expanduser('~/.local/share/Steam/steamapps/common/Machine for Pigs')
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_RELEASE = os.path.join(ROOT, 'build-premake', 'amfp', 'Release')

LOST_ATTRS = ('IsOccluder', 'Brightness', 'Falloff', 'Priority')
ADDED_VARS = ()   # variables a re-save added that retail doesn't have (none so far)
LOST_AREA_TYPES = ('ColorGrading', 'Infection')
SECTIONS = ('StaticObjects', 'Primitives', 'Decals', 'Entities')


def sanitize(text):
    text = re.sub(r'&(?!amp;|lt;|gt;|quot;|apos;|#)', '&amp;', text)
    text = re.sub(r'"(?=[A-Za-z_]+=")', '" ', text)
    return text


def parse(text):
    return ET.fromstring(sanitize(text))


def objects(root):
    """(tag, ID) -> element for the direct children of the map sections / ent ModelData sections."""
    out = {}
    for sec in root.iter():
        if sec.tag in SECTIONS or sec.tag in ('Shapes', 'Bodies', 'Joints'):
            for el in sec:
                if el.get('ID') is not None:
                    out[(el.tag, el.get('ID'))] = el
    return out


def file_table(root, section):
    for idx in root.iter('FileIndex_' + section):
        return {f.get('Id'): f.get('Path', '') for f in idx.findall('File')}
    return {}


def ent_mesh_is_fbx(game, ent_path, cache={}):
    if ent_path not in cache:
        p = os.path.join(game, ent_path)
        fbx = False
        if os.path.exists(p):
            m = re.search(r'<Mesh [^>]*Filename="([^"]*)"', open(p, encoding='utf-8', errors='replace').read())
            fbx = bool(m and m.group(1).lower().endswith('.fbx'))
        cache[ent_path] = fbx
    return cache[ent_path]


def element_span(text, tag, oid):
    """Start/end of the element <tag ... ID="oid" ...> ... </tag> (or self-closed)."""
    m = re.search(r'<%s\s[^>]*?\bID="%s"[^>]*?(/?)>' % (re.escape(tag), re.escape(oid)), text)
    if not m:
        return None
    if m.group(1) == '/':
        return m.start(), m.end(), m.end()
    # Objects of the same tag don't nest, so the next closing tag is ours.
    end = text.index('</%s>' % tag, m.end()) + len('</%s>' % tag)
    return m.start(), m.end(), end


def raw_element(text, tag, oid):
    span = element_span(text, tag, oid)
    return text[span[0]:span[2]] if span else None


START_TAG = re.compile(r'<([A-Za-z_]+)\s[^>]*?\bID="(\d+)"[^>]*?(/?)>')


def apply_object_edits(text, edits):
    """edits: (tag, id) -> {'attrs': [(name, value)], 'add_vars': [(name, value)], 'del_vars': [name]}.
    Finds every object once, then splices from the back so offsets stay valid."""
    index = {}
    for m in START_TAG.finditer(text):
        index.setdefault((m.group(1), m.group(2)), m)
    parts, pos_end = [], len(text)
    for key in sorted((k for k in edits if k in index), key=lambda k: index[k].start(), reverse=True):
        m = index[key]; tag = key[0]; e = edits[key]
        head = m.group(0)
        if m.group(3) == '/':
            body, end = '', m.end()
        else:
            end = text.index('</%s>' % tag, m.end())
            body = text[m.end():end]
        if e['add_vars']:
            bm = re.search(r'([ \t]*)</UserVariables>', body)
            if bm is not None:
                sib = re.search(r'\n([ \t]*)<Var', body)
                indent = sib.group(1) if sib else bm.group(1) + '\t'
                ins = ''.join('%s<Var Name="%s" Value="%s"/>\n' % (indent, n, v) for n, v in e['add_vars'])
                body = body[:bm.start()] + ins + body[bm.start():]
        for name in e['del_vars']:
            body = re.sub(r'\n[ \t]*<Var\s+Name="%s"[^>]*/>' % re.escape(name), '', body, count=1)
        if e['attrs']:
            close = '/>' if head.endswith('/>') else '>'
            core = head[:-len(close)].rstrip() + ''.join(' %s="%s"' % (n, v) for n, v in e['attrs'])
            head = core + (' ' if close == '/>' else '') + close
        parts.append(text[end:pos_end])
        parts.append(head + body)
        pos_end = m.start()
    parts.append(text[:pos_end])
    return ''.join(reversed(parts))


def repair_file(rel, game, release, dry_run):
    rpath, gpath = os.path.join(release, rel), os.path.join(game, rel)
    rtext = open(rpath, encoding='utf-8', errors='surrogateescape').read()
    gtext = open(gpath, encoding='utf-8', errors='replace').read()
    try:
        rroot, groot = parse(rtext), parse(gtext)
    except ET.ParseError as e:
        return ['skipped (parse error: %s)' % e]
    robj, gobj = objects(rroot), objects(groot)
    is_map = rel.endswith('.map')
    stats = {}
    def bump(k, n=1): stats[k] = stats.get(k, 0) + n

    ##########################################
    # Attributes and instance variables on objects both files have.
    edits = {}
    for key, g in gobj.items():
        r = robj.get(key)
        if r is None:
            continue
        e = {'attrs': [], 'add_vars': [], 'del_vars': []}
        for a in LOST_ATTRS:
            if g.get(a) is not None and r.get(a) is None:
                e['attrs'].append((a, g.get(a))); bump('attrs')
        gv, rv = g.find('UserVariables'), r.find('UserVariables')
        if gv is not None and rv is not None:
            have = {v.get('Name') for v in rv.findall('Var')}
            gnames = {v.get('Name') for v in gv.findall('Var')}
            for v in gv.findall('Var'):
                if v.get('Name') not in have:
                    e['add_vars'].append((v.get('Name'), v.get('Value', ''))); bump('vars')
            for name in ADDED_VARS:
                if name in have and name not in gnames:
                    e['del_vars'].append(name); bump('TDD vars removed')
        if e['attrs'] or e['add_vars'] or e['del_vars']:
            edits[key] = e
    text = apply_object_edits(rtext, edits)

    ##########################################
    # .ent type variables.
    if not is_map:
        gv, rv = groot.find('UserDefinedVariables'), rroot.find('UserDefinedVariables')
        if gv is not None and rv is not None:
            have = {v.get('Name') for v in rv.findall('Var')}
            missing = [v for v in gv.findall('Var') if v.get('Name') not in have]
            m = re.search(r'([ \t]*)</UserDefinedVariables>', text)
            if missing and m:
                sib = re.search(r'\n([ \t]*)<Var', text[max(0, m.start()-2000):m.start()])
                indent = sib.group(1) if sib else m.group(1) + '\t'
                ins = ''.join('%s<Var Name="%s" Value="%s"/>\n' % (indent, v.get('Name'), v.get('Value', '')) for v in missing)
                text = text[:m.start()] + ins + text[m.start():]
                bump('type vars', len(missing))

    ##########################################
    # Objects the editor couldn't load (maps only).
    if is_map:
        gfiles, rfiles = file_table(groot, 'Entities'), file_table(rroot, 'Entities')
        used_ids = {k[1] for k in robj}
        rnames = {(t, o.get('Name')) for (t, _), o in robj.items()}
        for (tag, oid), g in sorted(gobj.items(), key=lambda kv: int(kv[0][1])):
            if (tag, oid) in robj or (tag, g.get('Name')) in rnames:
                continue
            if tag == 'Entity':
                if not ent_mesh_is_fbx(game, gfiles.get(g.get('FileIndex'), '')):
                    continue
            elif not (tag == 'Area' and g.get('AreaType') in LOST_AREA_TYPES):
                continue
            raw = raw_element(gtext, tag, oid)
            if raw is None:
                continue
            if oid in used_ids:
                new_id = str(max(int(i) for i in used_ids if i.isdigit()) + 1)
                raw = re.sub(r'\bID="%s"' % oid, 'ID="%s"' % new_id, raw, count=1)
                oid = new_id
            used_ids.add(oid)
            if tag == 'Entity':
                path = gfiles[g.get('FileIndex')]
                rid = next((i for i, p in rfiles.items() if p.lower() == path.lower()), None)
                if rid is None:
                    rid = str(max([int(i) for i in rfiles] + [-1]) + 1)
                    rfiles[rid] = path
                    fm = re.search(r'\n([ \t]*)</FileIndex_Entities>', text)
                    sib = re.search(r'\n([ \t]*)<File ', text[max(0, fm.start()-500):fm.start()])
                    indent = sib.group(1) if sib else fm.group(1) + '\t'
                    text = text[:fm.start()] + '\n%s<File Id="%s" Path="%s"/>' % (indent, rid, path) + text[fm.start():]
                    text = re.sub(r'(<FileIndex_Entities NumOfFiles=")(\d+)"', lambda mm: '%s%d"' % (mm.group(1), len(rfiles)), text, count=1)
                raw = re.sub(r'\bFileIndex="\d+"', 'FileIndex="%s"' % rid, raw, count=1)
            em = re.search(r'\n([ \t]*)</Entities>', text)
            text = text[:em.start()] + '\n' + em.group(1) + '    ' + raw.replace('\r\n', '\n') + text[em.start():]
            bump('restored %s' % tag)

    if text != rtext:
        try:
            parse(text)
        except ET.ParseError as e:
            return ['NOT written, result would not parse: %s' % e]
        if not dry_run:
            open(rpath, 'w', encoding='utf-8', errors='surrogateescape').write(text)
    return ['%s x%d' % (k, v) for k, v in sorted(stats.items())]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--game', default=DEFAULT_GAME)
    ap.add_argument('--release', default=DEFAULT_RELEASE)
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('files', nargs='*', help='limit to these paths (relative to the game dir)')
    args = ap.parse_args()

    rels = args.files or sorted(
        [os.path.relpath(p, args.release) for p in glob.glob(os.path.join(args.release, '**', '*.map'), recursive=True)] +
        [os.path.relpath(p, args.release) for p in glob.glob(os.path.join(args.release, 'entities', '**', '*.ent'), recursive=True)])
    changed = 0
    for rel in rels:
        if not os.path.exists(os.path.join(args.game, rel)):
            continue
        res = repair_file(rel, args.game, args.release, args.dry_run)
        if res:
            changed += 1
            print('%s: %s' % (rel, ', '.join(res[:12]) + (' ...' if len(res) > 12 else '')))
    print('%d file(s) %s' % (changed, 'would change' if args.dry_run else 'changed'))


if __name__ == '__main__':
    main()
