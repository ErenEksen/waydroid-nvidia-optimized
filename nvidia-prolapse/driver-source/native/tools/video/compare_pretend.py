#!/usr/bin/env python3
"""Compare our pretend-class builds (native test `video::pretend::dump_pretend`) with stock rmtrace
captures of the same streams (vast-nvdec bundle `*.trace.gz`, one per stream).

usage: compare_pretend.py <ours-dir> <traces-dir> [stream ...]
       compare_pretend.py --diff <ours-A> <traces-A> <ours-B> <traces-B> [stream ...]

The --diff form is the per-generation check: for every picture and descriptor byte where stock on class A
differs from stock on class B, ours built for A must differ from ours built for B the same way (and nowhere
else). Harness artefacts common to both captures (submit-time dumps of engine-written regions, caller
surface choice) cancel out.

Per picture, in decode order:
* method list of the decoder class (subchannel 4): method, header form (INC/NONINC) and count must match;
  immediates (0x200 application id, 0x400 control params, 0x40c picture index) must be equal; buffer
  addresses are compared as descriptor- / bitstream-relative offsets, every other address through a
  per-picture bijection (stock and our buffers differ, which methods share a buffer must not). Stock's
  decoder-class semaphore methods (0x010 acquire, 0x240/0x244/0x248/0x304 release) are listed apart: our
  channel orders pictures with host-class semaphores instead.
* descriptor bytes where stock dumped them (PICSETUP/DESC, VP9 and AV1): every differing byte offset.
  VP9 pictures after one that needed backward adaptation counts compare 0..0x900 only (the pretend run
  has no engine counts, so its probability tables diverge by construction).
"""
import gzip
import os
import re
import struct
import sys

SEM = {0x010, 0x05c, 0x240, 0x244, 0x248, 0x304}


def stock_pictures(path):
    lines = gzip.open(path, 'rt').read().split('\n')
    pics, i = [], 0
    while i < len(lines):
        m = re.match(r'T \d+ GPFIFO chan (\S+) class (\S+) engine (\S+) .* words (\d+)', lines[i])
        if m and m.group(3) != '0' and i + 1 < len(lines) and lines[i + 1] == 'PB':
            words, j = [], i + 2
            while j < len(lines) and lines[j].startswith('  '):
                words += [int(x, 16) for x in lines[j].split()]
                j += 1
            desc = None
            if j < len(lines) and lines[j].startswith('PICSETUP'):
                k, d = j + 2, []
                while k < len(lines) and lines[k].startswith('  '):
                    d += [int(x, 16) for x in lines[k].split()]
                    k += 1
                desc, j = struct.pack('<%dI' % len(d), *d), k
            ms = decode(words)
            if any(m == 0x200 and s == 4 for m, _, s, _ in ms):
                pics.append((ms, desc))
            i = j
            continue
        i += 1
    return pics


def decode(words):
    r, i = [], 0
    while i < len(words):
        h = words[i]
        typ, cnt, sub, m = h >> 29, (h >> 16) & 0x1fff, (h >> 13) & 7, (h & 0x1fff) << 2
        if typ == 4:
            r.append((m, 'IMM', sub, [cnt]))
            i += 1
            continue
        r.append((m, {1: 'INC', 3: 'NONINC', 5: 'ONE'}.get(typ, str(typ)), sub, words[i + 1:i + 1 + cnt]))
        i += 1 + cnt
    return r


def va_of(kind, vals):
    if kind == 'NONINC' and len(vals) == 2:
        return (vals[0] << 8) | (vals[1] << 40)
    return vals[0] << 8


IMMEDIATE = {0x200, 0x400, 0x40c, 0x300}


def compare_methods(stock, ours, n, out):
    s = [x for x in stock if x[2] == 4 and x[0] not in SEM and x[0] != 0]
    o = [x for x in ours if x[2] == 4]
    sig = lambda l: [(m, k, len(v)) for m, k, _, v in l]
    if sig(s) != sig(o):
        out.append('pic %d: method list differs\n  stock %s\n  ours  %s' % (
            n, ' '.join('%x/%s%d' % t for t in sig(s)), ' '.join('%x/%s%d' % t for t in sig(o))))
        return
    def rel(l):
        d = va_of(*[(k, v) for m, k, _, v in l if m == 0x404][0])
        b = va_of(*[(k, v) for m, k, _, v in l if m == 0x408][0])
        return d, b
    (sd, sb), (od, ob) = rel(s), rel(o)
    fwd, back = {}, {}
    for (m, k, _, sv), (_, _, _, ov) in zip(s, o):
        if m in IMMEDIATE:
            if sv != ov:
                out.append('pic %d: method %#x value stock %s ours %s' % (
                    n, m, ' '.join('%#x' % x for x in sv), ' '.join('%#x' % x for x in ov)))
            continue
        a, b = va_of(k, sv), va_of(k, ov)
        if 0 <= a - sd < 0x5000 or 0 <= b - od < 0x5000:
            if a - sd != b - od:
                out.append('pic %d: method %#x desc+%#x (stock) vs desc+%#x (ours)' % (n, m, a - sd, b - od))
            continue
        if 0 <= a - sb < 0x100 or 0 <= b - ob < 0x100:
            if a - sb != b - ob:
                out.append('pic %d: method %#x bitstream offset differs' % (n, m))
            continue
        if fwd.setdefault(a, b) != b or back.setdefault(b, a) != a:
            out.append('pic %d: method %#x buffer identity differs (stock %#x, ours %#x)' % (n, m, a, b))


def compare_desc(stock, ours, n, limit, out):
    stock = stock[:limit]
    diff = [i for i in range(len(stock)) if stock[i] != ours[i]]
    if diff:
        spans, start = [], diff[0]
        for a, b in zip(diff, diff[1:] + [None]):
            if b != a + 1:
                spans.append((start, a))
                start = b
        out.append('pic %d: descriptor differs at %s' % (n, ', '.join(
            '%#x[%s→%s]' % (a, stock[a:b + 1].hex(), ours[a:b + 1].hex()) for a, b in spans[:12])))


def load_ours(d, name):
    files = sorted(f for f in os.listdir(os.path.join(d, name)) if f.endswith('.desc'))
    return [open(os.path.join(d, name, f), 'rb').read() for f in files]


def diff_main(args):
    oa, ta, ob, tb = args[:4]
    names = args[4:] or sorted(os.listdir(oa))
    bad = 0
    for name in names:
        sa = stock_pictures(os.path.join(ta, name + '.trace.gz'))
        sb = stock_pictures(os.path.join(tb, name + '.trace.gz'))
        da, db = load_ours(oa, name), load_ours(ob, name)
        stock_d, ours_d, n_cmp = {}, {}, 0
        # Stock captures dump a ring slot's setup at submit; pair them with our pictures by the
        # stream_len field (VP9 +0x30, AV1 +0x50) instead of trusting capture order.
        key_off = 0x30 if name.startswith('vp9') else 0x50
        key = lambda d: d[key_off:key_off + 4]
        def pair(stock):
            used, m = set(), {}
            for n, d in enumerate(da):
                for j, (_, x) in enumerate(stock):
                    if j not in used and x is not None and key(x) == key(d):
                        used.add(j)
                        m[n] = x
                        break
            return m
        ma, mb = pair(sa), pair(sb)
        for n in range(min(len(da), len(db))):
            xa, xb = ma.get(n), mb.get(n)
            if xa is None or xb is None:
                print('  pic %d: no stock picture with the same stream_len in %s' % (
                    n, 'A' if xa is None else 'B'))
                continue
            n_cmp += 1
            lim = min(len(xa), len(xb), 0x900)  # pic_s + tile table (probability/count regions excluded)
            for i in range(lim):
                if xa[i] != xb[i]:
                    stock_d[(n, i)] = (xa[i], xb[i])
                if da[n][i] != db[n][i]:
                    ours_d[(n, i)] = (da[n][i], db[n][i])
        only_stock = sorted(set(stock_d) - set(ours_d))
        only_ours = sorted(set(ours_d) - set(stock_d))
        value = sorted(k for k in set(stock_d) & set(ours_d) if stock_d[k] != ours_d[k])
        offs = lambda ks: sorted({'%#x' % i for _, i in ks})
        print('== %s: %d descriptor pairs; stock A/B differ at %s; ours A/B differ at %s' % (
            name, n_cmp, offs(stock_d.keys()) or '-', offs(ours_d.keys()) or '-'))
        for label, ks in (('only stock', only_stock), ('only ours', only_ours), ('different values', value)):
            if ks:
                bad += len(ks)
                print('  %s: %d bytes at offsets %s' % (label, len(ks), offs(ks)))
                for k in ks[:4]:
                    print('    pic %d +%#x stock %s ours %s' % (k[0], k[1], stock_d.get(k), ours_d.get(k)))
        if not (only_stock or only_ours or value) and n_cmp:
            print('  generation differences identical (%d byte pairs)' % len(stock_d))
    sys.exit(1 if bad else 0)


def main():
    if sys.argv[1] == '--diff':
        return diff_main(sys.argv[2:])
    ours_dir, traces = sys.argv[1], sys.argv[2]
    names = sys.argv[3:] or sorted(os.listdir(ours_dir))
    total_bad = 0
    for name in names:
        stock = stock_pictures(os.path.join(traces, name + '.trace.gz'))
        files = sorted(f for f in os.listdir(os.path.join(ours_dir, name)) if f.endswith('.words'))
        out, adapted, used, paired = [], False, set(), 0
        sems = set()
        for n, f in enumerate(files):
            base = os.path.join(ours_dir, name, f[:-6])
            words = open(base + '.words', 'rb').read()
            ours = decode(list(struct.unpack('<%dI' % (len(words) // 4), words)))
            desc = open(base + '.desc', 'rb').read()
            readback = 'readback true' in open(base + '.meta').read()
            if n >= len(stock):
                out.append('pic %d: stock has no picture' % n)
                continue
            sm, _ = stock[n]
            sems |= {(m, k, len(v)) for m, k, s, v in sm if s == 4 and m in SEM}
            compare_methods(sm, ours, n, out)
            # Descriptor dumps are paired by stream_len (VP9 +0x30, AV1 +0x50): a capture's dump does not
            # always belong to the picture of the same push.
            ko = 0x30 if name.startswith('vp9') else 0x50
            sd = next((d for j, (_, d) in enumerate(stock) if d is not None and j not in used
                       and d[ko:ko + 4] == desc[ko:ko + 4]), None)
            if sd is not None:
                used.add(next(j for j, (_, d) in enumerate(stock) if d is sd))
                paired += 1
                compare_desc(sd, desc, n, 0x900 if adapted else len(sd), out)
            adapted |= readback
        if len(stock) != len(files):
            out.append('picture count: stock %d, ours %d' % (len(stock), len(files)))
        print('== %s: %d pictures (%d descriptor dumps paired), %d differences; stock class semaphores %s%s' % (
            name, len(files), paired, len(out), sorted('%x/%s%d' % s for s in sems),
            ' (VP9 probs not compared after the first adapted frame)' if adapted else ''))
        for l in out[:40]:
            print('  ' + l)
        total_bad += len(out)
    sys.exit(1 if total_bad else 0)


if __name__ == '__main__':
    main()
