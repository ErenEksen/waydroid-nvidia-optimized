# Offline HEVC fixture generator for nv-nvdec-probe (V0 NVDEC spike): runs tinygrad's reference HEVC header parser (MIT,
# extra/hevc/hevc.py) on the CPU only and dumps per-frame nvdec_hevc_pic_s
# descriptors (0x300 bytes each: pic_s + Blackwell ext + tile sizes at 0x200)
# plus frame table. No GPU, no tinygrad device.  argv: tinygrad_root in out_dir
import sys, json, os
root, inp, out = sys.argv[1:4]
sys.path.insert(0, root)
import extra.hevc.hevc as H
captured = {}
class FakeTensor:
  def __init__(self, data, device=None): captured['ctx'] = bytes(data)
  def reshape(self, *a): return self
H.Tensor = FakeTensor
dat = open(inp, 'rb').read()
_, frames, w, h, luma_w, luma_h, chroma_off = H.parse_hevc_file_headers(dat, device="CPU")
os.makedirs(out, exist_ok=True)
open(os.path.join(out, 'desc.bin'), 'wb').write(captured['ctx'])
open(os.path.join(out, 'stream.hevc'), 'wb').write(dat)
json.dump({"w": w, "h": h, "luma_w": luma_w, "luma_h": luma_h, "chroma_off": chroma_off, "desc_stride": 0x300,
           "pic_s_size": __import__('ctypes').sizeof(H.nv_gpu.nvdec_hevc_pic_s),
           "frames": [dict(offset=o, size=s, pos=p, hist=hs, is_hist=bool(ih)) for o, s, p, hs, ih in frames]},
          open(os.path.join(out, 'frames.json'), 'w'), indent=1)
print(w, h, luma_w, luma_h, chroma_off, len(frames), frames[:6])
with open(os.path.join(out, 'frames.txt'), 'w') as f:
  f.write(f"{luma_w} {luma_h} {w} {h}\n")
  for o, s, p, hs, ih in frames: f.write(f"{o} {s} {p} {hs} {int(bool(ih))}\n")
