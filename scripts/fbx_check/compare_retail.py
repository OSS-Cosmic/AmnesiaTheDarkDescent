import json, os, subprocess, sys, glob
sys.path.insert(0, os.path.dirname(__file__))
from mshread import load_msh, load_anm
import numpy as np
G = os.environ.get('AMFP_DIR', os.path.expanduser('~/.local/share/Steam/steamapps/common/Machine for Pigs'))
ORACLE = os.environ.get('FBX_DUMP', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'fbx_dump'))
def run(path, mode):
    out = subprocess.run([ORACLE, path, mode], capture_output=True, text=True, timeout=300).stdout
    return json.loads(out)
def base(s): s = s.replace('\\', '/').split('/')[-1]; return s.rsplit('.', 1)[0].lower() if '.' in s else s.lower()
def cmp_mesh(ours, ret):
    issues = []
    rb = ret['bones']
    if [b['name'] for b in rb] != [b['name'] for b in ours['bones']]: issues.append('bone names/order differ (%d vs %d)' % (len(rb), len(ours['bones'])))
    else:
        if [b['parent'] for b in rb] != [b['parent'] for b in ours['bones']]: issues.append('bone parents differ')
        e = max([np.abs(np.array(a['m']) - np.array(b['m'])).max() for a, b in zip(ours['bones'], rb)] or [0])
        if e > 1e-3: issues.append('bone local err %.3g' % e)
    if len(ours['subs']) != len(ret['subs']): issues.append('submesh count %d vs %d' % (len(ours['subs']), len(ret['subs']))); return issues
    for o, r in zip(ours['subs'], ret['subs']):
        tag = o['name']
        if o['name'] != r['name']: issues.append('%s: name vs %s' % (tag, r['name']))
        if base(o['material']) != base(r['material']): issues.append('%s: material %r vs %r' % (tag, o['material'], r['material']))
        nv = len(o['pos']) // 3
        if nv != r['nv']: issues.append('%s: nv %d vs %d' % (tag, nv, r['nv'])); continue
        if o['idx'] != r['idx']: issues.append('%s: indices differ (%d vs %d)' % (tag, len(o['idx']), len(r['idx'])))
        P = np.array(r['arrays'][1][2]).reshape(-1, 4)[:, :3]; N = np.array(r['arrays'][0][2]).reshape(-1, 3)
        T = np.array(r['arrays'][5][2]).reshape(-1, 3); C = np.array(r['arrays'][2][2]).reshape(-1, 4)
        for nm, a, b, tol in (('pos', np.array(o['pos']).reshape(-1, 3), P, 1e-3), ('nrm', np.array(o['nrm']).reshape(-1, 3), N, 1e-2),
                              ('uv', np.array(o['uv']).reshape(-1, 3)[:, :2], T[:, :2], 1e-3), ('col', np.array(o['col']).reshape(-1, 4), C, 1e-3)):
            e = np.abs(a - b).max() if len(a) else 0
            if e > tol: issues.append('%s: %s err %.3g' % (tag, nm, e))
        so = {}; sr = {}
        for v, bo, w in o['pairs']: so[(v, bo)] = so.get((v, bo), 0) + w
        for v, bo, w in r['pairs']: sr[(v, bo)] = sr.get((v, bo), 0) + w
        extra = set(so) - set(sr); missing = set(sr) - set(so)
        werr = max([abs(so[k] - sr[k]) for k in set(so) & set(sr)] or [0])
        if extra or missing or werr > 1e-4:
            issues.append('%s: bone pairs differ (+%d -%d, weight err %.3g)' % (tag, len(extra), len(missing), werr))
    return issues
def cmp_anim(ours, ret):
    issues = []
    a = ours['anim']
    if a is None: return ['no animation imported']
    if abs(a['length'] - ret['length']) > 1e-3: issues.append('length %.4f vs %.4f' % (a['length'], ret['length']))
    on = [t['name'] for t in a['tracks']]; rn = [t['name'] for t in ret['tracks']]
    if on != rn: issues.append('tracks differ (%d vs %d)' % (len(on), len(rn))); return issues
    et = er = 0; nk = 0
    for o, r in zip(a['tracks'], ret['tracks']):
        if o['flags'] != r['flags']: issues.append('%s flags %d vs %d' % (o['name'], o['flags'], r['flags']))
        if len(o['keys']) != len(r['keys']): issues.append('%s keys %d vs %d' % (o['name'], len(o['keys']), len(r['keys']))); continue
        for k, rk in zip(o['keys'], r['keys']):
            if abs(k[0] - rk['time']) > 1e-4: issues.append('%s key time %.4f vs %.4f' % (o['name'], k[0], rk['time'])); break
            et = max(et, np.abs(np.array(k[1:4]) - rk['trans']).max())
            q = np.array(k[4:8]); rq = np.array(rk['rot'])
            er = max(er, min(np.abs(q - rq).max(), np.abs(q + rq).max())); nk += 1
    if et > 1e-3: issues.append('trans err %.3g' % et)
    if er > 1e-3: issues.append('rot err %.3g' % er)
    return issues
if __name__ == '__main__':
    files = sorted(glob.glob(G + '/**/*.[fF][bB][xX]', recursive=True))
    stats = {'ok': 0, 'bad': 0, 'nocache': 0, 'error': 0}; bad = []
    for f in files:
        msh, anm = f.rsplit('.', 1)[0] + '.msh', f.rsplit('.', 1)[0] + '.anm'
        rel = os.path.relpath(f, G)
        if os.path.exists(anm): mode, ret = 'anim', load_anm(anm)
        elif os.path.exists(msh): mode, ret = 'mesh', load_msh(msh)
        else:
            o = run(f, 'mesh'); stats['nocache'] += 1
            print('NOCACHE', rel, 'error' if 'error' in o else 'loads (%d subs, %d bones)' % (len(o['subs']), len(o['bones'])), o.get('warnings', '')); continue
        o = run(f, mode)
        if 'error' in o: stats['error'] += 1; print('ERROR', rel, o['error']); continue
        iss = cmp_mesh(o, ret) if mode == 'mesh' else cmp_anim(o, ret)
        if iss: stats['bad'] += 1; bad.append((rel, iss)); print('DIFF ', rel, '|', '; '.join(iss[:6]))
        else: stats['ok'] += 1
        if o.get('warnings'): print('   warnings:', o['warnings'][:3])
    print(stats)
