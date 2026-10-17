/* SPDX-License-Identifier: MIT
 * Host end-to-end test of the decoder core on the REAL nv_video backend (GNU build of
 * libnv_bionic_native) and the GPU: IVF VP9 -> Decoder::queue (one IVF frame = one unit,
 * superframes included) -> copy_out into LINEAR NV12 dma-bufs laid out like gralloc
 * patch 0004 (pitch aligned to 256, CbCr at pitch*even(h)) -> release fence -> compare the
 * crop window with a software reference (ffmpeg -pix_fmt nv12). Also checks that every
 * input unit is returned exactly once (hidden frames as empty) and that EOS comes last.
 *
 *   real_vp9_test <in.ivf> <ref.nv12> <w> <h> [pool buffers]
 * Buffers come from /dev/dma_heap/system (sysmem, i.e. a foreign dma-buf for the backend).
 */
#include "nvc2_decoder.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <linux/dma-heap.h>
#include <poll.h>
#include <set>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

using namespace nvc2;

static std::vector<uint8_t> slurp(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (!f) {
        perror(p);
        exit(2);
    }
    std::vector<uint8_t> v;
    uint8_t buf[1 << 16];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        v.insert(v.end(), buf, buf + n);
    fclose(f);
    return v;
}

struct HeapPool : OutputPool {
    struct Buf { int fd; size_t size; bool busy; };
    std::vector<Buf> bufs;
    uint32_t w, h, pitch, luma_rows;
    HeapPool(int n, uint32_t w_, uint32_t h_) : w(w_), h(h_)
    {
        pitch = (w + 255) & ~255u;
        luma_rows = (h + 1) & ~1u;
        size_t size = (size_t)pitch * luma_rows * 3 / 2;
        int heap = open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
        if (heap < 0) {
            perror("/dev/dma_heap/system");
            exit(2);
        }
        for (int i = 0; i < n; i++) {
            struct dma_heap_allocation_data a = {};
            a.len = (size + 4095) & ~4095ul;
            a.fd_flags = O_RDWR | O_CLOEXEC;
            if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &a) != 0) {
                perror("DMA_HEAP_IOCTL_ALLOC");
                exit(2);
            }
            bufs.push_back({(int)a.fd, (size_t)a.len, false});
        }
        close(heap);
    }
    int fetch(uint32_t fw, uint32_t fh, uint32_t format, OutputBuffer *out) override
    {
        if (format != NV_VIDEO_FORMAT_NV12 || fw > w || fh > h)
            return -EINVAL;
        for (size_t i = 0; i < bufs.size(); i++)
            if (!bufs[i].busy) {
                bufs[i].busy = true;
                *out = OutputBuffer{};
                out->target.dmabuf_fd[0] = out->target.dmabuf_fd[1] = bufs[i].fd;
                out->target.offset[0] = 0;
                out->target.offset[1] = (uint64_t)pitch * luma_rows;
                out->target.pitch[0] = out->target.pitch[1] = pitch;
                out->target.modifier = 0;
                out->target.width = w;
                out->target.height = h;
                out->target.acquire_fence_fd = -1;
                out->cookie = i;
                return 0;
            }
        return -EAGAIN;
    }
    void discard(const OutputBuffer &b) override { bufs[b.cookie].busy = false; }
};

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: real_vp9_test in.ivf ref.nv12 w h [pool]\n");
        return 2;
    }
    const auto ivf = slurp(argv[1]);
    const auto ref = slurp(argv[2]);
    const uint32_t w = atoi(argv[3]), h = atoi(argv[4]);
    const int npool = argc > 5 ? atoi(argv[5]) : 3;
    if (ivf.size() < 32 || memcmp(ivf.data(), "DKIF", 4)) {
        fprintf(stderr, "not IVF\n");
        return 2;
    }
    const size_t frame_bytes = (size_t)w * h * 3 / 2;

    NvVideoDevice *dev = nullptr;
    NvVideoDeviceInfo info{};
    if (nv_video_device_open(&dev, &info) != 0) {
        fprintf(stderr, "device_open: %s\n", nv_video_last_error());
        return 1;
    }
    Config cfg;
    cfg.codec = NV_VIDEO_CODEC_VP9;
    cfg.max_width = w;
    cfg.max_height = h;
    cfg.extra_output_slots = 0;
    std::string err;
    Decoder *d = Decoder::create(dev, cfg, &err);
    if (!d) {
        fprintf(stderr, "create: %s\n", err.c_str());
        return 1;
    }
    HeapPool pool(npool, w, h);

    size_t shown = 0, empties = 0, bad_frames = 0, units = 0, agains = 0;
    std::set<uint64_t> returned;
    bool eos_seen = false, order_ok = true;
    auto consume = [&](std::vector<Output> &outs) {
        for (auto &o : outs) {
            if (returned.count(o.index) || eos_seen)
                order_ok = false;
            returned.insert(o.index);
            if (o.eos)
                eos_seen = true;
            if (o.empty) {
                empties++;
                continue;
            }
            if (o.release_fence >= 0) {
                struct pollfd p = {o.release_fence, POLLIN, 0};
                if (poll(&p, 1, 2000) != 1) {
                    fprintf(stderr, "copy fence timeout\n");
                    exit(1);
                }
                close(o.release_fence);
            }
            auto &b = pool.bufs[o.buffer.cookie];
            uint8_t *m = (uint8_t *)mmap(nullptr, b.size, PROT_READ, MAP_SHARED, b.fd, 0);
            if (m == MAP_FAILED) {
                perror("mmap");
                exit(1);
            }
            size_t diff = 0;
            const uint8_t *r = ref.data() + shown * frame_bytes;
            if ((shown + 1) * frame_bytes > ref.size()) {
                fprintf(stderr, "more pictures than reference frames\n");
                exit(1);
            }
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t x = 0; x < w; x++)
                    diff += m[(size_t)y * pool.pitch + x] != r[(size_t)y * w + x];
            const uint8_t *rc = r + (size_t)w * h;
            const uint8_t *mc = m + (size_t)pool.pitch * pool.luma_rows;
            for (uint32_t y = 0; y < h / 2; y++)
                for (uint32_t x = 0; x < w; x++)
                    diff += mc[(size_t)y * pool.pitch + x] != rc[(size_t)y * w + x];
            munmap(m, b.size);
            if (diff) {
                bad_frames++;
                fprintf(stderr, "picture %zu (unit %llu): %zu bytes differ\n", shown, (unsigned long long)o.index,
                        diff);
            }
            shown++;
            b.busy = false; /* the "consumer" returns the buffer at once */
        }
        outs.clear();
    };

    size_t off = 32;
    std::vector<Output> outs;
    while (off + 12 <= ivf.size()) {
        uint32_t len = ivf[off] | ivf[off + 1] << 8 | ivf[off + 2] << 16 | (uint32_t)ivf[off + 3] << 24;
        const uint8_t *p = ivf.data() + off + 12;
        off += 12 + len;
        const bool last = off + 12 > ivf.size();
        for (;;) {
            Status st = d->queue(p, len, units, last, false, &pool, &outs);
            consume(outs);
            if (st == Status::Error) {
                fprintf(stderr, "queue(unit %zu): %s\n", units, d->error().c_str());
                return 1;
            }
            if (st == Status::Ok)
                break;
            agains++;
            if (d->drain(&pool, &outs) == Status::Error) {
                fprintf(stderr, "drain: %s\n", d->error().c_str());
                return 1;
            }
            consume(outs);
        }
        units++;
    }
    while (!eos_seen) {
        if (d->drain(&pool, &outs) == Status::Error) {
            fprintf(stderr, "drain: %s\n", d->error().c_str());
            return 1;
        }
        bool none = outs.empty();
        consume(outs);
        if (none && !eos_seen) {
            fprintf(stderr, "stuck: EOS never returned\n");
            return 1;
        }
    }
    delete d;
    nv_video_device_close(dev);
    const size_t ref_frames = ref.size() / frame_bytes;
    printf("real_vp9_test %ux%u on %s: %zu units, %zu pictures (ref %zu), %zu empty (hidden), %zu AGAIN retries, "
           "%zu returned, order %s, %zu mismatching pictures\n",
           w, h, info.name, units, shown, ref_frames, empties, agains, returned.size(), order_ok ? "ok" : "BAD",
           bad_frames);
    const bool ok = shown == ref_frames && returned.size() == units && order_ok && !bad_frames && eos_seen;
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
