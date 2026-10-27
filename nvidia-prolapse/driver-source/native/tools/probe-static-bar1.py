#!/usr/bin/env python3
"""Compile-only exact-tag public static-BAR1 C oracle, independently m32/m64.
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
OUT = ROOT / 'build/rm-abi-stability/static-bar1'
INCLUDES = ['src/common/sdk/nvidia/inc', 'src/common/inc', 'src/nvidia/arch/nvalloc/unix/include', 'kernel-open/common/inc']
ENV = dict(os.environ, GIT_NO_LAZY_FETCH='1')
TYPE = 'NV2080_CTRL_FB_GET_STATIC_BAR1_INFO_PARAMS'
CMD = 'NV2080_CTRL_CMD_FB_GET_STATIC_BAR1_INFO'


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
    with tempfile.TemporaryDirectory(prefix='rm-static-bar1-') as tmp:
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
        headers = ['ctrl/ctrl2080/ctrl2080fb.h', 'nvos.h', 'nvmisc.h']
        for header in headers:
            copy(header)
        header = (tmp / INCLUDES[0] / headers[0]).read_text()
        present = bool(re.search(r'^\s*#define\s+' + CMD + r'\s', header, re.M))
        fields = []
        if present:
            definition = re.search(r'typedef struct\s+' + TYPE + r'\s*\{(.*?)\}\s*' + TYPE, header, re.S)
            if not definition:
                raise RuntimeError(f'{tag}: command present but unexpected type definition')
            fields = re.findall(r'\b(?:NvBool|NvU64)\s+(\w+)', definition[1])
            assert set(fields) <= {'bStaticBar1Enabled', 'bStaticBar1WriteCombined', 'staticBar1StartOffset', 'staticBar1Size'}, (tag, fields)
            assert 'bStaticBar1Enabled' in fields and 'staticBar1Size' in fields, (tag, fields)
        keys = []
        exprs = []
        def value(key, expression):
            keys.append(key)
            exprs.append(expression)
        if present:
            value(CMD, CMD)
            value(TYPE + '.size', 'sizeof(' + TYPE + ')')
            value(TYPE + '.align', '_Alignof(' + TYPE + ')')
            for field in fields:
                value(TYPE + '.' + field + '.offset', 'offsetof(' + TYPE + ',' + field + ')')
                value(TYPE + '.' + field + '.width', 'sizeof(((' + TYPE + '*)0)->' + field + ')')
        nvos = (tmp / INCLUDES[0] / 'nvos.h').read_text()
        mapping = bool(re.search(r'^\s*#define\s+NVOS33_FLAGS_MAPPING_REFLECTED\s', nvos, re.M))
        if mapping:
            for key, expression in [
                ('NVOS33_FLAGS_MAPPING_REFLECTED', 'NVOS33_FLAGS_MAPPING_REFLECTED'),
                ('NVOS33_FLAGS_MAPPING_REFLECTED_ENCODED', 'DRF_DEF(OS33,_FLAGS,_MAPPING,_REFLECTED)'),
                ('NVOS33_FLAGS_MAPPING_MASK', 'DRF_SHIFTMASK(NVOS33_FLAGS_MAPPING)'),
            ]:
                value(key, expression)
        source = '#include <stddef.h>\n' + ''.join('#include <' + h + '>\n' for h in headers)
        source += 'const unsigned int probe[] __attribute__((section(".barabi"),used)) = {' + ','.join(exprs or ['0']) + '};\n'
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
            subprocess.run(['objcopy', '--dump-section', '.barabi=' + str(binary), str(obj)], check=True)
            values = dict(zip(keys, struct.unpack('<' + 'I' * max(1, len(keys)), binary.read_bytes())))
            outputs[abi] = values
            # Record the scratch directory symbolically so provenance is reproducible.
            commands.append([a.replace(str(tmp), '$TMP') for a in command])
            lines = []
            if present:
                lines += [CMD + '\t' + hex(values[CMD]), TYPE + '\tsize=' + str(values[TYPE + '.size']) + ' align=' + str(values[TYPE + '.align'])]
                lines += [TYPE + '.' + f + '\toff=' + str(values[TYPE + '.' + f + '.offset']) + ' len=' + str(values[TYPE + '.' + f + '.width']) for f in fields]
            else:
                lines += [CMD + '\tMISSING command in exact public header', TYPE + '\tMISSING optional query not declared']
            lines += [k + '\t' + hex(values[k]) for k in keys if k.startswith('NVOS33_')]
            (out / ('probe-' + abi + '.tsv')).write_text('\n'.join(lines) + '\n')
        assert outputs['m32'] == outputs['m64'], tag
        (out / 'source.c').write_text(source)
    proof = {'tag': tag, 'commit': git('rev-parse', tag + '^{commit}').decode().strip(), 'command_present': present,
             'fields': fields, 'headers': used, 'commands': commands, 'values': outputs,
             'compiler': subprocess.check_output(['cc', '--version'], text=True).splitlines()[0],
             'method': 'Compile full exact public headers to m32/m64 ELF data; objcopy section; no executable or GPU'}
    (out / 'provenance.json').write_text(json.dumps(proof, indent=1) + '\n')
    return {'tag': tag, 'present': present, 'fields': fields, 'values': outputs['m64']}


if __name__ == '__main__':
    tags = sorted(p.name for p in (ROOT / 'build/rm-abi-stability/ranges/probe').iterdir() if (p / 'probe-m64.tsv').exists())
    # Census: every published OGKM tag, no more, no less.
    assert tags == sorted(git('tag').decode().split()), 'public tag census differs from the OGKM tag list'
    OUT.mkdir(parents=True, exist_ok=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        results = list(pool.map(one, tags))
    (OUT / 'summary.json').write_text(json.dumps({'exact_tags': len(tags), 'results': results}, indent=1) + '\n')
    print('PASS', len(results), 'exact public tags, independent C m32/m64 compile-only layouts')
    print('present', sum(r['present'] for r in results), 'with WC field', sum('bStaticBar1WriteCombined' in r['fields'] for r in results))
