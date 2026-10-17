/* SPDX-License-Identifier: MIT
 * What the Codec2 store exposes, derived only from nv_video_query_caps() at runtime.
 * One source of truth for the component list (store), the component interfaces
 * (sizes, bit depths) and media_codecs_c2.xml (tools/nvc2_xmlgen.cpp).
 */
#ifndef NVC2_CAPS_H
#define NVC2_CAPS_H
#include <cstdint>
#include <string>
#include <vector>
#include "nv_video.h"

namespace nvc2 {

struct CodecEntry {
    uint32_t codec;           /* enum NvVideoCodec */
    const char *name;         /* c2.nvprolapse.<x>.decoder */
    const char *mime;
    NvVideoCaps caps;
};

/* Static table of names/mimes; order = preference in the store listing. */
const std::vector<std::pair<uint32_t, std::pair<const char *, const char *>>> &codec_table();

/* Queries every codec; returns only those with caps.supported. A query error (not
 * "unsupported") is reported in *error and makes the result empty: no partial store. */
std::vector<CodecEntry> supported_codecs(NvVideoDevice *dev, std::string *error);

/* media_codecs_c2.xml for exactly these entries (Codec2InfoBuilder format). */
std::string media_codecs_xml(const std::vector<CodecEntry> &entries);

} // namespace nvc2
#endif
