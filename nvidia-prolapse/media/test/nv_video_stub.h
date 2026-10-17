/* SPDX-License-Identifier: MIT */
#ifndef NV_VIDEO_STUB_H
#define NV_VIDEO_STUB_H
#include <cstdint>
#include <vector>
#include "nv_video.h"

struct StubControl {
    NvVideoCaps caps[5]{};
    unsigned dpb_slots = 4;     /* + extra_output_slots = unreleased-picture limit */
    unsigned reorder_depth = 2; /* H.264/HEVC */
    bool device_lost = false;
    bool fence_fds = false;
    unsigned decode_calls = 0, copies = 0, sessions_created = 0, sessions_destroyed = 0;
    unsigned outstanding_at_destroy = 0;
    uint32_t last_copy_format = 0;
    std::vector<uint8_t> last_codec_config;
};
extern StubControl g_stub;
#endif
