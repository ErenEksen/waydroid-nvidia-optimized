//! GNU host test of nv_video.h through its C ABI: every frame goes through
//! nv_video_decode -> nv_video_next_picture -> nv_video_copy_out (LINEAR NV12
//! dma-buf target) -> wait release fence -> CPU readback -> compare with a
//! software (FFmpeg) NV12 reference. Annex-B H.264 (.h264/.264) or HEVC (.hevc/.265), one
//! access unit per nv_video_decode() call.
//! usage: nv-video-avc-test <in.h264|in.hevc> <reference.nv12|-> <max_w> <max_h>
//! NV_VIDEO_BIT_DEPTH=10: create a 10-bit session and copy out P010 (references and dumps are then
//! p010le: 16-bit little-endian samples, 10 MSBs).
//! NV_VIDEO_DUMP=<out.nv12>: also write every output picture (display order,
//! display window, tightly packed NV12, concatenated) to that file.
use nv_bionic_native::video::{
    bits::access_units, ffi::*, NvVideoDeviceInfo, TestDmaBuf, VideoDevice, CODEC_H264, CODEC_HEVC,
    FORMAT_NV12, FORMAT_P010,
};
use std::time::{Duration, Instant};

fn check(r: i32, what: &str) {
    if r < 0 {
        // SAFETY: nv_video_last_error returns a valid C string.
        let msg = unsafe { std::ffi::CStr::from_ptr(nv_video_last_error()) };
        eprintln!("{what}: error {r}: {}", msg.to_string_lossy());
        std::process::exit(2);
    }
}

fn wait_fd(fd: i32) {
    if fd < 0 {
        return;
    }
    #[repr(C)]
    struct P {
        fd: i32,
        e: i16,
        r: i16,
    }
    extern "C" {
        fn poll(f: *mut P, n: std::ffi::c_ulong, t: i32) -> i32;
        fn close(fd: i32) -> i32;
    }
    let mut p = P { fd, e: 1, r: 0 };
    // SAFETY: one pollfd; fd owned by us.
    unsafe {
        if poll(&mut p, 1, 2000) != 1 {
            eprintln!("release fence did not signal");
            std::process::exit(3);
        }
        close(fd);
    }
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let data = std::fs::read(&args[0]).expect("read stream");
    let hevc = args[0].ends_with(".hevc") || args[0].ends_with(".265");
    let packets = access_units(&data, hevc);
    let reference = (args.get(1).map(String::as_str) != Some("-"))
        .then(|| std::fs::read(&args[1]).expect("read reference"));
    let max_w: u32 = args[2].parse().unwrap();
    let max_h: u32 = args[3].parse().unwrap();

    let mut dev = std::ptr::null_mut();
    let mut info = std::mem::MaybeUninit::<NvVideoDeviceInfo>::zeroed();
    check(
        unsafe { nv_video_device_open(&mut dev, info.as_mut_ptr()) },
        "device_open",
    );
    let info = unsafe { info.assume_init() };
    let name = String::from_utf8_lossy(&info.name);
    println!(
        "device {:04x} {} class {:#x} engines {}",
        info.pci_device,
        name.trim_end_matches('\0'),
        info.nvdec_class,
        info.nvdec_engines
    );
    let bit_depth: u32 =
        std::env::var("NV_VIDEO_BIT_DEPTH").map_or(8, |v| v.parse().expect("NV_VIDEO_BIT_DEPTH"));
    let bps = if bit_depth > 8 { 2usize } else { 1 };
    let si = NvVideoSessionInfo {
        codec: if hevc { CODEC_HEVC } else { CODEC_H264 },
        max_width: max_w,
        max_height: max_h,
        bit_depth,
        extra_output_slots: 2,
        codec_config: std::ptr::null(),
        codec_config_len: 0,
        reserved: [0; 8],
    };
    let mut s = std::ptr::null_mut();
    check(
        unsafe { nv_video_session_create(dev, &si, &mut s) },
        "session_create",
    );
    let vdev = VideoDevice::open().expect("device");
    let pitch = (max_w * bps as u32).next_multiple_of(256);
    let luma = pitch as usize * max_h as usize;
    let size = luma + pitch as usize * max_h.div_ceil(2) as usize;
    let target = TestDmaBuf::new(&vdev, size).expect("dma-buf");
    use std::os::fd::AsRawFd;
    let tfd = target.fd.as_raw_fd();

    let mut off = 0usize;
    let (mut frames, mut bad, mut diff_total) = (0usize, 0usize, 0usize);
    let mut dec_times = Vec::new();
    let mut copy_times = Vec::new();
    let mut out = vec![0u8; size];
    let mut dump = std::env::var_os("NV_VIDEO_DUMP")
        .map(|p| std::io::BufWriter::new(std::fs::File::create(p).expect("create NV_VIDEO_DUMP")));
    let mut handle = |s: *mut NvVideoSession,
                      frames: &mut usize,
                      bad: &mut usize,
                      diff_total: &mut usize,
                      off: &mut usize,
                      copy_times: &mut Vec<Duration>| {
        loop {
            let mut pic = std::mem::MaybeUninit::<NvVideoPicture>::zeroed();
            let r = unsafe { nv_video_next_picture(s, pic.as_mut_ptr()) };
            check(r, "next_picture");
            if r == 0 {
                break;
            }
            let pic = unsafe { pic.assume_init() };
            let (w, h) = (pic.crop_width as usize, pic.crop_height as usize);
            target.fill(0xa5).unwrap();
            let t = NvVideoTarget {
                dmabuf_fd: [tfd, tfd],
                offset: [0, luma as u64],
                pitch: [pitch, pitch],
                modifier: 0,
                format: if bps == 2 { FORMAT_P010 } else { FORMAT_NV12 },
                width: max_w,
                height: max_h,
                acquire_fence_fd: -1,
                reserved: [0; 6],
            };
            let mut fence = -1;
            let t0 = Instant::now();
            check(
                unsafe { nv_video_copy_out(s, pic.id, &t, &mut fence) },
                "copy_out",
            );
            wait_fd(fence);
            copy_times.push(t0.elapsed());
            target.read(0, &mut out).unwrap();
            let mut nv12 = Vec::with_capacity(w * h * 3 / 2 * bps);
            for y in 0..h {
                nv12.extend_from_slice(&out[y * pitch as usize..][..w * bps]);
            }
            for y in 0..h.div_ceil(2) {
                nv12.extend_from_slice(
                    &out[luma + y * pitch as usize..][..w.div_ceil(2) * 2 * bps],
                );
            }
            if let Some(d) = dump.as_mut() {
                std::io::Write::write_all(d, &nv12).expect("write NV_VIDEO_DUMP");
            }
            if let Some(r) = &reference {
                let refp = &r[*off..*off + nv12.len()];
                let d = nv12.iter().zip(refp).filter(|(a, b)| a != b).count();
                if d != 0 {
                    *bad += 1;
                    *diff_total += d;
                    if *bad <= 5 {
                        println!(
                            "frame {}: {d} of {} bytes differ ({w}x{h})",
                            *frames,
                            nv12.len()
                        );
                    }
                }
            }
            *off += nv12.len();
            *frames += 1;
            check(unsafe { nv_video_picture_release(s, pic.id) }, "release");
        }
    };
    let start = Instant::now();
    for (i, p) in packets.iter().enumerate() {
        let t0 = Instant::now();
        check(
            unsafe { nv_video_decode(s, p.as_ptr(), p.len(), i as u64, 0) },
            "decode",
        );
        dec_times.push(t0.elapsed());
        handle(
            s,
            &mut frames,
            &mut bad,
            &mut diff_total,
            &mut off,
            &mut copy_times,
        );
    }
    check(
        unsafe { nv_video_decode(s, std::ptr::null(), 0, 0, 1) },
        "drain",
    );
    handle(
        s,
        &mut frames,
        &mut bad,
        &mut diff_total,
        &mut off,
        &mut copy_times,
    );
    let wall = start.elapsed();
    if let Some(d) = dump.as_mut() {
        std::io::Write::flush(d).expect("flush NV_VIDEO_DUMP");
    }
    let stat = |v: &mut Vec<Duration>| {
        v.sort();
        let total: Duration = v.iter().sum();
        (
            total / v.len().max(1) as u32,
            v.get(v.len() / 2).copied().unwrap_or_default(),
            v.last().copied().unwrap_or_default(),
        )
    };
    let (dm, dmed, dmax) = stat(&mut dec_times);
    let (cm, cmed, cmax) = stat(&mut copy_times);
    println!(
        "{} packets, {frames} pictures, max {max_w}x{max_h}; nv_video_decode mean {dm:?} median {dmed:?} max {dmax:?}; copy_out+fence mean {cm:?} median {cmed:?} max {cmax:?}; wall {wall:?}",
        packets.len()
    );
    unsafe {
        nv_video_session_destroy(s);
        nv_video_device_close(dev);
    }
    if let Some(r) = &reference {
        if off != r.len() {
            println!("MISMATCH: produced {off} bytes, reference {}", r.len());
            std::process::exit(1);
        } else if bad == 0 {
            println!("PASS: all {frames} pictures bit-exact against the FFmpeg software decode");
        } else {
            println!("MISMATCH: {bad} pictures differ ({diff_total} bytes)");
            std::process::exit(1);
        }
    }
}
