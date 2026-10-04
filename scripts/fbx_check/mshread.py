import struct, sys
MAGIC = 0x76034569
class R:
    def __init__(s, b): s.b, s.p = b, 0
    def i32(s): v = struct.unpack_from('<i', s.b, s.p)[0]; s.p += 4; return v
    def i16(s): v = struct.unpack_from('<h', s.b, s.p)[0]; s.p += 2; return v
    def f32(s): v = struct.unpack_from('<f', s.b, s.p)[0]; s.p += 4; return v
    def boolean(s): v = s.b[s.p] != 0; s.p += 1; return v
    def fa(s, n): v = struct.unpack_from('<%df' % n, s.b, s.p); s.p += 4*n; return list(v)
    def ia(s, n): v = struct.unpack_from('<%di' % n, s.b, s.p); s.p += 4*n; return list(v)
    def ba(s, n): v = s.b[s.p:s.p+n]; s.p += n; return v
    def string(s):
        e = s.b.index(b'\0', s.p); v = s.b[s.p:e].decode('latin1'); s.p = e+1; return v
def bone(r, out, parent):
    name = r.string(); sid = r.string(); m = r.fa(16); n = r.i32()
    out.append(dict(name=name, sid=sid, m=m, parent=parent)); me = len(out)-1
    for _ in range(n): bone(r, out, me)
def node(r, out, parent):
    name = r.string(); m = r.fa(16); flags = r.i32(); n = r.i32()
    out.append(dict(name=name, m=m, parent=parent))
    for _ in range(n): node(r, out, len(out)-1)
def anim(r):
    a = dict(name=r.string(), length=r.f32(), tracks=[])
    for _ in range(r.i32()):
        t = dict(name=r.string(), flags=r.i16(), keys=[])
        for _ in range(r.i32()):
            t['keys'].append(dict(time=r.f32(), trans=r.fa(3), rot=r.fa(4)))  # rot = x y z w
        a['tracks'].append(t)
    return a
def load_msh(path):
    r = R(open(path, 'rb').read())
    assert r.i32() == MAGIC; ver = r.i32()
    nsub = r.i32(); nanim = r.i32(); skel = r.boolean()
    m = dict(version=ver, bones=[], nodes=[], subs=[], anims=[])
    if skel:
        for _ in range(r.i32()): bone(r, m['bones'], -1)
    for _ in range(r.i32()): node(r, m['nodes'], -1)
    for _ in range(nsub):
        s = dict(name=r.string(), material=r.string(), mtx=r.fa(16), scale=r.fa(3), collide=r.boolean(), colliders=[])
        for _ in range(r.i32()):
            s['colliders'].append((r.i16(), r.fa(16), r.fa(3), r.boolean()))
        s['pairs'] = [(r.i32(), r.i32(), r.f32()) for _ in range(r.i32())]
        nv = r.i32(); nt = r.i32(); s['nv'] = nv; s['arrays'] = {}
        for _ in range(nt):
            typ = r.i16(); fmt = r.i16(); pvi = r.i32(); ne = r.i32()
            cnt = nv*ne
            data = r.fa(cnt) if fmt == 0 else (r.ia(cnt) if fmt == 1 else r.ba(cnt))
            s['arrays'][typ] = (fmt, ne, data)
        s['idx'] = r.ia(r.i32())
        m['subs'].append(s)
    for _ in range(r.i32()): m['anims'].append(anim(r))
    return m
def load_anm(path):
    r = R(open(path, 'rb').read()); assert r.i32() == MAGIC; r.i32(); return anim(r)
if __name__ == '__main__':
    p = sys.argv[1]
    if p.lower().endswith('.anm'):
        a = load_anm(p); print('anim', a['name'], a['length'], len(a['tracks']))
        for t in a['tracks'][:5]: print(' ', t['name'], t['flags'], len(t['keys']), t['keys'][0] if t['keys'] else '')
    else:
        m = load_msh(p)
        print('bones', len(m['bones']), 'nodes', len(m['nodes']), 'subs', len(m['subs']), 'anims', len(m['anims']))
        for b in m['bones'][:4]: print('  bone', b['name'], [round(x,3) for x in b['m']])
        for s in m['subs']:
            vt = sorted({p[0] for p in s['pairs']})
            print('  sub', repr(s['name']), 'mat', repr(s['material']), 'nv', s['nv'], 'idx', len(s['idx']), 'pairs', len(s['pairs']), 'verts_with_pairs', len(vt), 'arrays', {k:(v[0],v[1]) for k,v in s['arrays'].items()}, 'mtx_ident', s['mtx']==[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1])
