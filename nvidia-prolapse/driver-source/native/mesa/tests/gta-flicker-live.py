#!/usr/bin/env python3
"""Live check: GTA San Andreas in the running Waydroid container must not drop whole frames.

Symptom it detects (2026-10-16, nvrm root mesa262c): every few frames the game's window buffer is
pure black except for untextured draws (the HUD health bar) -> on screen the game flickers.

  gta-flicker-live.py OUT_DIR [--launch] [--frames N] [--package PKG]

--launch  force-stops the game, starts it, taps "Skip Sign in" and "Resume" (guest display coordinates
          scaled from 2560x1412), and waits until a screencap shows a non-black scene.
Without --launch the game must already be in a scene (gameplay, not the menu).

Measures a burst of N guest `screencap -p` frames (SurfaceFlinger composite of the game layer), and per
frame the fraction of exactly-black pixels. A frame is a DROPOUT when >= 90 % of its pixels are black
while the burst median is < 50 % (i.e. the scene itself is not dark). Exit 0 = PASS (no dropout),
1 = FAIL (dropouts, listed), 2 = could not measure (no scene, container down).
Writes OUT_DIR/frameNN.png (downscaled 1/2) and OUT_DIR/result.json.
"""
import argparse, json, os, subprocess, sys, time

try:
    from PIL import Image
except ImportError:
    sys.exit("gta-flicker-live: needs python3-pillow on the host")

LXC = ["sudo", "-n", "lxc-attach", "-P", "/var/lib/waydroid/lxc", "-n", "waydroid", "--clear-env", "--",
       "/system/bin/sh", "-c"]
ENV = "export PATH=/system/bin:/vendor/bin; "


def guest(cmd, binary=False):
    # pipe through the parent (never redirect lxc-attach output to a file: it chowns it to root 0600)
    p = subprocess.run(LXC + [ENV + cmd], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return p.stdout if binary else p.stdout.decode(errors="replace")


def display_size():
    out = guest("wm size")
    for line in out.splitlines():
        if "size:" in line:
            w, h = line.split(":")[1].strip().split("x")
            return int(w), int(h)
    sys.exit(f"gta-flicker-live: cannot read guest display size: {out!r}")


def black_fraction(im):
    im = im.convert("RGB")
    # pixels black in all channels: count via a point-mapped L image
    gray = im.convert("L").point(lambda v: 255 if v == 0 else 0)
    n = im.width * im.height
    return gray.histogram()[255] / n


def grab(path_full):
    data = guest("screencap -p /data/local/tmp/gta-flicker.png && cat /data/local/tmp/gta-flicker.png",
                 binary=True)
    if not data.startswith(b"\x89PNG"):
        return None
    with open(path_full, "wb") as f:
        f.write(data)
    return Image.open(path_full)


def tap(x, y, size):
    w, h = size
    guest(f"input tap {int(x * w / 2560)} {int(y * h / 1412)}")


def launch(pkg, size, tmp):
    guest(f"am force-stop {pkg}")
    time.sleep(1)
    guest(f"am start -n {pkg}/com.rockstargames.gtasa.GameActivity")
    time.sleep(15)
    tap(1566, 770, size)      # "Skip Sign in" (absent after the first sign-in choice: taps black area)
    time.sleep(12)
    tap(1280, 1140, size)     # "Resume" on the main menu
    for _ in range(30):
        time.sleep(3)
        im = grab(os.path.join(tmp, "probe.png"))
        if im is not None and black_fraction(im) < 0.5:
            return True
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--launch", action="store_true")
    ap.add_argument("--frames", type=int, default=16)
    ap.add_argument("--package", default="com.rockstargames.gtasa")
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    size = display_size()
    if a.launch and not launch(a.package, size, out):
        print("UNKNOWN gta-flicker-live: no scene reached after launch")
        return 2
    frames = []
    t0 = time.time()
    for i in range(a.frames):
        full = os.path.join(out, f"full{i:02d}.png")
        im = grab(full)
        if im is None:
            print("UNKNOWN gta-flicker-live: screencap failed")
            return 2
        bf = black_fraction(im)
        im.convert("RGB").resize((im.width // 2, im.height // 2)).save(os.path.join(out, f"frame{i:02d}.png"))
        os.unlink(full)
        frames.append({"frame": i, "t": round(time.time() - t0, 3), "black_fraction": round(bf, 4)})
    med = sorted(f["black_fraction"] for f in frames)[len(frames) // 2]
    if med >= 0.5:
        print(f"UNKNOWN gta-flicker-live: burst median black fraction {med:.2f}: not in a lit scene")
        res, code = "unknown", 2
    else:
        drops = [f["frame"] for f in frames if f["black_fraction"] >= 0.9]
        res, code = ("fail", 1) if drops else ("pass", 0)
        print(f"{'FAIL' if drops else 'PASS'}    gta-flicker-live: {len(drops)}/{len(frames)} dropout frames "
              f"{drops} (median black {med:.3f}); frames in {out}")
    json.dump({"result": res, "median_black": med, "frames": frames}, open(os.path.join(out, "result.json"), "w"),
              indent=1)
    return code


if __name__ == "__main__":
    sys.exit(main())
