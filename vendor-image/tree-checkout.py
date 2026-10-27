#!/usr/bin/env python3
"""Put an already synced LineageOS 20 + Waydroid tree at the exact revisions of a pinned manifest or a recorded state.

Usage: tree-checkout.py TREE SPEC [--check | --verify | --record OUT] [-j N]

SPEC is a pinned manifest (.xml, written by pin-manifest.py: every branch-tracked project carries a full SHA, AOSP
projects keep their tag) or a state file (.tsv, written by --record: "path<TAB>commit" per project, '#' lines are
comments). For every project of SPEC this resets the project's working tree to that revision: tracked files are
restored (`checkout -f --detach` + `reset --hard`), so earlier patch commits (Waydroid's base-patches, our own) and
stray edits are gone. Projects already at the revision with no tracked changes are not touched at all (their files
keep their mtimes, so the Android build does not regenerate or rebuild them). A manifest revision that is not present
locally is fetched by SHA (GitHub/googlesource accept full SHAs); a state file's commit must be present locally.
Untracked files are left alone on purpose: repo's <linkfile>s live inside project directories
(external/chromium-webview/Android.mk, ...) and `git clean` would delete them.

--check  only reports which projects differ (no fetch, no checkout).
--verify like --check, but exits 3 when any project differs (build-vendor.sh: is the tree still in a stamped state?).
--record writes the current HEAD of every SPEC project to OUT (atomically; no checkout).
A project path from SPEC that does not exist in the tree is an error: this tool updates an existing tree,
it does not replace `repo sync` for new projects (no silent partial tree).
Afterwards the caller applies Waydroid's patches (vendor/extra/waydroid-patches/apply-patches.sh).
"""
import concurrent.futures
import os
import subprocess
import sys
import urllib.parse
import xml.etree.ElementTree as ET

MANIFEST_URL = "https://github.com/LineageOS/android"   # repo init -u (resolves fetch=".." remotes)


def git(path, *args, check=True):
    r = subprocess.run(["git", "-C", path] + list(args), capture_output=True, text=True)
    if check and r.returncode != 0:
        raise RuntimeError("git -C {} {}: {}".format(path, " ".join(args), r.stderr.strip()))
    return r


def projects(xml_path):
    root = ET.parse(xml_path).getroot()
    default = root.find("default")
    remotes = {r.get("name"): r for r in root.findall("remote")}
    for p in root.findall("project"):
        remote = remotes[p.get("remote") or default.get("remote")]
        rev = p.get("revision") or remote.get("revision") or default.get("revision")
        base = urllib.parse.urljoin(MANIFEST_URL, remote.get("fetch"))
        url = base.rstrip("/") + "/" + p.get("name")
        yield p.get("path") or p.get("name"), rev, url


def state_projects(tsv_path):
    """(path, commit, None) per line of a --record state file."""
    with open(tsv_path) as f:
        for line in f:
            if line.strip() and not line.startswith("#"):
                path, sha = line.rstrip("\n").split("\t")
                yield path, sha, None


def resolve(path, rev):
    """Commit SHA of rev inside the project, or None when it is not present locally."""
    r = git(path, "rev-parse", "-q", "--verify", rev + "^{commit}", check=False)
    return r.stdout.strip() if r.returncode == 0 else None


def one(tree, path, rev, url, check_only):
    full = tree + "/" + path
    head = git(full, "rev-parse", "HEAD").stdout.strip()
    want = resolve(full, rev)
    dirty = git(full, "status", "--porcelain", "--untracked-files=no").stdout.strip() != ""
    if want and head == want and not dirty:
        return path, "ok", want
    if check_only:
        return path, "differs" if want else "missing-rev", want or rev
    if not want:
        if url is None:
            raise RuntimeError("{}: recorded commit {} is not present locally".format(path, rev))
        git(full, "fetch", "-q", url, rev)
        want = resolve(full, "FETCH_HEAD")
        if rev.startswith("refs/tags/"):
            git(full, "tag", "-f", rev[len("refs/tags/"):], want)
        elif want != rev:
            raise RuntimeError("{}: fetched {} but got {}".format(path, rev, want))
    git(full, "checkout", "-q", "-f", "--detach", want)
    git(full, "reset", "-q", "--hard", want)
    return path, "reset" if head != want else "cleaned", want


def record(tree, plist, out, jobs):
    with concurrent.futures.ThreadPoolExecutor(jobs) as ex:
        heads = list(ex.map(lambda p: (p, git(tree + "/" + p, "rev-parse", "HEAD").stdout.strip()),
                            [p for p, _, _ in plist]))
    with open(out + ".tmp", "w") as f:
        f.writelines("{}\t{}\n".format(p, h) for p, h in heads)
    os.replace(out + ".tmp", out)
    print("recorded {} projects in {}".format(len(heads), out))


def main(argv):
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("tree")
    ap.add_argument("spec", help="pinned manifest (.xml) or recorded state (.tsv)")
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true")
    mode.add_argument("--verify", action="store_true")
    mode.add_argument("--record", metavar="OUT")
    ap.add_argument("-j", type=int, default=8)
    a = ap.parse_args(argv)
    plist = list(state_projects(a.spec) if a.spec.endswith(".tsv") else projects(a.spec))
    absent = [p for p, _, _ in plist if not os.path.isdir(os.path.join(a.tree, p, ".git"))
              and not os.path.isfile(os.path.join(a.tree, p, ".git"))]
    if absent:
        sys.exit("projects absent from {} (run repo sync for them first): {}".format(a.tree, ", ".join(absent)))
    if a.record:
        return record(a.tree, plist, a.record, a.j)
    report_only = a.check or a.verify
    counts, failed = {}, []
    with concurrent.futures.ThreadPoolExecutor(a.j) as ex:
        futs = {ex.submit(one, a.tree, p, r, u, report_only): p for p, r, u in plist}
        for f in concurrent.futures.as_completed(futs):
            try:
                path, state, sha = f.result()
            except Exception as e:                     # noqa: BLE001 - report every failing project, then fail
                failed.append(str(e))
                continue
            counts[state] = counts.get(state, 0) + 1
            if state != "ok":
                print("{}\t{}\t{}".format(state, path, sha))
    print("summary: " + ", ".join("{} {}".format(v, k) for k, v in sorted(counts.items())))
    if failed:
        sys.exit("FAILED:\n" + "\n".join(failed))
    if a.verify and set(counts) != {"ok"}:
        sys.exit(3)


if __name__ == "__main__":
    main(sys.argv[1:])
