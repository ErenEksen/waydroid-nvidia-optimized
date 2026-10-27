#!/usr/bin/env python3
"""Compile-only exact-tag public ZCULL bind/size C oracle, independently m32/m64.
Reads only prefetched upstream git blobs. No GPU or C executable is run.
"""
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[4]
REPO = ROOT / 'build/open-gpu-kernel-modules'
OUT = ROOT / 'build/rm-abi-stability/zcull'
INCLUDES = ['src/common/sdk/nvidia/inc', 'src/common/inc', 'src/nvidia/arch/nvalloc/unix/include', 'kernel-open/common/inc']
ENV = dict(os.environ, GIT_NO_LAZY_FETCH='1')
# ZCULL contracts: the context-switch buffer used by every graphics channel (stock
# 580/615: FIFO_GET_ENGINE_CONTEXT_PROPERTIES engine GRAPHICS_ZCULL sizes the
# buffer; GR_CTXSW_ZCULL_BIND binds it SEPARATE_BUFFER).
QUERIES = [
    ('ctrl/ctrl0080/ctrl0080fifo.h', 'NV0080_CTRL_CMD_FIFO_GET_ENGINE_CONTEXT_PROPERTIES',
     'NV0080_CTRL_FIFO_GET_ENGINE_CONTEXT_PROPERTIES_PARAMS', {'engineId', 'alignment', 'size'},
     ['NV0080_CTRL_FIFO_GET_ENGINE_CONTEXT_PROPERTIES_ENGINE_ID_GRAPHICS_ZCULL']),
    ('ctrl/ctrl2080/ctrl2080gr.h', 'NV2080_CTRL_CMD_GR_CTXSW_ZCULL_BIND',
     'NV2080_CTRL_GR_CTXSW_ZCULL_BIND_PARAMS', {'hClient', 'hChannel', 'vMemPtr', 'zcullMode'},
     ['NV2080_CTRL_CTXSW_ZCULL_MODE_SEPARATE_BUFFER']),
    # ZCULL region geometry for the 3D driver (stock 615 queries it per
    # subdevice at device open; the nouveau kernel's GSP path copies the same
    # ten fields into DRM_NOUVEAU_GET_ZCULL_INFO).
    ('ctrl/ctrl2080/ctrl2080gr.h', 'NV2080_CTRL_CMD_GR_GET_ZCULL_INFO',
     'NV2080_CTRL_GR_GET_ZCULL_INFO_PARAMS',
     {'widthAlignPixels', 'heightAlignPixels', 'pixelSquaresByAliquots', 'aliquotTotal',
      'zcullRegionByteMultiplier', 'zcullRegionHeaderSize', 'zcullSubregionHeaderSize',
      'subregionCount', 'subregionWidthAlignPixels', 'subregionHeightAlignPixels'},
     []),
]


def git(*args):
    return subprocess.check_output(['git', '-C', str(REPO), *args], env=ENV)


def one(tag):
    out = OUT / tag
    out.mkdir(parents=True, exist_ok=True)
    tree = {}
    for rec in git('ls-tree', '-r', '-z', tag, '--', *INCLUDES).split(b'\0'):
        if rec:
            meta, path = rec.split(b'\t', 1)
            if path.endswith(b'.h'):
                tree[path.decode()] = meta.split()[-1].decode()
    used = {}
    with tempfile.TemporaryDirectory(prefix='rm-zcull-') as tmp:
        tmp = Path(tmp)
        def copy(name, parent=''):
            choices = [parent + '/' + name] + [p + '/' + name for p in INCLUDES]
            path = next((p for p in choices if p in tree), None)
            if path is None or path in used:
                return
            raw = git('cat-file', 'blob', tree[path])
            used[path] = {'blob': tree[path], 'sha256': hashlib.sha256(raw).hexdigest()}
            dest = tmp / path
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(raw)
            for inc in re.findall(rb'^\s*#\s*include\s*[<"]([^>"\n]+)', raw, re.M):
                copy(inc.decode(), str(Path(path).parent))
        headers = [q[0] for q in QUERIES] + ['nvos.h', 'nvmisc.h']
        for header in headers:
            copy(header)
        keys = []
        exprs = []
        def value(key, expression):
            keys.append(key)
            exprs.append(expression)
        found = {}
        for path, cmd, typ, allowed, constants in QUERIES:
            header = (tmp / INCLUDES[0] / path).read_text()
            present = bool(re.search(r'^\s*#define\s+' + cmd + r'\s', header, re.M))
            fields = []
            if present:
                definition = re.search(r'typedef struct\s+' + typ + r'\s*\{(.*?)\}\s*' + typ, header, re.S)
                if not definition:
                    raise RuntimeError(f'{tag}: {cmd} present but unexpected type definition')
                fields = re.findall(r'\b(?:NvU32|NvHandle|NvU64)\s+(\w+)', definition[1])
                assert set(fields) == allowed, (tag, typ, fields)
                value(cmd, cmd)
                value(typ + '.size', 'sizeof(' + typ + ')')
                value(typ + '.align', '_Alignof(' + typ + ')')
                for field in fields:
                    value(typ + '.' + field + '.offset', 'offsetof(' + typ + ',' + field + ')')
                    value(typ + '.' + field + '.width', 'sizeof(((' + typ + '*)0)->' + field + ')')
                for constant in constants:
                    value(constant, constant)
            found[cmd] = (typ, fields, constants, present)
        source = '#include <stddef.h>\n' + ''.join('#include <' + h + '>\n' for h in headers)
        source += 'const unsigned int probe[] __attribute__((section(".zcabi"),used)) = {' + ','.join(exprs or ['0']) + '};\n'
        src = tmp / 'probe.c'
        src.write_text(source)
        outputs = {}
        commands = []
        for abi in ['m32', 'm64']:
            obj, binary = tmp / (abi + '.o'), tmp / (abi + '.bin')
            command = ['cc', '-' + abi, '-std=c11', '-Wall', '-Werror', '-ffreestanding', *['-I' + str(tmp / p) for p in INCLUDES], '-c', str(src), '-o', str(obj)]
            result = subprocess.run(command, capture_output=True, text=True)
            if result.returncode:
                raise RuntimeError(tag + ': ' + result.stderr)
            subprocess.run(['objcopy', '--dump-section', '.zcabi=' + str(binary), str(obj)], check=True)
            values = dict(zip(keys, struct.unpack('<' + 'I' * max(1, len(keys)), binary.read_bytes())))
            outputs[abi] = values
            # Record the scratch directory symbolically so provenance is reproducible.
            commands.append([a.replace(str(tmp), '$TMP') for a in command])
            lines = []
            for cmd, (typ, fields, constants, present) in found.items():
                if present:
                    lines += [cmd + '\t' + hex(values[cmd]), typ + '\tsize=' + str(values[typ + '.size']) + ' align=' + str(values[typ + '.align'])]
                    lines += [typ + '.' + f + '\toff=' + str(values[typ + '.' + f + '.offset']) + ' len=' + str(values[typ + '.' + f + '.width']) for f in fields]
                    lines += [c + '\t' + hex(values[c]) for c in constants]
                else:
                    lines += [cmd + '\tMISSING command in exact public header', typ + '\tMISSING not declared']
            (out / ('probe-' + abi + '.tsv')).write_text('\n'.join(lines) + '\n')
        assert outputs['m32'] == outputs['m64'], tag
        (out / 'source.c').write_text(source)
    proof = {'tag': tag, 'commit': git('rev-parse', tag + '^{commit}').decode().strip(),
             'queries': {c: {'type': v[0], 'fields': v[1], 'present': v[3]} for c, v in found.items()}, 'headers': used, 'commands': commands, 'values': outputs,
             'compiler': subprocess.check_output(['cc', '--version'], text=True).splitlines()[0],
             'method': 'Compile full exact public headers to m32/m64 ELF data; objcopy section; no executable or GPU'}
    (out / 'provenance.json').write_text(json.dumps(proof, indent=1) + '\n')
    return {'tag': tag, 'present': {c: v[3] for c, v in found.items()}, 'values': outputs['m64']}


if __name__ == '__main__':
    tags = sorted(p.name for p in (ROOT / 'build/rm-abi-stability/ranges/probe').iterdir() if (p / 'probe-m64.tsv').exists())
    # Census: every published OGKM tag, no more, no less.
    assert tags == sorted(git('tag').decode().split()), 'public tag census differs from the OGKM tag list'
    OUT.mkdir(parents=True, exist_ok=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        results = list(pool.map(one, tags))
    (OUT / 'summary.json').write_text(json.dumps({'exact_tags': len(tags), 'results': results}, indent=1) + '\n')
    print('PASS', len(results), 'exact public tags, independent C m32/m64 compile-only layouts')
    for q in QUERIES:
        print(q[1], 'present in', sum(r['present'][q[1]] for r in results))
