#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Structural check of a generated media_codecs_c2.xml against what Android 13's
# MediaCodecsXmlParser accepts (frameworks/av android-13 media/libstagefright/xmlparser:
# MediaCodecs > Decoders > MediaCodec{name,type} > Limit|Feature) and our own rules:
# names c2.nvprolapse.<x>.decoder (never a Chrome software prefix), sizes WxH with even
# bounds and min <= max, every codec at most once. Usage: validate_xml.py FILE
import sys
import xml.etree.ElementTree as ET

MIMES = {"vp9": "video/x-vnd.on2.vp9", "av1": "video/av01", "hevc": "video/hevc", "avc": "video/avc"}
SOFT = ("omx.google.", "c2.android.")

def size(s):
    w, h = s.split("x")
    return int(w), int(h)

def main(path):
    root = ET.parse(path).getroot()
    assert root.tag == "MediaCodecs", root.tag
    assert [c.tag for c in root] == ["Decoders"], "only <Decoders> expected"
    seen = set()
    for mc in root[0]:
        assert mc.tag == "MediaCodec", mc.tag
        name, mime = mc.get("name"), mc.get("type")
        assert not name.lower().startswith(SOFT) and ".sw.dec" not in name, name
        parts = name.split(".")
        assert len(parts) == 4 and parts[:2] == ["c2", "nvprolapse"] and parts[3] == "decoder", name
        assert MIMES[parts[2]] == mime, (name, mime)
        assert name not in seen, "duplicate " + name
        seen.add(name)
        limits = {}
        for ch in mc:
            assert ch.tag in ("Limit", "Feature"), ch.tag
            if ch.tag == "Limit":
                limits[ch.get("name")] = ch
        assert ch is not None
        s = limits["size"]
        (minw, minh), (maxw, maxh) = size(s.get("min")), size(s.get("max"))
        assert 0 < minw <= maxw and 0 < minh <= maxh, name
        assert all(v % 2 == 0 for v in (minw, minh, maxw, maxh)), name
        assert limits["alignment"].get("value") == "2x2"
        assert limits["block-size"].get("value") == "16x16"
    assert seen, "no codecs"
    print("validate_xml: OK %s (%s)" % (path, ", ".join(sorted(seen))))

if __name__ == "__main__":
    main(sys.argv[1])
