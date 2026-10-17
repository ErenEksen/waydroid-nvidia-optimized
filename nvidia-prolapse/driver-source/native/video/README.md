# nv_video — NVDEC decode on the native RM backend

C ABI: `nv_video.h` (v1, unchanged). Implementation: `src/video/` (exported from
`libnv_bionic_native` for GNU and bionic, 64- and 32-bit). Host test:
`target/release/nv-video-test <in.ivf> <ref.nv12|-> [max_w max_h]` (every frame through
decode → next_picture → copy_out into a LINEAR NV12 dma-buf → release fence → readback).

## Layers

| File | Role |
|---|---|
| `mod.rs` | errors (`VideoError` ↔ `NV_VIDEO_E_*`), device open by PCI identity (`NV_VIDEO_PCI`), caps per NVDEC class, the `CodecDecoder` trait |
| `channel.rs` | NVDEC channel (TSG on NVDEC0, SYNC subcontext, host GPFIFO, decoder object) + 64-bit completion timeline on an RM semaphore surface; CPU waits are sync_file + poll, never spins |
| `session.rs` | DPB surface pool (VRAM, engine layout, sized from the session max so the pitch never changes), 4-slot descriptor/bitstream ring (stock growth `(len+0x8ff)&~0xff`), display queue, picture refcounts (`E_AGAIN`), `copy_out` on a backend copy `Queue` |
| `vp9.rs` | VP9 profile 0 (8-bit) and profile 2 (10-bit) codec |
| `av1/` | AV1 main 8/10-bit 4:2:0: OBU/header parser, default CDFs, descriptor builder, film grain, `codec.rs` = `CodecDecoder` (film grain needs the sequence header as `codec_config`: grain-free references go to codec-private surfaces, the session surface receives the grain-applied picture, hidden frames included; a showable grain frame keeps that surface referenced so show_existing_frame outputs it) |
| `h264/` | H.264: SPS/PPS/slice parser, POC 0/1/2, gaps, MMCO, NEW_H264 (app 8) and legacy (app 3, interlaced frame pictures) setups byte-exact vs stock; `codec.rs` = plug-in |
| `hevc/` | HEVC: general parser (POC MSB, RPS DPB, 8.3.4 lists, scaling lists, tiles, CRA/RASL), setup byte-exact vs stock; `codec.rs` = plug-in |
| `bits.rs` | Annex-B NAL / access-unit splitting, emulation-prevention removal, RBSP bit reader |
| `ffi.rs` | `extern "C"` entry points, per-thread `nv_video_last_error` |

## Codec plug-in contract (`CodecDecoder`, for impl-av1 / impl-avc)

A codec is a parse → descriptor builder; the session owns every GPU resource that is not
codec-specific.

```rust
pub trait CodecDecoder {
    fn codec(&self) -> u32;
    fn surface_layout(&self, max_w: u32, max_h: u32) -> SurfaceLayout; // SurfaceLayout::new(w, h, bytes/sample)
    fn gob_kinds(&self, decoder_class: u32) -> [u32; 2];   // copy-engine GOB kind of luma / chroma output
    fn dpb_size(&self) -> usize;                           // max surfaces `referenced()` can return
    fn allocate(&mut self, alloc: &mut dyn Allocator, max_w: u32, max_h: u32) -> VResult<()>; // side buffers
    fn surfaces_needed(&self, unit: &[u8]) -> VResult<usize>; // checked before any state change -> E_AGAIN
    fn push_unit(&mut self, unit: &[u8]) -> VResult<()>;
    fn next_job(&mut self) -> VResult<Option<Job>>;        // decode order; may parse lazily
    fn build(&mut self, job: &DecodeJob, ctx: &BuildContext, desc: &mut [u8]) -> VResult<Vec<u32>>;
    fn finish(&mut self, job: &DecodeJob, current: usize, readback: Option<&[u8]>) -> VResult<()>;
    fn referenced(&self) -> Vec<usize>;                    // surface indices held by the DPB
    fn bump_ready(&self, pending: usize) -> bool { true }  // reorder bound for display_key codecs
    fn flush(&mut self);
    fn max_referenced(&self) -> Option<usize> { None }     // AV1: Some(8), see admission below
    fn status_offset(&self) -> Option<usize> { None }      // AV1: Some(0x4700), nvdec_status_s
}
```

Per unit the session runs the admission check (`session::unit_admitted`): `surfaces_needed` ≤ free
surfaces, or, for codecs with `max_referenced() = Some(n)`, `n + 1 + held ≤ pool` (`held` = distinct
surfaces of unreleased pictures): the references, the picture in flight and every held picture then
fit at once, so each picture of the unit finds a surface whatever the unit refreshes. Otherwise
`E_AGAIN`; a caller that holds nothing is never refused (AV1 temporal units with several hidden
alt-ref frames need more surfaces than are free before the unit, the refreshes inside it free them).
Running out of surfaces inside an admitted unit is `E_INVALID` (accounting bug), never `E_AGAIN`.
Then `push_unit` → loop `next_job`:

- `Job::ShowExisting { surface, info }` → a display picture on an existing surface.
- `Job::Decode(DecodeJob { info, show, display_key, bitstream, readback })` → the session picks a
  free surface (`ctx.current`: not referenced, not held by any picture), takes the next ring slot
  (waits only if that slot's previous picture is still running), calls `build` with a zeroed
  0x5000-byte descriptor image and `BuildContext { decoder_class, descriptor, bitstream,
  bitstream_len, picture_index, current, surfaces: &[SurfaceRef { luma, chroma }], layout }`,
  uploads descriptor + `bitstream`, submits the returned methods (subchannel 4, ending in
  EXECUTE; the session appends the timeline release). If `readback` is set the session waits for
  that picture and passes those descriptor bytes to `finish` (VP9 counts); otherwise `finish` runs
  right after submission. `show` pictures enter the display queue in decode order
  (`display_key: None`) or sorted by key, released while `bump_ready(len)` or at end of stream.

Engine status (codecs with `status_offset`): every picture's `nvdec_status_s` is read once it is
complete (non-blocking at each `decode`/`copy_out`, before its ring slot is reused, and `copy_out`
waits for the decode of the picture it copies if that is still unchecked). error_status ≠ 0 →
`E_BITSTREAM` naming the status, the blocks in error and the picture; sticky until `flush`
(predicted pictures are wrong too). Never a silent wrong picture.

Rules: never hold GPU addresses across sessions; use `ctx.surfaces[i]` for references (relocation
= NONINC pair `va >> 8, va >> 40` as stock); report unsupported stream features as
`VideoError::Unsupported("<feature>")`, malformed data as `Bitstream`; no silent fallbacks.

## copy_out

Copy engine, not a shader: the measured engine output is a layout the copy engine reads natively.

- GB202 (class 0xCFB0), VP9 `tileformat = 1`: luma = Blackwell 8-bit GOB (64×8 B GOB of 8×8-byte
  tiles), CbCr = Blackwell 16-bit GOB (16×8-byte tiles); GOBs in 1-wide, 2-high blocks.
  Mesa NIL `CopyGOBBlackwell2D1BPP` / `CopyGOBBlackwell2D2BPP` are these layouts, and the
  Blackwell copy class reads them with `SET_SRC_BLOCK_SIZE.GOB_KIND = 1 / 2` (clc9b5.h).
  Measured 2026-10-14: tileformat 0 = 8×2 sector "TBL" layout (not copy-engine readable),
  tileformat 2 = 16×16 tiles. H.264 (both setups) and HEVC with tileformat 1 write the same
  Blackwell kinds 1/2 (measured: kinds 0/0 differ in ~30% of bytes, 1/2 bit-exact).
- 10-bit (HEVC Main 10, VP9 profile 2, AV1 10-bit incl. film grain): the session is created with
  `bit_depth = 10`; surfaces hold 16-bit containers (pitch align64(2w), stock `+0x1dc = 8`), the
  engine writes luma as Blackwell 16-bit GOBs and CbCr as 32-bit GOBs (kinds 2/0; stock writes the
  same pair as `luma | chroma << 2` = 2 into every picture setup, 9 for 8-bit) with the sample in
  the 10 MSBs. P010 is therefore a plain copy with 2 bytes/sample: bit-exact against FFmpeg p010le,
  no shift or conversion pass. NV12 for 10-bit / P010 for 8-bit pictures is `E_UNSUPPORTED` (no
  depth conversion), and a stream whose depth differs from the session's is `E_UNSUPPORTED`.
- Destination: LINEAR (pitch per plane) or `DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D` (uncompressed,
  g = 2 → kind 0, g = 3 → kinds 1/2 for NV12, 2/0 for P010 on Blackwell copy classes; the P010
  block-linear write is not yet verified end to end). Target dma-bufs are imported
  per call (`external::import`) and kept until the copy retires; fds stay the caller's.
- Ordering: the copy queue acquires the NVDEC timeline (GPU wait); the release fence is the copy
  queue's semaphore sync_file. The acquire fence is waited on the CPU (open item).
- NV12 from 8-bit pictures only; P010 → `E_UNSUPPORTED` until 10-bit decode exists.

## Coverage (RTX 5090, 615.71.09, 2026-10-14) — all bit-exact vs libvpx through the C ABI

| Stream | Feature | Frames |
|---|---|---|
| a640, b1080, c1928, d1000aq (stock-trace streams) | 640x360 / 1080p / odd 1928x1208 / aq-mode 3 segmentation | 40/40/8/40 |
| altref (2-pass, auto-alt-ref, lag 25, frame-parallel 0) | hidden alt-ref, backward adaptation, superframes | 60 |
| errres (error-resilient 1) | sign-bias reset, no adaptation | 60 |
| reschange (640x360 → 1280x720 at a key frame) | resolution change inside one session | 60 |
| e4k (3840x2160, 4 tile columns, frame-parallel 0) | 4K timing | 30 |
| profile 1/2 | `E_UNSUPPORTED` (unit test) | — |

## H.264 / HEVC coverage (RTX 5090, 615.71.09, 2026-10-14) — `nv-video-avc-test`, bit-exact vs FFmpeg software decode

Host test: `target/release/nv-video-avc-test <in.h264|in.hevc> <ref.nv12|-> <max_w> <max_h>` (one
Annex-B access unit per `nv_video_decode`). Descriptor-level checks against stock captures:
`NVDEC_STOCK_EVIDENCE=<stock captures dir> cargo test --lib -- --include-ignored video::`.

| Stream | Feature | Frames |
|---|---|---|
| H.264 cabac / cavlc / cqm (2 slices) | High + B, Main CAVLC weightp, custom matrices | 12 / 12 / 12 |
| H.264 pyramid, wrap | B-pyramid MMCO ref 5; frame_num + POC LSB wrap | 20 / 300 |
| H.264 poc2 1080p, 720p 4-slice, 642x362, baseline ref 16, lossless, open GOP | POC type 2, slices, crop + temporal direct, 16 refs, qpprime bypass | 10 / 120 / 40 / 40 / 20 / 30 |
| H.264 mbaff 352x288, 1080i | legacy APPLICATION_ID 3 (MBAFF frames) | 12 / 30 |
| HEVC hevc8, multislice, pocwrap, cqm, open GOP, pyramid 3-slice, 1080p B ref4, 642x362 open GOP | B, 4 slices, POC MSB, scaling lists, CRA/RASL, odd size | 12 / 12 / 300 / 6 / 80 / 40 / 60 / 40 |
| HEVC tiles (comma 1952x1216, 3 columns) | non-uniform tiles | 7 of 8 (8th access unit is cut by the 512 KiB asset) |
| HEVC Main10 | descriptor byte-exact vs stock; decode → `E_UNSUPPORTED` until P010 copy_out | — |
| H.264 field pictures (PAFF), 4:2:2/4:4:4, >8-bit; HEVC RExt/SCC, PCM, slice long-term refs, 2-D tile grids | `E_UNSUPPORTED` with the feature name | — |

## AV1 coverage (RTX 5090, 615.71.09, 2026-10-14) — bit-exact vs libdav1d through the C ABI

| Stream (libaom) | Feature | Frames |
|---|---|---|
| a_640 | 640x360, hidden ARF, temporal MVs, CDF adaptation | 12 shown / 13 decoded |
| fg_640 | film grain (CPU grain templates, display surface) | 8 |
| nocdf_640 | error-resilient, cdf-update-mode 0 | 8 |
| t_1080 | 1920x1080, 2x2 tiles, aq-mode 1 segmentation | 8 |
| svt_1928 (SVT-AV1 `--preset 8`, testsrc2 1928x1208) | odd size, SVT tool set | 16 |
| aom_sb128 (aomenc `--sb-size=128 --tile-columns=1 --lag-in-frames=16 --enable-overlay=1 --cpu-used=8 --end-usage=q --cq-level=40`, mandelbrot 720p) | 128x128 superblocks, 2 tiles, show_existing_frame | 24 |
| rav1e_360 (rav1e `-s 10 --quantizer 120`, testsrc2) | separate U/V delta q, segmentation ±, loop_filter_delta_enabled = 0 | 20 |
| aom_hidden_640 (`testdata/`, libaom cpu-used 6, 2x2 tiles, lag 35, auto-alt-ref; BBB 640x360) | units of up to several hidden alt-ref frames (gallery `av1_aom` "needs 6, 4 free" fix) | 40 |
| svt_grain_640 (`testdata/`, SVT-AV1 preset 8 film-grain=12; BBB 640x360) | film grain on hidden frames, show_existing_frame of grain frames | 40 |
| gallery av1_svt / av1_aom / av1_grain (BBB 1080p, `build/video-gallery`) | as above at 1920x1080 | 150 each |
| aom_superres (`--superres-mode=1 --superres-denominator=12`) | key frame bit-exact; inter frames → `E_UNSUPPORTED` ("AV1 superres on inter frames", engine error_status 0xb/0xd, needs a stock capture) | 1 of 10 |

Descriptor level vs stock captures (`tools/av1/compare_stock.py`): pic_s, +0x244 word, tile info,
global motion, film-grain buffer, CDF buffer selection, colMV/segment buffer selection 37/37 each.
