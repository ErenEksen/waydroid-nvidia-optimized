#!/usr/bin/env python3
"""Pin a repo manifest to one image's build date.

Usage: pin-manifest.py MERGED.xml PROJECTS.tsv CUTOFF_SHAS.tsv OUT.xml [OVERRIDES.tsv]
  MERGED.xml        `repo manifest -o MERGED.xml` of the synced tree (merged, unpinned)
  PROJECTS.tsv      project-delta.sh's projects.tsv (path, project, url, rrev, synced sha)
  CUTOFF_SHAS.tsv   path <TAB> sha at the cutoff, for every branch-tracked project (project-delta.sh cutoff.tsv)
  OVERRIDES.tsv     path <TAB> name <TAB> sha <TAB> upstream: projects the LineageOS manifest moved between the
                    tree's sync and the cutoff (e.g. AOSP tag -> LineageOS fork); they use the default remote
Every <project> gets revision=<sha> and upstream=<original branch>; tag/sha revisions are kept as they are.
A branch-tracked project without a cutoff sha is an error (no silent fallback to the July sync state)."""
import sys
import xml.etree.ElementTree as ET

merged, projects_tsv, cutoff_tsv, out = sys.argv[1:5]
overrides = {}
if len(sys.argv) > 5:
    for l in open(sys.argv[5]):
        if l.strip() and not l.startswith("#"):
            path, name, sha, up = l.rstrip("\n").split("\t")
            overrides[path] = (name, sha, up)
cut = dict(l.rstrip("\n").split("\t")[:2] for l in open(cutoff_tsv) if l.strip())
rows = {l.split("\t")[0]: l.rstrip("\n").split("\t") for l in open(projects_tsv) if l.strip()}
tree = ET.parse(merged)
root = tree.getroot()
default = root.find("default")
default_rev = default.get("revision") if default is not None else None
remotes = {r.get("name"): r for r in root.findall("remote")}
missing, pinned = [], 0
for p in root.findall("project"):
    path = p.get("path") or p.get("name")
    if path in overrides:
        name, sha, up = overrides.pop(path)
        p.set("name", name)
        if "remote" in p.attrib:
            del p.attrib["remote"]
        p.set("revision", sha)
        p.set("upstream", up)
        p.set("dest-branch", up)
        pinned += 1
        continue
    rev = p.get("revision") or remotes.get(p.get("remote") or default.get("remote")).get("revision") or default_rev
    if rev.startswith("refs/tags/") or (len(rev) == 40 and all(c in "0123456789abcdef" for c in rev)):
        continue
    sha = cut.get(path)
    if not sha:
        missing.append(path)
        continue
    p.set("revision", sha)
    p.set("upstream", rev if rev.startswith("refs/") else "refs/heads/" + rev)
    p.set("dest-branch", p.get("upstream"))
    pinned += 1
if overrides:
    missing += ["override for absent project " + k for k in overrides]
if missing:
    sys.exit("no cutoff sha for branch-tracked projects: " + ", ".join(missing))
tree.write(out, xml_declaration=True, encoding="UTF-8")
print("pinned {} branch-tracked projects -> {}".format(pinned, out))
