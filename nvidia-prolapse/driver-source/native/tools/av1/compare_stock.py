#!/usr/bin/env python3
"""Compare native AV1 picture jobs (dump_jobs test output) with stock libnvcuvid captures.
Usage: compare_stock.py <stock capture dir with *.json> <dump dir>"""
import sys, json, hashlib, re
ev, out = sys.argv[1], sys.argv[2]
tot = {}
for name in ['a_640', 'fg_640', 'nocdf_640', 't_1080']:
    j = json.load(open(f'{ev}/{name}.json'))
    summ = [l for l in open(f'{out}/{name}.txt')]
    for f, s in zip(j['frames'], summ):
        n = f['decode_order']
        b = open(f'{out}/{name}_{n:03d}.bin', 'rb').read()
        res = []
        sp = bytes.fromhex(f['pic_s'])
        d = [i for i in range(572) if sp[i] != b[i]]
        res.append('pic_s ' + ('OK' if not d else 'DIFF@' + ','.join(hex(i) for i in d)))
        # word after pic_s (+0x244): stock value is in the raw dumps only; recorded here from them
        ext = {'t_1080': 16}.get(name, 6)
        res.append('ext244 ' + ('OK' if int.from_bytes(b[0x244:0x248], 'little') == ext else 'DIFF'))
        ti = bytes.fromhex(f['tile_info'])
        res.append('tiles ' + ('OK' if b[0x300:0x300 + len(ti)] == ti else 'DIFF'))
        gm = bytes.fromhex(f['global_motion'])
        res.append('gm ' + ('OK' if b[0x1300:0x1300 + len(gm)] == gm else 'DIFF'))
        fg = hashlib.sha256(b[0x1400:0x1400 + 0x3300]).hexdigest()
        res.append('fg ' + ('OK' if fg == f['film_grain_sha256'] else 'DIFF'))
        m = re.search(r'cdf_r=(\d+) cdf_w=(\d+)', s)
        res.append('cdf ' + ('OK' if (int(m[1]), int(m[2])) == (f['cdf_read_index'], f['cdf_write_index']) else
                             f"DIFF ours r{m[1]} w{m[2]} stock r{f['cdf_read_index']} w{f['cdf_write_index']}"))
        for r in res:
            k = r.split()[0]; tot.setdefault(k, [0, 0]); tot[k][0 if r.split()[1] == 'OK' else 1] += 1
        print(name, n, ' | '.join(res))
print('TOTAL', {k: f'{v[0]}/{v[0]+v[1]}' for k, v in tot.items()})

# DPB slot / colMV / segment buffer selection vs stock methods (0x414 pool base, slot = mv + seg size).
def al(v, a): return (v + a - 1) // a * a
mv_ok = mv_n = 0
for name in ['a_640', 'fg_640', 'nocdf_640', 't_1080']:
    j = json.load(open(f'{ev}/{name}.json'))
    w, h = (1920, 1080) if name == 't_1080' else (640, 360)
    mvs = al(al(w, 128) * al(h, 128) // 4096 * 256, 4096); segs = al(al(w, 128) * al(h, 128) // 4096 * 128, 4096)
    for f, s in zip(j['frames'], open(f'{out}/{name}.txt')):
        m = {int(k, 16): int(v, 16) for k, v in f['methods'].items()}; base = m[0x414]
        slot = int(re.search(r'slot=(\d+)', s)[1])
        seg = re.search(r'seg=(\w+(?:\(\d+\))?)', s)[1]
        colmv = re.search(r'colmv=\[([^\]]*)\]', s)[1].split(', ')
        bad = []
        if m[0x65c] - base != slot * (mvs + segs): bad.append('colmv_write')
        if m[0x64c] - base != slot * (mvs + segs) + mvs: bad.append('seg_write')
        exp_seg = 0 if seg == 'None' else int(seg[5:-1]) * (mvs + segs) + mvs
        if m[0x648] - base != exp_seg: bad.append(f'seg_read stock {m[0x648]-base:#x} ours {exp_seg:#x}')
        for jx, c in enumerate(colmv):
            if c != 'None' and m[0x650 + 4 * jx] - base != int(c[5:-1]) * (mvs + segs): bad.append(f'colmv_read{jx}')
        mv_n += 1; mv_ok += not bad
        if bad: print(name, f['decode_order'], 'buffers', bad)
print('buffer selection (colMV/segment write+read, used colMV reads):', f'{mv_ok}/{mv_n}')
