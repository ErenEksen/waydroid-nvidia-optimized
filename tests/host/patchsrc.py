"""Source text of our own patch code without the upstream trees.

The Mesa and virglrenderer patches are applied to upstream sources the tests cannot assume are present. Every line a
patch adds is in the patch, so code that a patch adds whole (functions, structs) can be rebuilt from it: the hunks'
old-side lines are laid out at their line numbers in a synthetic pre-image (unknown lines are unique fillers), the
patch is applied with `git apply`, and the post-image holds the added code verbatim. A patch that does not apply to
its own hunks' pre-image fails here, so this also checks hunk headers and context.
"""
from pathlib import Path
import re
import subprocess
import tempfile

HUNK = re.compile(r'^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@')


def synthesize(patches, root):
    """Write the pre-image every patch in `patches` agrees on under `root`."""
    files = {}
    for patch in patches:
        cur = line = None
        for text in Path(patch).read_text().split('\n'):
            m = re.match(r'^diff --git a/(\S+) ', text)
            if m:
                cur, line = m.group(1), None
                files.setdefault(cur, {})
                continue
            m = HUNK.match(text)
            if m:
                line = int(m.group(1))
                continue
            if line is None or cur is None:
                continue
            if text.startswith((' ', '-')):
                known = files[cur].get(line)
                if known is not None and known != text[1:]:
                    raise ValueError(f'{patch}: {cur}:{line} disagrees with another patch')
                files[cur][line] = text[1:]
                line += 1
            elif not text.startswith(('+', '\\')):
                line = None
    for name, lines in files.items():
        if not lines:
            continue  # created by the patch
        path = Path(root, name)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(''.join(lines.get(i, f'/* filler {i} */') + '\n' for i in range(1, max(lines) + 4)))


def post_image(patch, path, workdir=None):
    """The text of `path` after `patch`, applied to its own synthetic pre-image."""
    with tempfile.TemporaryDirectory(prefix='nvwd-patchsrc-') as tmp:
        root = Path(tmp)
        synthesize([patch], root)
        subprocess.run(['git', 'init', '-q', str(root)], check=True)
        subprocess.run(['git', 'apply', '--whitespace=nowarn', str(Path(patch).resolve())], cwd=root, check=True)
        return (root / path).read_text()


def c_block(source, head):
    """The C definition that starts with the line `head` (exact), through its closing brace (and `;` for types)."""
    lines = source.split('\n')
    try:
        start = lines.index(head)
    except ValueError:
        raise LookupError(f'definition not found: {head!r}') from None
    # a function's signature may span lines; the body starts at the first '{'
    depth, seen, out = 0, False, []
    for text in lines[start:]:
        out.append(text)
        for ch in text:
            if ch == '{':
                depth += 1
                seen = True
            elif ch == '}':
                depth -= 1
        if seen and depth == 0:
            return '\n'.join(out) + '\n'
    raise LookupError(f'unterminated definition: {head!r}')


def c_lines(source, first, last):
    """Lines from the one equal to `first` through the one equal to `last`."""
    lines = source.split('\n')
    a = lines.index(first)
    b = lines.index(last, a)
    return '\n'.join(lines[a:b + 1]) + '\n'
