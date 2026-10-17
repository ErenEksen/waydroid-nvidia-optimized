#!/usr/bin/env python3
"""Compare two ext4 vendor images file by file, without root or mounting (debugfs only).

Usage: compare-vendor.py REFERENCE.img BUILT.img [--report OUT.md] [--ignore-prop-file PATH ...]

For every path in either image: type, mode, uid/gid, SELinux label (security.selinux xattr), symlink target and
the SHA-256 of regular files. Differing ELF files are classified further: same DT_NEEDED + same dynamic symbol
set ("same ABI, code differs") or not. build.prop-style files are compared line by line with the build
date/fingerprint/incremental lines dropped (they always differ between two builds).
ELF files whose only difference is the .note.gnu.build-id section form their own class (elf-build-id-only).
Exit status: 0 when the images match under those rules, 1 otherwise (the report lists everything).
"""
import argparse
import hashlib
import os
import re
import subprocess
import sys
import tempfile

VOLATILE_PROP = re.compile(r"^ro\.(vendor\.|odm\.|vendor_dlkm\.|odm_dlkm\.)?build\.(date|date\.utc|fingerprint|"
                           r"version\.incremental|thumbprint)=|^# from |^ro\.(vendor\.)?build\.description=")


def debugfs(image, *requests):
    with tempfile.NamedTemporaryFile("w", suffix=".cmds", delete=False) as f:
        f.write("\n".join(requests) + "\n")
        name = f.name
    try:
        r = subprocess.run(["debugfs", "-f", name, image], capture_output=True)
    finally:
        os.unlink(name)
    return r.stdout.decode(errors="replace")


def walk(image):
    """{path: (type, mode, uid, gid)} from recursive `ls -p` listings."""
    entries, todo = {}, ["/"]
    while todo:
        d = todo.pop()
        out = debugfs(image, "ls -p " + d)
        for line in out.splitlines():
            parts = line.split("/")
            if len(parts) < 7 or parts[5] in ("", ".", "..", "lost+found"):
                continue
            mode = int(parts[2], 8)
            path = (d.rstrip("/") + "/" + parts[5])
            kind = {0o040000: "dir", 0o100000: "file", 0o120000: "link"}.get(mode & 0o170000, "other")
            entries[path] = (kind, mode & 0o7777, int(parts[3]), int(parts[4]))
            if kind == "dir":
                todo.append(path)
    return entries


def labels_and_links(image, entries):
    reqs = []
    for p, (kind, *_rest) in entries.items():
        reqs.append("ea_get {} security.selinux".format(p))
    out = debugfs(image, *reqs)
    labels, cur = {}, None
    for line in out.splitlines():
        m = re.match(r"debugfs: ea_get (\S+) security\.selinux", line)
        if m:
            cur = m.group(1)
            continue
        v = re.search(r'=\s*"(.*?)(?:\\000)?"\s*$', line)
        if cur and v:
            labels[cur] = v.group(1)
            cur = None
    links = {}
    lreqs = [p for p, e in entries.items() if e[0] == "link"]
    out = debugfs(image, *("stat " + p for p in lreqs))
    cur = None
    for line in out.splitlines():
        m = re.match(r"debugfs: stat (\S+)", line)
        if m:
            cur = m.group(1)
            continue
        m = re.match(r'Fast link dest: "(.*)"', line.strip())
        if cur and m:
            links[cur] = m.group(1)
    return labels, links


def dump_files(image, entries, dest):
    debugfs(image, "rdump / " + dest)
    root = os.path.join(dest, "")
    hashes = {}
    for p, e in entries.items():
        if e[0] == "file":
            with open(root + p.lstrip("/"), "rb") as f:
                hashes[p] = hashlib.sha256(f.read()).hexdigest()
    return hashes


def elf_summary(path):
    def run(*cmd):
        return subprocess.run(cmd, capture_output=True, text=True).stdout
    needed = sorted(re.findall(r"\(NEEDED\).*\[(.*)\]", run("readelf", "-d", path)))
    syms = sorted(set(l.split()[-1] for l in run("nm", "-D", "--defined-only", path).splitlines() if l.strip()))
    return needed, syms


def without_build_id(path):
    """File bytes with the .note.gnu.build-id section zeroed. Soong links with --build-id=md5 over the unstripped
    output, so a different OUT_DIR (paths in debug info that stripping removes) changes only these 16 bytes."""
    data = bytearray(open(path, "rb").read())
    out = subprocess.run(["readelf", "-W", "-S", path], capture_output=True, text=True).stdout
    for line in out.splitlines():
        if ".note.gnu.build-id" in line:
            f = line.split("]", 1)[1].split()
            off, size = int(f[3], 16), int(f[4], 16)
            data[off:off + size] = bytes(size)
    return bytes(data)


def is_elf(path):
    with open(path, "rb") as f:
        return f.read(4) == b"\x7fELF"


def prop_lines(path):
    with open(path, errors="replace") as f:
        return [l.rstrip("\n") for l in f if not VOLATILE_PROP.match(l)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("reference")
    ap.add_argument("built")
    ap.add_argument("--report")
    a = ap.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        sides = {}
        for tag, img in (("ref", a.reference), ("new", a.built)):
            e = walk(img)
            labels, links = labels_and_links(img, e)
            d = os.path.join(tmp, tag)
            os.makedirs(d)
            sides[tag] = dict(entries=e, labels=labels, links=links, hashes=dump_files(img, e, d), root=d)
        ref, new = sides["ref"], sides["new"]
        only_ref = sorted(set(ref["entries"]) - set(new["entries"]))
        only_new = sorted(set(new["entries"]) - set(ref["entries"]))
        rows = {"identical": [], "meta": [], "prop": [], "elf-build-id-only": [], "elf-same-abi": [], "elf-abi": [],
            "content": []}
        for p in sorted(set(ref["entries"]) & set(new["entries"])):
            er, en = ref["entries"][p], new["entries"][p]
            notes = []
            if er != en:
                notes.append("type/mode/uid/gid {} -> {}".format(er, en))
            if ref["labels"].get(p) != new["labels"].get(p):
                notes.append("label {} -> {}".format(ref["labels"].get(p), new["labels"].get(p)))
            if er[0] == "link" and ref["links"].get(p) != new["links"].get(p):
                notes.append("link {} -> {}".format(ref["links"].get(p), new["links"].get(p)))
            if er[0] == en[0] == "file" and ref["hashes"][p] != new["hashes"][p]:
                fr, fn = ref["root"] + p, new["root"] + p
                if p.endswith(".prop"):
                    if prop_lines(fr) != prop_lines(fn):
                        rows["prop"].append((p, "non-volatile property lines differ"))
                    elif notes:
                        rows["meta"].append((p, "; ".join(notes)))
                    else:
                        rows["identical"].append((p, "volatile build props only"))
                    continue
                if is_elf(fr) and is_elf(fn):
                    if without_build_id(fr) == without_build_id(fn):
                        rows["elf-build-id-only"].append((p, "; ".join(notes) or "only .note.gnu.build-id differs"))
                        continue
                    same = elf_summary(fr) == elf_summary(fn)
                    rows["elf-same-abi" if same else "elf-abi"].append((p, "; ".join(notes) or "code differs"))
                else:
                    rows["content"].append((p, "; ".join(notes) or "bytes differ"))
                continue
            (rows["meta"] if notes else rows["identical"]).append((p, "; ".join(notes)))
    out = ["# vendor image comparison", "", "reference: `{}`".format(a.reference), "built: `{}`".format(a.built), ""]
    out.append("| class | count |")
    out.append("|---|---|")
    for k, v in rows.items():
        out.append("| {} | {} |".format(k, len(v)))
    out.append("| only in reference | {} |".format(len(only_ref)))
    out.append("| only in built | {} |".format(len(only_new)))
    for title, items in [("only in reference", [(p, "") for p in only_ref]), ("only in built", [(p, "") for p in only_new])] + \
            [(k, v) for k, v in rows.items() if k not in ("identical", "elf-build-id-only")]:
        if items:
            out += ["", "## " + title, ""] + ["- `{}` {}".format(p, n) for p, n in items]
    text = "\n".join(out) + "\n"
    if a.report:
        with open(a.report, "w") as f:
            f.write(text)
    print(text if len(text) < 20000 else text[:20000] + "\n... (truncated; see --report)")
    bad = only_ref or only_new or any(rows[k] for k in rows if k not in ("identical", "elf-build-id-only"))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
