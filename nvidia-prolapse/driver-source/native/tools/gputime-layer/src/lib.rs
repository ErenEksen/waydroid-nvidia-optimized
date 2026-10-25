//! VK_LAYER_PERF_gputime: GPU time of every render pass and every dispatch of an unmodified Vulkan
//! application, measured the same way on any driver (NVK, NVIDIA's own), so two runs line up pass by pass.
//!
//! What it records, inside the frame window only (no commands are added outside it):
//! * per primary command buffer: a TOP_OF_PIPE timestamp right after vkBeginCommandBuffer and a
//!   BOTTOM_OF_PIPE timestamp right before vkEndCommandBuffer (row kind `cb`);
//! * per render pass instance (vkCmdBeginRenderPass/2 -> `rp`, vkCmdBeginRendering -> `dr`): a
//!   BOTTOM_OF_PIPE timestamp before the begin and one after the end, both outside the pass;
//! * per dispatch (vkCmdDispatch/DispatchBase -> `cs`, vkCmdDispatchIndirect -> `csi`): BOTTOM_OF_PIPE
//!   before and after;
//! * per frame (`frame`): union of the `cb` intervals of the frame's submissions (GPU busy) and their span.
//!
//! In the default barrier mode (GPUTIME_SYNC=barrier) every BOTTOM_OF_PIPE timestamp is preceded by a barrier
//! that waits for all earlier work of the queue (see timestamp_after_all), so a region's time is "work done
//! between two idle points": consecutive regions do not overlap and add up.
//! The queries live in one timestamp pool per device, handed out in chunks that are reset on the host when
//! they come back (see Chunks).
//!
//! Identity of a region, to match passes between drivers: frame, seq (order within the frame: submission
//! order, command buffer order, region order), kind, attachment formats (VkFormat numbers, `+`-joined, from
//! the VkRenderPass or the dynamic-rendering image views), render area / dispatch size, pipeline hash
//! (FNV-1a 64 over every stage's SPIR-V hash, entry point and specialization data; a pass folds the
//! pipelines it draws with, in order), the number of distinct pipeline changes and the draw count.
//!
//! The submission is never changed (no extra command buffers, no extra semaphores). A command buffer that
//! is submitted again while its previous submission has not been read back makes the layer wait for that
//! previous submission's queries first (counted as `waits` in the frame row); without that wait the
//! second execution's reset would destroy unread results.
//!
//! Configuration (environment on glibc, system properties on Android):
//!   GPUTIME_OUT / debug.gputime.out        output prefix; "<prefix>-<pid>-d<N>.csv" + "-pipelines.csv" + "-sync.csv"
//!   GPUTIME_FRAMES / debug.gputime.frames  "A-B", inclusive frame window (default 1000-1299)
//!   GPUTIME_FRAME / debug.gputime.frame    "present" (default) or "submit": what advances the frame counter
//!   GPUTIME_SYNC / debug.gputime.sync      "barrier" (default) or "none"
//!   GPUTIME_DISPATCH / debug.gputime.dispatch  "off": no regions around dispatches (default on)
//! Nothing is measured without an output prefix.

#![allow(non_snake_case, clippy::missing_safety_doc)]

use ash::vk;
use ash::vk::Handle;
use std::cell::RefCell;
use std::collections::{BTreeMap, HashMap, VecDeque};
use std::ffi::{c_char, c_void, CStr};
use std::fmt::Write as _;
use std::fs::File;
use std::io::{BufWriter, Write};
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};
use std::sync::{Arc, Mutex, OnceLock, RwLock};

const LAYER_NAME: &[u8] = b"VK_LAYER_PERF_gputime\0";
const LAYER_DESC: &[u8] = b"GPU time per render pass, dispatch, command buffer and frame\0";
const POOL_QUERIES: u32 = 1 << 16;
const CHUNK: u32 = 128;

// ---------------------------------------------------------------------------------------------------------
// Loader interface (vk_layer.h); ash does not carry these.

const LAYER_LINK_INFO: i32 = 0;
const LOADER_DATA_CALLBACK: i32 = 1;

#[repr(C)]
struct LayerInstanceLink {
    next: *mut LayerInstanceLink,
    next_gipa: vk::PFN_vkGetInstanceProcAddr,
    next_gpdpa: *const c_void,
}

#[repr(C)]
struct LayerInstanceCreateInfo {
    s_type: vk::StructureType,
    p_next: *const c_void,
    function: i32,
    u: *mut c_void,
}

#[repr(C)]
struct LayerDeviceLink {
    next: *mut LayerDeviceLink,
    next_gipa: vk::PFN_vkGetInstanceProcAddr,
    next_gdpa: vk::PFN_vkGetDeviceProcAddr,
}

#[repr(C)]
struct LayerDeviceCreateInfo {
    s_type: vk::StructureType,
    p_next: *const c_void,
    function: i32,
    u: *mut c_void,
}

// ---------------------------------------------------------------------------------------------------------
// Logging and configuration.

#[cfg(target_os = "android")]
mod platform {
    use std::ffi::{c_char, c_int, CString};
    #[link(name = "log")]
    extern "C" {
        fn __android_log_write(prio: c_int, tag: *const c_char, text: *const c_char) -> c_int;
    }
    extern "C" {
        fn __system_property_get(name: *const c_char, value: *mut c_char) -> c_int;
    }
    pub fn log(msg: &str) {
        let m = CString::new(msg.replace('\0', " ")).unwrap_or_default();
        unsafe { __android_log_write(5, c"gputime".as_ptr(), m.as_ptr()) };
    }
    pub fn config(_env: &str, prop: &str) -> Option<String> {
        let name = CString::new(prop).ok()?;
        let mut buf = [0 as c_char; 92];
        let n = unsafe { __system_property_get(name.as_ptr(), buf.as_mut_ptr()) };
        if n <= 0 {
            return None;
        }
        let bytes: Vec<u8> = buf[..n as usize].iter().map(|&c| c as u8).collect();
        String::from_utf8(bytes).ok()
    }
}

#[cfg(not(target_os = "android"))]
mod platform {
    pub fn log(msg: &str) {
        eprintln!("VK_LAYER_PERF_gputime: {msg}");
    }
    pub fn config(env: &str, _prop: &str) -> Option<String> {
        std::env::var(env).ok().filter(|s| !s.is_empty())
    }
}

use platform::log;

mod synclog;

struct Config {
    out: Option<String>,
    sync: bool,
    first: u64,
    last: u64,
    per_submit: bool,
    dispatch_regions: bool,
}

fn config() -> &'static Config {
    static C: OnceLock<Config> = OnceLock::new();
    C.get_or_init(|| {
        let out = platform::config("GPUTIME_OUT", "debug.gputime.out");
        let (mut first, mut last) = (1000u64, 1299u64);
        if let Some(w) = platform::config("GPUTIME_FRAMES", "debug.gputime.frames") {
            match w.split_once('-').map(|(a, b)| (a.trim().parse::<u64>(), b.trim().parse::<u64>())) {
                Some((Ok(a), Ok(b))) if a <= b => (first, last) = (a, b),
                _ => log(&format!("frame window '{w}' is not 'A-B' with A <= B; using {first}-{last}")),
            }
        }
        let per_submit = match platform::config("GPUTIME_FRAME", "debug.gputime.frame").as_deref() {
            None | Some("present") => false,
            Some("submit") => true,
            Some(other) => {
                log(&format!("frame boundary '{other}' is neither 'present' nor 'submit'; using present"));
                false
            }
        };
        let sync = match platform::config("GPUTIME_SYNC", "debug.gputime.sync").as_deref() {
            None | Some("barrier") => true,
            Some("none") => false,
            Some(other) => {
                log(&format!("sync mode '{other}' is neither 'barrier' nor 'none'; using barrier"));
                true
            }
        };
        // GPUTIME_DISPATCH=off: dispatches get no region (no timestamps around them), so back-to-back dispatches
        // run as the driver schedules them; their time shows only in the command buffer row.
        let dispatch_regions = !matches!(platform::config("GPUTIME_DISPATCH", "debug.gputime.dispatch").as_deref(), Some("off"));
        Config { out, sync, first, last, per_submit, dispatch_regions }
    })
}

// ---------------------------------------------------------------------------------------------------------
// Hashing (FNV-1a 64).

const FNV0: u64 = 0xcbf2_9ce4_8422_2325;
fn fnv(mut h: u64, bytes: &[u8]) -> u64 {
    for &b in bytes {
        h ^= b as u64;
        h = h.wrapping_mul(0x100_0000_01b3);
    }
    h
}
fn fold(h: u64, v: u64) -> u64 {
    fnv(h, &v.to_le_bytes())
}

// ---------------------------------------------------------------------------------------------------------
// Next-layer function tables.

macro_rules! device_fns {
    ($($field:ident : $pfn:ident = [$($name:literal),+]),* $(,)?) => {
        #[derive(Default)]
        struct DevFns { $($field: Option<vk::$pfn>,)* }
        impl DevFns {
            unsafe fn load(gdpa: vk::PFN_vkGetDeviceProcAddr, d: vk::Device) -> DevFns {
                DevFns { $($field: {
                    let mut p: Option<vk::$pfn> = None;
                    $(if p.is_none() {
                        p = gdpa(d, concat!($name, "\0").as_ptr() as *const c_char)
                            .map(|f| std::mem::transmute::<unsafe extern "system" fn(), vk::$pfn>(f));
                    })+
                    p
                },)* }
            }
        }
    };
}

device_fns! {
    destroy_device: PFN_vkDestroyDevice = ["vkDestroyDevice"],
    queue_submit: PFN_vkQueueSubmit = ["vkQueueSubmit"],
    queue_submit2: PFN_vkQueueSubmit2 = ["vkQueueSubmit2", "vkQueueSubmit2KHR"],
    queue_present: PFN_vkQueuePresentKHR = ["vkQueuePresentKHR"],
    create_command_pool: PFN_vkCreateCommandPool = ["vkCreateCommandPool"],
    destroy_command_pool: PFN_vkDestroyCommandPool = ["vkDestroyCommandPool"],
    reset_command_pool: PFN_vkResetCommandPool = ["vkResetCommandPool"],
    allocate_command_buffers: PFN_vkAllocateCommandBuffers = ["vkAllocateCommandBuffers"],
    free_command_buffers: PFN_vkFreeCommandBuffers = ["vkFreeCommandBuffers"],
    begin_command_buffer: PFN_vkBeginCommandBuffer = ["vkBeginCommandBuffer"],
    end_command_buffer: PFN_vkEndCommandBuffer = ["vkEndCommandBuffer"],
    reset_command_buffer: PFN_vkResetCommandBuffer = ["vkResetCommandBuffer"],
    create_shader_module: PFN_vkCreateShaderModule = ["vkCreateShaderModule"],
    destroy_shader_module: PFN_vkDestroyShaderModule = ["vkDestroyShaderModule"],
    create_graphics_pipelines: PFN_vkCreateGraphicsPipelines = ["vkCreateGraphicsPipelines"],
    create_compute_pipelines: PFN_vkCreateComputePipelines = ["vkCreateComputePipelines"],
    destroy_pipeline: PFN_vkDestroyPipeline = ["vkDestroyPipeline"],
    create_render_pass: PFN_vkCreateRenderPass = ["vkCreateRenderPass"],
    create_render_pass2: PFN_vkCreateRenderPass2 = ["vkCreateRenderPass2", "vkCreateRenderPass2KHR"],
    destroy_render_pass: PFN_vkDestroyRenderPass = ["vkDestroyRenderPass"],
    create_image_view: PFN_vkCreateImageView = ["vkCreateImageView"],
    destroy_image_view: PFN_vkDestroyImageView = ["vkDestroyImageView"],
    cmd_begin_render_pass: PFN_vkCmdBeginRenderPass = ["vkCmdBeginRenderPass"],
    cmd_begin_render_pass2: PFN_vkCmdBeginRenderPass2 = ["vkCmdBeginRenderPass2", "vkCmdBeginRenderPass2KHR"],
    cmd_end_render_pass: PFN_vkCmdEndRenderPass = ["vkCmdEndRenderPass"],
    cmd_end_render_pass2: PFN_vkCmdEndRenderPass2 = ["vkCmdEndRenderPass2", "vkCmdEndRenderPass2KHR"],
    cmd_begin_rendering: PFN_vkCmdBeginRendering = ["vkCmdBeginRendering", "vkCmdBeginRenderingKHR"],
    cmd_end_rendering: PFN_vkCmdEndRendering = ["vkCmdEndRendering", "vkCmdEndRenderingKHR"],
    cmd_bind_pipeline: PFN_vkCmdBindPipeline = ["vkCmdBindPipeline"],
    cmd_draw: PFN_vkCmdDraw = ["vkCmdDraw"],
    cmd_draw_indexed: PFN_vkCmdDrawIndexed = ["vkCmdDrawIndexed"],
    cmd_draw_indirect: PFN_vkCmdDrawIndirect = ["vkCmdDrawIndirect"],
    cmd_draw_indexed_indirect: PFN_vkCmdDrawIndexedIndirect = ["vkCmdDrawIndexedIndirect"],
    cmd_draw_indirect_count: PFN_vkCmdDrawIndirectCount = ["vkCmdDrawIndirectCount", "vkCmdDrawIndirectCountKHR"],
    cmd_draw_indexed_indirect_count: PFN_vkCmdDrawIndexedIndirectCount =
        ["vkCmdDrawIndexedIndirectCount", "vkCmdDrawIndexedIndirectCountKHR"],
    cmd_dispatch: PFN_vkCmdDispatch = ["vkCmdDispatch"],
    cmd_dispatch_base: PFN_vkCmdDispatchBase = ["vkCmdDispatchBase", "vkCmdDispatchBaseKHR"],
    cmd_dispatch_indirect: PFN_vkCmdDispatchIndirect = ["vkCmdDispatchIndirect"],
    cmd_execute_commands: PFN_vkCmdExecuteCommands = ["vkCmdExecuteCommands"],
    cmd_write_timestamp: PFN_vkCmdWriteTimestamp = ["vkCmdWriteTimestamp"],
    cmd_pipeline_barrier: PFN_vkCmdPipelineBarrier = ["vkCmdPipelineBarrier"],
    cmd_pipeline_barrier2: PFN_vkCmdPipelineBarrier2 = ["vkCmdPipelineBarrier2", "vkCmdPipelineBarrier2KHR"],
    cmd_wait_events: PFN_vkCmdWaitEvents = ["vkCmdWaitEvents"],
    cmd_wait_events2: PFN_vkCmdWaitEvents2 = ["vkCmdWaitEvents2", "vkCmdWaitEvents2KHR"],
    cmd_set_event: PFN_vkCmdSetEvent = ["vkCmdSetEvent"],
    cmd_set_event2: PFN_vkCmdSetEvent2 = ["vkCmdSetEvent2", "vkCmdSetEvent2KHR"],
    cmd_copy_buffer: PFN_vkCmdCopyBuffer = ["vkCmdCopyBuffer"],
    cmd_copy_image: PFN_vkCmdCopyImage = ["vkCmdCopyImage"],
    cmd_blit_image: PFN_vkCmdBlitImage = ["vkCmdBlitImage"],
    cmd_resolve_image: PFN_vkCmdResolveImage = ["vkCmdResolveImage"],
    cmd_copy_buffer_to_image: PFN_vkCmdCopyBufferToImage = ["vkCmdCopyBufferToImage"],
    cmd_copy_image_to_buffer: PFN_vkCmdCopyImageToBuffer = ["vkCmdCopyImageToBuffer"],
    cmd_fill_buffer: PFN_vkCmdFillBuffer = ["vkCmdFillBuffer"],
    cmd_update_buffer: PFN_vkCmdUpdateBuffer = ["vkCmdUpdateBuffer"],
    cmd_clear_color_image: PFN_vkCmdClearColorImage = ["vkCmdClearColorImage"],
    cmd_clear_depth_stencil_image: PFN_vkCmdClearDepthStencilImage = ["vkCmdClearDepthStencilImage"],
    cmd_clear_attachments: PFN_vkCmdClearAttachments = ["vkCmdClearAttachments"],
    get_device_queue: PFN_vkGetDeviceQueue = ["vkGetDeviceQueue"],
    get_device_queue2: PFN_vkGetDeviceQueue2 = ["vkGetDeviceQueue2"],
    create_image: PFN_vkCreateImage = ["vkCreateImage"],
    allocate_memory: PFN_vkAllocateMemory = ["vkAllocateMemory"],
    bind_image_memory: PFN_vkBindImageMemory = ["vkBindImageMemory"],
    bind_image_memory2: PFN_vkBindImageMemory2 = ["vkBindImageMemory2", "vkBindImageMemory2KHR"],
    reset_query_pool: PFN_vkResetQueryPool = ["vkResetQueryPoolEXT", "vkResetQueryPool"],
    create_query_pool: PFN_vkCreateQueryPool = ["vkCreateQueryPool"],
    destroy_query_pool: PFN_vkDestroyQueryPool = ["vkDestroyQueryPool"],
    get_query_pool_results: PFN_vkGetQueryPoolResults = ["vkGetQueryPoolResults"],
}

struct Inst {
    handle: vk::Instance,
    gipa: vk::PFN_vkGetInstanceProcAddr,
    destroy: vk::PFN_vkDestroyInstance,
    props: vk::PFN_vkGetPhysicalDeviceProperties,
    qfam: vk::PFN_vkGetPhysicalDeviceQueueFamilyProperties,
    enum_dev_ext: Option<vk::PFN_vkEnumerateDeviceExtensionProperties>,
}

// ---------------------------------------------------------------------------------------------------------
// Per-device state.

#[derive(Clone, Copy, PartialEq)]
enum Kind {
    RenderPass,
    Rendering,
    Dispatch,
    DispatchIndirect,
}
impl Kind {
    fn name(self) -> &'static str {
        match self {
            Kind::RenderPass => "rp",
            Kind::Rendering => "dr",
            Kind::Dispatch => "cs",
            Kind::DispatchIndirect => "csi",
        }
    }
}

struct Region {
    kind: Kind,
    q: u32,
    fmt: Arc<str>,
    extent: String,
    pipe: u64,
    npipe: u32,
    draws: u32,
}

/// The device's timestamp queries, handed out in chunks of CHUNK. A chunk is reset on the host
/// (vkResetQueryPool, VK_EXT_host_query_reset) when it comes back, i.e. only once no execution that writes it
/// can still be pending; so a query reads as available only after the execution it was handed to wrote it.
/// (A vkCmdResetQueryPool inside the next command buffer would leave the previous values readable until the
/// GPU reaches that reset.)
struct Chunks {
    device: vk::Device,
    pool: vk::QueryPool,
    reset: Option<vk::PFN_vkResetQueryPool>,
    free: Mutex<Vec<u32>>,
}
impl Chunks {
    fn take(&self) -> Option<u32> {
        self.free.lock().ok()?.pop()
    }
    fn reset(&self, chunks: &[u32]) {
        if let Some(r) = self.reset {
            for &c in chunks {
                unsafe { r(self.device, self.pool, c * CHUNK, CHUNK) };
            }
        }
    }
    fn give_back(&self, chunks: &mut Vec<u32>) {
        self.reset(chunks);
        if let Ok(mut f) = self.free.lock() {
            f.extend(chunks.drain(..));
        }
    }
}

/// A finished recording of a primary command buffer. Its query chunks go back to the device when the last
/// reference (the command buffer's own, or a pending submission's) is dropped: by then the command buffer
/// was re-begun, reset or freed (so no execution of it is pending) and every submission of it was read.
struct Recorded {
    id: u64,
    regions: Vec<Region>,
    cb_q: u32,
    chunks: Vec<u32>,
    owner: Arc<Chunks>,
    pending: AtomicU32,
    submissions: AtomicU32,
    skipped: u32,
}
impl Drop for Recorded {
    fn drop(&mut self) {
        let mut c = std::mem::take(&mut self.chunks);
        self.owner.give_back(&mut c);
    }
}

#[derive(Default)]
struct Build {
    id: u64,
    instrumented: bool,
    chunks: Vec<u32>,
    cur: u32,
    end: u32,
    cb_q: u32,
    regions: Vec<Region>,
    open: Option<usize>,
    gfx_pipe: u64,
    cs_pipe: u64,
    last_folded: u64,
    // secondary command buffers: what vkCmdExecuteCommands folds into the open pass
    draws_total: u32,
    pipe_fold: u64,
    npipe_total: u32,
    skipped: u32,
}

#[derive(Default)]
struct CbSt {
    build: Option<Build>,
    recorded: Option<Arc<Recorded>>,
    sec_draws: u32,
    sec_pipe: u64,
    sec_npipe: u32,
}

struct Cb {
    dev: Arc<Dev>,
    pool: u64,
    primary: bool,
    alive: AtomicBool,
    st: Mutex<CbSt>,
}

struct Pending {
    frame: u64,
    items: Vec<(Arc<Recorded>, u32, u32)>, // recording, seq base, cb index in frame
}

#[derive(Default)]
struct FrameAcc {
    outstanding: u32,
    intervals: Vec<(u64, u64)>,
    seq: u32,
    cbs: u32,
    unmeasured_cbs: u32,
    waits: u32,
    skipped: u32,
}

#[derive(Default)]
struct Objects {
    modules: HashMap<u64, u64>,
    pipes: HashMap<u64, u64>,
    render_passes: HashMap<u64, Arc<str>>,
    views: HashMap<u64, i32>,
    cmd_pools: HashMap<u64, u32>,
    queues: HashMap<u64, (u32, u32)>,
}

struct Out {
    csv: BufWriter<File>,
    pipes: BufWriter<File>,
    sync: BufWriter<File>,
}

#[derive(Default)]
struct Harvest {
    out: Option<Out>,
    pending: VecDeque<Pending>,
    frames: BTreeMap<u64, FrameAcc>,
    done: bool,
    warned_pool: bool,
}

struct Dev {
    handle: vk::Device,
    index: u32,
    f: DevFns,
    gdpa: vk::PFN_vkGetDeviceProcAddr,
    ts_period: f64,
    families: Vec<(vk::QueueFlags, u32)>,
    pool: vk::QueryPool,
    chunks: Arc<Chunks>,
    frame: AtomicU64,
    measuring: bool,
    objects: RwLock<Objects>,
    harvest: Mutex<Harvest>,
}

impl Dev {
    fn in_window(&self, frame: u64) -> bool {
        let c = config();
        self.measuring && frame >= c.first && frame <= c.last
    }
    /// Recording may start in the frame before the window (the CPU records ahead of the present).
    fn record_window(&self) -> bool {
        let c = config();
        let f = self.frame.load(Ordering::Relaxed);
        self.measuring && f + 1 >= c.first && f <= c.last
    }
    fn mask(&self, family_ts_bits: u32) -> u64 {
        if family_ts_bits >= 64 { u64::MAX } else { (1u64 << family_ts_bits) - 1 }
    }
}

// ---------------------------------------------------------------------------------------------------------
// Global registries.

fn key<T: Handle>(h: T) -> usize {
    let p = h.as_raw() as *const usize;
    if p.is_null() { 0 } else { unsafe { *p } }
}

fn instances() -> &'static RwLock<HashMap<usize, Arc<Inst>>> {
    static M: OnceLock<RwLock<HashMap<usize, Arc<Inst>>>> = OnceLock::new();
    M.get_or_init(Default::default)
}
fn devices() -> &'static RwLock<HashMap<usize, Arc<Dev>>> {
    static M: OnceLock<RwLock<HashMap<usize, Arc<Dev>>>> = OnceLock::new();
    M.get_or_init(Default::default)
}
fn cbs() -> &'static RwLock<HashMap<u64, Arc<Cb>>> {
    static M: OnceLock<RwLock<HashMap<u64, Arc<Cb>>>> = OnceLock::new();
    M.get_or_init(Default::default)
}
static DEVICE_COUNT: AtomicU32 = AtomicU32::new(0);

fn inst_of<T: Handle>(h: T) -> Option<Arc<Inst>> {
    instances().read().ok()?.get(&key(h)).cloned()
}
fn dev_of<T: Handle>(h: T) -> Option<Arc<Dev>> {
    devices().read().ok()?.get(&key(h)).cloned()
}

thread_local! {
    static LAST_CB: RefCell<Option<(u64, Arc<Cb>)>> = const { RefCell::new(None) };
}

fn cb_of(cb: vk::CommandBuffer) -> Option<Arc<Cb>> {
    let raw = cb.as_raw();
    let hit = LAST_CB.with(|c| match &*c.borrow() {
        Some((h, s)) if *h == raw && s.alive.load(Ordering::Relaxed) => Some(s.clone()),
        _ => None,
    });
    if hit.is_some() {
        return hit;
    }
    let s = cbs().read().ok()?.get(&raw).cloned()?;
    LAST_CB.with(|c| *c.borrow_mut() = Some((raw, s.clone())));
    Some(s)
}

/// Next-layer entry point for a command-buffer command, for a command buffer this layer does not know.
fn dev_for_cb(cb: vk::CommandBuffer) -> Arc<Dev> {
    match cb_of(cb) {
        Some(c) => c.dev.clone(),
        None => dev_of(cb).expect("VK_LAYER_PERF_gputime: command buffer of an unknown device"),
    }
}

// ---------------------------------------------------------------------------------------------------------
// Recording helpers.

impl Build {
    /// Two consecutive queries (already reset on the host).
    unsafe fn alloc_pair(&mut self, dev: &Dev, _cb: vk::CommandBuffer, warned: &mut bool) -> Option<u32> {
        if self.cur + 2 > self.end {
            let chunk = dev.chunks.take();
            let Some(chunk) = chunk else {
                if !*warned {
                    *warned = true;
                    log(&format!("device {}: timestamp pool exhausted; regions are counted as skipped", dev.index));
                }
                return None;
            };
            self.chunks.push(chunk);
            self.cur = chunk * CHUNK;
            self.end = self.cur + CHUNK;
        }
        let q = self.cur;
        self.cur += 2;
        Some(q)
    }

    fn count_draw(&mut self) {
        self.draws_total += 1;
        if self.gfx_pipe != self.last_folded {
            self.last_folded = self.gfx_pipe;
            self.pipe_fold = fold(self.pipe_fold, self.gfx_pipe);
            self.npipe_total += 1;
            if let Some(i) = self.open {
                let r = &mut self.regions[i];
                r.pipe = fold(r.pipe, self.gfx_pipe);
                r.npipe += 1;
            }
        }
        if let Some(i) = self.open {
            self.regions[i].draws += 1;
        }
    }
}

fn warned_pool(dev: &Dev) -> bool {
    dev.harvest.lock().map(|h| h.warned_pool).unwrap_or(true)
}
fn set_warned_pool(dev: &Dev) {
    if let Ok(mut h) = dev.harvest.lock() {
        h.warned_pool = true;
    }
}

unsafe fn begin_region(c: &Cb, cb: vk::CommandBuffer, kind: Kind, fmt: Arc<str>, extent: String, pipe: u64) {
    let dev = &c.dev;
    let mut st = c.st.lock().unwrap();
    let Some(b) = st.build.as_mut() else { return };
    if !b.instrumented {
        return;
    }
    let mut warned = warned_pool(dev);
    let was = warned;
    let q = b.alloc_pair(dev, cb, &mut warned);
    if warned && !was {
        set_warned_pool(dev);
    }
    let Some(q) = q else {
        b.skipped += 1;
        return;
    };
    timestamp_after_all(dev, cb, q);
    let (npipe, pipe) = if kind == Kind::Dispatch || kind == Kind::DispatchIndirect { (1, pipe) } else { (0, FNV0) };
    b.regions.push(Region { kind, q, fmt, extent, pipe, npipe, draws: 0 });
    b.open = Some(b.regions.len() - 1);
    b.last_folded = 0;
}

unsafe fn end_region(c: &Cb, cb: vk::CommandBuffer) {
    let mut st = c.st.lock().unwrap();
    let Some(b) = st.build.as_mut() else { return };
    if let Some(i) = b.open.take() {
        let q = b.regions[i].q + 1;
        timestamp_after_all(&c.dev, cb, q);
    }
}

/// BOTTOM_OF_PIPE timestamp; in barrier mode preceded by a barrier that makes all earlier work of the queue
/// complete first (source ALL_COMMANDS + MEMORY_WRITE, destination ALL_COMMANDS with no access, so nothing is
/// invalidated). Without it a timestamp may be written while earlier fragment or compute work still runs:
/// NVK writes it with a report at the end of the 3D pipeline and waits for idle only for barriers that name a
/// write access (src/nouveau/vulkan/nvk_query_pool.c nvk_CmdWriteTimestamp2,
/// nvk_cmd_buffer.c nvk_barrier_flushes_waits).
unsafe fn timestamp_after_all(dev: &Dev, cb: vk::CommandBuffer, q: u32) {
    if config().sync {
        let mb = vk::MemoryBarrier {
            src_access_mask: vk::AccessFlags::MEMORY_WRITE,
            dst_access_mask: vk::AccessFlags::empty(),
            ..Default::default()
        };
        (dev.f.cmd_pipeline_barrier.unwrap())(
            cb,
            vk::PipelineStageFlags::ALL_COMMANDS,
            vk::PipelineStageFlags::ALL_COMMANDS,
            vk::DependencyFlags::empty(),
            1,
            &mb,
            0,
            std::ptr::null(),
            0,
            std::ptr::null(),
        );
    }
    (dev.f.cmd_write_timestamp.unwrap())(cb, vk::PipelineStageFlags::BOTTOM_OF_PIPE, dev.pool, q);
}

fn fmt_list(formats: impl Iterator<Item = i32>) -> Arc<str> {
    let mut s = String::new();
    for f in formats {
        if !s.is_empty() {
            s.push('+');
        }
        let _ = write!(s, "{f}");
    }
    Arc::from(s)
}

// ---------------------------------------------------------------------------------------------------------
// Read-back and output.

unsafe fn read_pair(dev: &Dev, q: u32, wait: bool) -> Option<(u64, u64)> {
    let mut r = [0u64; 4];
    let mut flags = vk::QueryResultFlags::TYPE_64 | vk::QueryResultFlags::WITH_AVAILABILITY;
    if wait {
        flags |= vk::QueryResultFlags::WAIT;
    }
    let res = (dev.f.get_query_pool_results.unwrap())(
        dev.handle,
        dev.pool,
        q,
        2,
        std::mem::size_of_val(&r),
        r.as_mut_ptr() as *mut c_void,
        16,
        flags,
    );
    if res != vk::Result::SUCCESS || r[1] == 0 || r[3] == 0 {
        return None;
    }
    Some((r[0], r[2]))
}

/// Reads every pending submission whose queries are all available (`wait_for`: block on the submissions
/// holding that recording), writes their rows, and closes the frames that are complete.
unsafe fn harvest(dev: &Dev, h: &mut Harvest, wait_for: Option<&Arc<Recorded>>) {
    let mask = dev.mask(dev.families.first().map(|f| f.1).unwrap_or(64));
    let ns = |t: u64| ((t & mask) as f64 * dev.ts_period) as u64;
    let dt = |s: u64, e: u64| (((e.wrapping_sub(s)) & mask) as f64 * dev.ts_period) as u64;
    let mut i = 0;
    while i < h.pending.len() {
        let wait = wait_for.is_some_and(|w| h.pending[i].items.iter().any(|(r, _, _)| Arc::ptr_eq(r, w)));
        let mut rows: Vec<String> = Vec::new();
        let mut ivs: Vec<(u64, u64)> = Vec::new();
        let mut complete = true;
        let frame = h.pending[i].frame;
        'items: for (rec, base, cbi) in &h.pending[i].items {
            let Some((cs, ce)) = read_pair(dev, rec.cb_q, wait) else {
                complete = false;
                break 'items;
            };
            for (k, r) in rec.regions.iter().enumerate() {
                let Some((s, e)) = read_pair(dev, r.q, wait) else {
                    complete = false;
                    break 'items;
                };
                rows.push(format!(
                    "{frame},{},{cbi},{},{},{},{:016x},{},{},{},{},\n",
                    base + k as u32,
                    r.kind.name(),
                    r.fmt,
                    r.extent,
                    r.pipe,
                    r.npipe,
                    r.draws,
                    dt(s, e),
                    ns(s)
                ));
            }
            rows.push(format!(
                "{frame},{cbi},{cbi},cb,,,0,0,{},{},{},regions={};skipped={};rec={}\n",
                rec.regions.iter().map(|r| r.draws).sum::<u32>(),
                dt(cs, ce),
                ns(cs),
                rec.regions.len(),
                rec.skipped,
                rec.id
            ));
            ivs.push((ns(cs), ns(cs) + dt(cs, ce)));
        }
        if !complete {
            i += 1;
            continue;
        }
        let p = h.pending.remove(i).unwrap();
        for (rec, _, _) in &p.items {
            rec.pending.fetch_sub(1, Ordering::AcqRel);
        }
        if let Some(o) = h.out.as_mut() {
            for r in &rows {
                let _ = o.csv.write_all(r.as_bytes());
            }
        }
        if let Some(fa) = h.frames.get_mut(&p.frame) {
            fa.outstanding -= 1;
            fa.intervals.extend(ivs);
        }
    }
    close_frames(dev, h);
    // Applications are often killed without vkDestroyDevice: keep the file complete at every read-back.
    if let Some(o) = h.out.as_mut() {
        let _ = o.csv.flush();
        let _ = o.sync.flush();
    }
}

fn close_frames(dev: &Dev, h: &mut Harvest) {
    let cur = dev.frame.load(Ordering::Relaxed);
    let ready: Vec<u64> = h.frames.iter().filter(|(f, a)| **f < cur && a.outstanding == 0).map(|(f, _)| *f).collect();
    for f in ready {
        let mut a = h.frames.remove(&f).unwrap();
        a.intervals.sort_unstable();
        let (mut busy, mut cs, mut ce) = (0u64, 0u64, 0u64);
        for (k, &(s, e)) in a.intervals.iter().enumerate() {
            if k > 0 && s <= ce {
                ce = ce.max(e);
                continue;
            }
            busy += ce - cs;
            cs = s;
            ce = e;
        }
        busy += ce - cs;
        let first = a.intervals.first().map(|i| i.0).unwrap_or(0);
        let last = a.intervals.iter().map(|i| i.1).max().unwrap_or(0);
        if let Some(o) = h.out.as_mut() {
            let _ = writeln!(
                o.csv,
                "{f},,,frame,,,0,0,0,{busy},{first},span={};cbs={};unmeasured_cbs={};waits={};skipped={}",
                last - first,
                a.cbs,
                a.unmeasured_cbs,
                a.waits,
                a.skipped
            );
        }
    }
    let c = config();
    if !h.done && cur > c.last && h.pending.is_empty() && h.frames.is_empty() {
        h.done = true;
        if let Some(mut o) = h.out.take() {
            let _ = writeln!(o.csv, "# end of window {}-{}", c.first, c.last);
            let _ = o.csv.flush();
            let _ = o.pipes.flush();
            log(&format!("device {}: frame window {}-{} written", dev.index, c.first, c.last));
        }
    }
}

// ---------------------------------------------------------------------------------------------------------
// Instance-level entry points.

unsafe extern "system" fn layer_CreateInstance(
    ci: *const vk::InstanceCreateInfo,
    alloc: *const vk::AllocationCallbacks,
    out: *mut vk::Instance,
) -> vk::Result {
    let mut li = (*ci).p_next as *mut LayerInstanceCreateInfo;
    while !li.is_null()
        && !((*li).s_type == vk::StructureType::LOADER_INSTANCE_CREATE_INFO && (*li).function == LAYER_LINK_INFO)
    {
        li = (*li).p_next as *mut LayerInstanceCreateInfo;
    }
    if li.is_null() {
        log("vkCreateInstance: no loader link info");
        return vk::Result::ERROR_INITIALIZATION_FAILED;
    }
    let link = (*li).u as *mut LayerInstanceLink;
    let gipa = (*link).next_gipa;
    (*li).u = (*link).next as *mut c_void;
    let Some(create) = gipa(vk::Instance::null(), c"vkCreateInstance".as_ptr()) else {
        return vk::Result::ERROR_INITIALIZATION_FAILED;
    };
    let create: vk::PFN_vkCreateInstance = std::mem::transmute(create);
    let r = create(ci, alloc, out);
    if r != vk::Result::SUCCESS {
        return r;
    }
    let inst = *out;
    let get = |n: &CStr| gipa(inst, n.as_ptr());
    let Some(destroy) = get(c"vkDestroyInstance") else { return vk::Result::ERROR_INITIALIZATION_FAILED };
    let Some(props) = get(c"vkGetPhysicalDeviceProperties") else { return vk::Result::ERROR_INITIALIZATION_FAILED };
    let Some(qfam) = get(c"vkGetPhysicalDeviceQueueFamilyProperties") else {
        return vk::Result::ERROR_INITIALIZATION_FAILED;
    };
    let i = Inst {
        handle: inst,
        gipa,
        destroy: std::mem::transmute(destroy),
        props: std::mem::transmute(props),
        qfam: std::mem::transmute(qfam),
        enum_dev_ext: get(c"vkEnumerateDeviceExtensionProperties").map(|f| std::mem::transmute(f)),
    };
    instances().write().unwrap().insert(key(inst), Arc::new(i));
    vk::Result::SUCCESS
}

unsafe extern "system" fn layer_DestroyInstance(inst: vk::Instance, alloc: *const vk::AllocationCallbacks) {
    let i = instances().write().unwrap().remove(&key(inst));
    if let Some(i) = i {
        (i.destroy)(inst, alloc);
    }
}

unsafe extern "system" fn layer_CreateDevice(
    pd: vk::PhysicalDevice,
    ci: *const vk::DeviceCreateInfo,
    alloc: *const vk::AllocationCallbacks,
    out: *mut vk::Device,
) -> vk::Result {
    let Some(inst) = inst_of(pd) else {
        log("vkCreateDevice: physical device of an unknown instance");
        return vk::Result::ERROR_INITIALIZATION_FAILED;
    };
    let mut li = (*ci).p_next as *mut LayerDeviceCreateInfo;
    while !li.is_null()
        && !((*li).s_type == vk::StructureType::LOADER_DEVICE_CREATE_INFO && (*li).function == LAYER_LINK_INFO)
    {
        li = (*li).p_next as *mut LayerDeviceCreateInfo;
    }
    if li.is_null() {
        log("vkCreateDevice: no loader link info");
        return vk::Result::ERROR_INITIALIZATION_FAILED;
    }
    let _ = LOADER_DATA_CALLBACK; // this layer creates no dispatchable objects of its own
    let link = (*li).u as *mut LayerDeviceLink;
    let gipa = (*link).next_gipa;
    let gdpa = (*link).next_gdpa;
    (*li).u = (*link).next as *mut c_void;
    let Some(create) = gipa(inst.handle, c"vkCreateDevice".as_ptr()) else {
        return vk::Result::ERROR_INITIALIZATION_FAILED;
    };
    let create: vk::PFN_vkCreateDevice = std::mem::transmute(create);
    // Measuring needs host query reset (see Chunks): enable VK_EXT_host_query_reset and its feature on the
    // application's device, the feature through the application's own Vulkan12/HostQueryReset feature struct
    // when it chains one (set for the duration of the call, then restored), else through ours, appended at
    // the end of the chain (Android's loader strips its own link structs only from the front of the chain).
    let ext_name: &CStr = c"VK_EXT_host_query_reset";
    let want = config().out.is_some() && device_has_extension(&inst, pd, ext_name);
    if config().out.is_some() && !want {
        log("vkCreateDevice: VK_EXT_host_query_reset not supported, not measuring");
    }
    let mut ci2: vk::DeviceCreateInfo = *ci;
    let mut exts: Vec<*const c_char> = if (*ci).enabled_extension_count > 0 {
        std::slice::from_raw_parts((*ci).pp_enabled_extension_names, (*ci).enabled_extension_count as usize).to_vec()
    } else {
        Vec::new()
    };
    let mut hq = vk::PhysicalDeviceHostQueryResetFeatures { host_query_reset: vk::TRUE, ..Default::default() };
    let mut restore: Option<(*mut vk::Bool32, vk::Bool32)> = None;
    let mut unlink: Option<*mut vk::BaseOutStructure> = None;
    if want {
        if !exts.iter().any(|e| CStr::from_ptr(*e) == ext_name) {
            exts.push(ext_name.as_ptr());
        }
        ci2.enabled_extension_count = exts.len() as u32;
        ci2.pp_enabled_extension_names = exts.as_ptr();
        let mut p = (*ci).p_next as *mut vk::BaseOutStructure;
        while !p.is_null() {
            let field = match (*p).s_type {
                vk::StructureType::PHYSICAL_DEVICE_VULKAN_1_2_FEATURES => {
                    Some(&mut (*(p as *mut vk::PhysicalDeviceVulkan12Features)).host_query_reset as *mut vk::Bool32)
                }
                vk::StructureType::PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES => {
                    Some(&mut (*(p as *mut vk::PhysicalDeviceHostQueryResetFeatures)).host_query_reset as *mut vk::Bool32)
                }
                _ => None,
            };
            if let Some(f) = field {
                restore = Some((f, *f));
                *f = vk::TRUE;
                break;
            }
            p = (*p).p_next;
        }
        if restore.is_none() {
            let mut last = (*ci).p_next as *mut vk::BaseOutStructure;
            if last.is_null() {
                ci2.p_next = &hq as *const _ as *const c_void;
            } else {
                while !(*last).p_next.is_null() {
                    last = (*last).p_next;
                }
                (*last).p_next = &mut hq as *mut _ as *mut vk::BaseOutStructure;
                unlink = Some(last);
            }
        }
    }
    let r = create(pd, &ci2, alloc, out);
    if let Some((f, v)) = restore {
        *f = v;
    }
    if let Some(last) = unlink {
        (*last).p_next = std::ptr::null_mut();
    }
    if r != vk::Result::SUCCESS {
        return r;
    }
    let d = *out;
    let f = DevFns::load(gdpa, d);
    let mut props = vk::PhysicalDeviceProperties::default();
    (inst.props)(pd, &mut props);
    let mut n = 0u32;
    (inst.qfam)(pd, &mut n, std::ptr::null_mut());
    let mut qf = vec![vk::QueueFamilyProperties::default(); n as usize];
    (inst.qfam)(pd, &mut n, qf.as_mut_ptr());
    let families: Vec<(vk::QueueFlags, u32)> =
        qf.iter().take(n as usize).map(|q| (q.queue_flags, q.timestamp_valid_bits)).collect();
    let index = DEVICE_COUNT.fetch_add(1, Ordering::Relaxed);
    let name = CStr::from_ptr(props.device_name.as_ptr()).to_string_lossy().into_owned();

    let mut pool = vk::QueryPool::null();
    let mut measuring = false;
    let mut harvest = Harvest::default();
    let cfg = config();
    let needed = f.cmd_write_timestamp.is_some()
        && f.cmd_pipeline_barrier.is_some()
        && f.reset_query_pool.is_some()
        && f.create_query_pool.is_some()
        && f.get_query_pool_results.is_some();
    if let Some(prefix) = cfg.out.as_deref().filter(|_| want) {
        if !needed {
            log(&format!("device {index} ({name}): timestamp entry points missing, not measuring"));
        } else if props.limits.timestamp_compute_and_graphics == vk::FALSE {
            log(&format!("device {index} ({name}): timestampComputeAndGraphics is false, not measuring"));
        } else {
            let qpi = vk::QueryPoolCreateInfo {
                query_type: vk::QueryType::TIMESTAMP,
                query_count: POOL_QUERIES,
                ..Default::default()
            };
            let r = (f.create_query_pool.unwrap())(d, &qpi, std::ptr::null(), &mut pool);
            if r != vk::Result::SUCCESS {
                log(&format!("device {index} ({name}): vkCreateQueryPool failed ({r:?}), not measuring"));
            } else {
                let pid = std::process::id();
                let csv_path = format!("{prefix}-{pid}-d{index}.csv");
                let pipe_path = format!("{prefix}-{pid}-d{index}-pipelines.csv");
                let sync_path = format!("{prefix}-{pid}-d{index}-sync.csv");
                match (File::create(&csv_path), File::create(&pipe_path), File::create(&sync_path)) {
                    (Ok(c), Ok(p), Ok(sy)) => {
                        let mut o = Out { csv: BufWriter::new(c), pipes: BufWriter::new(p), sync: BufWriter::new(sy) };
                        let _ = writeln!(
                            o.csv,
                            "# device={name} vendor=0x{:04x} device_id=0x{:04x} driver_version=0x{:08x} api=0x{:08x} \
                             timestamp_period={} frames={}-{} boundary={} sync={}",
                            props.vendor_id,
                            props.device_id,
                            props.driver_version,
                            props.api_version,
                            props.limits.timestamp_period,
                            cfg.first,
                            cfg.last,
                            if cfg.per_submit { "submit" } else { "present" },
                            if cfg.sync { "barrier" } else { "none" }
                        );
                        let _ = writeln!(o.csv, "frame,seq,cb,kind,fmt,extent,pipe,npipe,draws,gpu_ns,start_ns,note");
                        let _ = writeln!(o.pipes, "pipe,bind,stages");
                        let _ = writeln!(o.sync, "ev,frame,rec,ridx,inrp,detail");
                        for (k, q) in qf.iter().enumerate() {
                            let _ = writeln!(o.sync, "family,,,,,{k},{:x},{}", q.queue_flags.as_raw(), q.queue_count);
                        }
                        for k in 0..(*ci).queue_create_info_count as usize {
                            let q = &*(*ci).p_queue_create_infos.add(k);
                            let _ = writeln!(o.sync, "qcreate,,,,,{},{},f{:x}", q.queue_family_index, q.queue_count, q.flags.as_raw());
                        }
                        if let Some(gmp) = (inst.gipa)(inst.handle, c"vkGetPhysicalDeviceMemoryProperties".as_ptr()) {
                            let gmp: vk::PFN_vkGetPhysicalDeviceMemoryProperties = std::mem::transmute(gmp);
                            let mut mp = vk::PhysicalDeviceMemoryProperties::default();
                            gmp(pd, &mut mp);
                            for k in 0..mp.memory_type_count as usize {
                                let t = mp.memory_types[k];
                                let _ = writeln!(o.sync, "mtype,,,,,{k},{:x},{}", t.property_flags.as_raw(), t.heap_index);
                            }
                        }
                        harvest.out = Some(o);
                        measuring = true;
                        log(&format!("device {index} ({name}): writing {csv_path}"));
                    }
                    (c, p, sy) => log(&format!(
                        "device {index}: cannot create output ({:?} / {:?} / {:?}), not measuring",
                        c.err(),
                        p.err(),
                        sy.err()
                    )),
                }
            }
        }
    }
    let f_reset = f.reset_query_pool;
    if measuring {
        (f_reset.unwrap())(d, pool, 0, POOL_QUERIES);
    }
    let dev = Arc::new(Dev {
        handle: d,
        index,
        f,
        gdpa,
        ts_period: props.limits.timestamp_period as f64,
        families,
        pool,
        chunks: Arc::new(Chunks {
            device: d,
            pool,
            reset: f_reset,
            free: Mutex::new(if measuring { (0..POOL_QUERIES / CHUNK).rev().collect() } else { Vec::new() }),
        }),
        frame: AtomicU64::new(0),
        measuring,
        objects: RwLock::new(Objects::default()),
        harvest: Mutex::new(harvest),
    });
    devices().write().unwrap().insert(key(d), dev);
    vk::Result::SUCCESS
}

unsafe fn device_has_extension(inst: &Inst, pd: vk::PhysicalDevice, name: &CStr) -> bool {
    let Some(f) = inst.enum_dev_ext else { return false };
    let mut n = 0u32;
    if f(pd, std::ptr::null(), &mut n, std::ptr::null_mut()) != vk::Result::SUCCESS {
        return false;
    }
    let mut v = vec![vk::ExtensionProperties::default(); n as usize];
    if f(pd, std::ptr::null(), &mut n, v.as_mut_ptr()) != vk::Result::SUCCESS {
        return false;
    }
    v.iter().take(n as usize).any(|e| CStr::from_ptr(e.extension_name.as_ptr()) == name)
}

unsafe extern "system" fn layer_DestroyDevice(d: vk::Device, alloc: *const vk::AllocationCallbacks) {
    let Some(dev) = devices().write().unwrap().remove(&key(d)) else { return };
    {
        let mut h = dev.harvest.lock().unwrap();
        if let Some(o) = h.out.as_mut() {
            let _ = o.csv.flush();
            let _ = o.pipes.flush();
        }
        h.pending.clear();
    }
    cbs().write().unwrap().retain(|_, c| {
        let mine = Arc::ptr_eq(&c.dev, &dev);
        if mine {
            c.alive.store(false, Ordering::Relaxed);
        }
        !mine
    });
    if dev.pool != vk::QueryPool::null() {
        (dev.f.destroy_query_pool.unwrap())(d, dev.pool, std::ptr::null());
    }
    (dev.f.destroy_device.unwrap())(d, alloc);
}

// ---------------------------------------------------------------------------------------------------------
// Object tracking.

unsafe extern "system" fn layer_CreateShaderModule(
    d: vk::Device,
    ci: *const vk::ShaderModuleCreateInfo,
    a: *const vk::AllocationCallbacks,
    out: *mut vk::ShaderModule,
) -> vk::Result {
    let dev = dev_of(d).unwrap();
    let r = (dev.f.create_shader_module.unwrap())(d, ci, a, out);
    if r == vk::Result::SUCCESS {
        let code = std::slice::from_raw_parts((*ci).p_code as *const u8, (*ci).code_size);
        dev.objects.write().unwrap().modules.insert((*out).as_raw(), fnv(FNV0, code));
    }
    r
}

unsafe extern "system" fn layer_DestroyShaderModule(d: vk::Device, m: vk::ShaderModule, a: *const vk::AllocationCallbacks) {
    let dev = dev_of(d).unwrap();
    dev.objects.write().unwrap().modules.remove(&m.as_raw());
    (dev.f.destroy_shader_module.unwrap())(d, m, a)
}

/// Hash of one shader stage: stage bit, SPIR-V hash (module, or an inline VkShaderModuleCreateInfo in the
/// stage's pNext), entry point, specialization map and data. Also returns "stage:spirvhash:entry".
unsafe fn stage_hash(o: &Objects, s: &vk::PipelineShaderStageCreateInfo) -> (u64, String) {
    let mut spirv = 0u64;
    if s.module != vk::ShaderModule::null() {
        spirv = o.modules.get(&s.module.as_raw()).copied().unwrap_or(0);
    } else {
        let mut p = s.p_next as *const vk::BaseInStructure;
        while !p.is_null() {
            if (*p).s_type == vk::StructureType::SHADER_MODULE_CREATE_INFO {
                let m = &*(p as *const vk::ShaderModuleCreateInfo);
                spirv = fnv(FNV0, std::slice::from_raw_parts(m.p_code as *const u8, m.code_size));
            }
            p = (*p).p_next;
        }
    }
    let entry = if s.p_name.is_null() { &b""[..] } else { CStr::from_ptr(s.p_name).to_bytes() };
    let mut h = fold(fold(FNV0, s.stage.as_raw() as u64), spirv);
    h = fnv(h, entry);
    if !s.p_specialization_info.is_null() {
        let si = &*s.p_specialization_info;
        if si.map_entry_count > 0 && !si.p_map_entries.is_null() {
            let m = std::slice::from_raw_parts(
                si.p_map_entries as *const u8,
                si.map_entry_count as usize * std::mem::size_of::<vk::SpecializationMapEntry>(),
            );
            h = fnv(h, m);
        }
        if si.data_size > 0 && !si.p_data.is_null() {
            h = fnv(h, std::slice::from_raw_parts(si.p_data as *const u8, si.data_size));
        }
    }
    let name = match s.stage {
        vk::ShaderStageFlags::VERTEX => "vert",
        vk::ShaderStageFlags::FRAGMENT => "frag",
        vk::ShaderStageFlags::COMPUTE => "comp",
        vk::ShaderStageFlags::GEOMETRY => "geom",
        vk::ShaderStageFlags::TESSELLATION_CONTROL => "tesc",
        vk::ShaderStageFlags::TESSELLATION_EVALUATION => "tese",
        _ => "other",
    };
    (h, format!("{name}:{spirv:016x}:{}", String::from_utf8_lossy(entry)))
}

unsafe fn record_pipeline(dev: &Dev, pipe: vk::Pipeline, bind: &str, h: u64, desc: &str) {
    dev.objects.write().unwrap().pipes.insert(pipe.as_raw(), h);
    let mut hv = dev.harvest.lock().unwrap();
    if let Some(o) = hv.out.as_mut() {
        let _ = writeln!(o.pipes, "{h:016x},{bind},{desc}");
        let _ = o.pipes.flush();
    }
}

unsafe extern "system" fn layer_CreateGraphicsPipelines(
    d: vk::Device,
    cache: vk::PipelineCache,
    n: u32,
    ci: *const vk::GraphicsPipelineCreateInfo,
    a: *const vk::AllocationCallbacks,
    out: *mut vk::Pipeline,
) -> vk::Result {
    let dev = dev_of(d).unwrap();
    let r = (dev.f.create_graphics_pipelines.unwrap())(d, cache, n, ci, a, out);
    for i in 0..n as usize {
        let p = *out.add(i);
        if p == vk::Pipeline::null() {
            continue;
        }
        let info = &*ci.add(i);
        let (h, desc) = {
            let o = dev.objects.read().unwrap();
            let mut h = fold(FNV0, 1);
            let mut desc = String::new();
            for k in 0..info.stage_count as usize {
                let (sh, sd) = stage_hash(&o, &*info.p_stages.add(k));
                h = fold(h, sh);
                if !desc.is_empty() {
                    desc.push('|');
                }
                desc.push_str(&sd);
            }
            let mut pn = info.p_next as *const vk::BaseInStructure;
            while !pn.is_null() {
                if (*pn).s_type == vk::StructureType::PIPELINE_LIBRARY_CREATE_INFO_KHR {
                    let l = &*(pn as *const vk::PipelineLibraryCreateInfoKHR);
                    for j in 0..l.library_count as usize {
                        let lib = (*l.p_libraries.add(j)).as_raw();
                        let lh = o.pipes.get(&lib).copied().unwrap_or(0);
                        h = fold(h, lh);
                        if !desc.is_empty() {
                            desc.push('|');
                        }
                        let _ = write!(desc, "lib:{lh:016x}");
                    }
                }
                pn = (*pn).p_next;
            }
            (h, desc)
        };
        record_pipeline(&dev, p, "gfx", h, &desc);
    }
    r
}

unsafe extern "system" fn layer_CreateComputePipelines(
    d: vk::Device,
    cache: vk::PipelineCache,
    n: u32,
    ci: *const vk::ComputePipelineCreateInfo,
    a: *const vk::AllocationCallbacks,
    out: *mut vk::Pipeline,
) -> vk::Result {
    let dev = dev_of(d).unwrap();
    let r = (dev.f.create_compute_pipelines.unwrap())(d, cache, n, ci, a, out);
    for i in 0..n as usize {
        let p = *out.add(i);
        if p == vk::Pipeline::null() {
            continue;
        }
        let (sh, sd) = stage_hash(&dev.objects.read().unwrap(), &(*ci.add(i)).stage);
        record_pipeline(&dev, p, "cs", fold(fold(FNV0, 2), sh), &sd);
    }
    r
}

unsafe extern "system" fn layer_DestroyPipeline(d: vk::Device, p: vk::Pipeline, a: *const vk::AllocationCallbacks) {
    let dev = dev_of(d).unwrap();
    dev.objects.write().unwrap().pipes.remove(&p.as_raw());
    (dev.f.destroy_pipeline.unwrap())(d, p, a)
}

unsafe extern "system" fn layer_CreateRenderPass(
    d: vk::Device,
    ci: *const vk::RenderPassCreateInfo,
    a: *const vk::AllocationCallbacks,
    out: *mut vk::RenderPass,
) -> vk::Result {
    let dev = dev_of(d).unwrap();
    let r = (dev.f.create_render_pass.unwrap())(d, ci, a, out);
    if r == vk::Result::SUCCESS {
        let att = std::slice::from_raw_parts((*ci).p_attachments, (*ci).attachment_count as usize);
        let s = fmt_list(att.iter().map(|x| x.format.as_raw()));
        dev.objects.write().unwrap().render_passes.insert((*out).as_raw(), s);
        let (a, d) = synclog::rp1_rows(&*ci);
        synclog::note_render_pass(&dev, *out, &a, &d);
    }
    r
}

unsafe extern "system" fn layer_CreateRenderPass2(
    d: vk::Device,
    ci: *const vk::RenderPassCreateInfo2,
    a: *const vk::AllocationCallbacks,
    out: *mut vk::RenderPass,
) -> vk::Result {
    let dev = dev_of(d).unwrap();
    let r = (dev.f.create_render_pass2.unwrap())(d, ci, a, out);
    if r == vk::Result::SUCCESS {
        let att = std::slice::from_raw_parts((*ci).p_attachments, (*ci).attachment_count as usize);
        let s = fmt_list(att.iter().map(|x| x.format.as_raw()));
        dev.objects.write().unwrap().render_passes.insert((*out).as_raw(), s);
        let (a, d) = synclog::rp2_rows(&*ci);
        synclog::note_render_pass(&dev, *out, &a, &d);
    }
    r
}

unsafe extern "system" fn layer_DestroyRenderPass(d: vk::Device, rp: vk::RenderPass, a: *const vk::AllocationCallbacks) {
    let dev = dev_of(d).unwrap();
    dev.objects.write().unwrap().render_passes.remove(&rp.as_raw());
    (dev.f.destroy_render_pass.unwrap())(d, rp, a)
}

unsafe extern "system" fn layer_CreateImageView(
    d: vk::Device,
    ci: *const vk::ImageViewCreateInfo,
    a: *const vk::AllocationCallbacks,
    out: *mut vk::ImageView,
) -> vk::Result {
    let dev = dev_of(d).unwrap();
    let r = (dev.f.create_image_view.unwrap())(d, ci, a, out);
    if r == vk::Result::SUCCESS {
        dev.objects.write().unwrap().views.insert((*out).as_raw(), (*ci).format.as_raw());
    }
    r
}

unsafe extern "system" fn layer_DestroyImageView(d: vk::Device, v: vk::ImageView, a: *const vk::AllocationCallbacks) {
    let dev = dev_of(d).unwrap();
    dev.objects.write().unwrap().views.remove(&v.as_raw());
    (dev.f.destroy_image_view.unwrap())(d, v, a)
}

// ---------------------------------------------------------------------------------------------------------
// Command pools and buffers.

unsafe extern "system" fn layer_CreateCommandPool(
    d: vk::Device,
    ci: *const vk::CommandPoolCreateInfo,
    a: *const vk::AllocationCallbacks,
    out: *mut vk::CommandPool,
) -> vk::Result {
    let dev = dev_of(d).unwrap();
    let r = (dev.f.create_command_pool.unwrap())(d, ci, a, out);
    if r == vk::Result::SUCCESS {
        dev.objects.write().unwrap().cmd_pools.insert((*out).as_raw(), (*ci).queue_family_index);
    }
    r
}

fn forget_cbs(dev: &Arc<Dev>, pred: impl Fn(&Cb, u64) -> bool) {
    cbs().write().unwrap().retain(|h, c| {
        let gone = Arc::ptr_eq(&c.dev, dev) && pred(c, *h);
        if gone {
            c.alive.store(false, Ordering::Relaxed);
        }
        !gone
    });
}

unsafe extern "system" fn layer_DestroyCommandPool(d: vk::Device, p: vk::CommandPool, a: *const vk::AllocationCallbacks) {
    let dev = dev_of(d).unwrap();
    forget_cbs(&dev, |c, _| c.pool == p.as_raw());
    dev.objects.write().unwrap().cmd_pools.remove(&p.as_raw());
    (dev.f.destroy_command_pool.unwrap())(d, p, a)
}

unsafe extern "system" fn layer_ResetCommandPool(d: vk::Device, p: vk::CommandPool, fl: vk::CommandPoolResetFlags) -> vk::Result {
    let dev = dev_of(d).unwrap();
    for c in cbs().read().unwrap().values() {
        if Arc::ptr_eq(&c.dev, &dev) && c.pool == p.as_raw() {
            let mut st = c.st.lock().unwrap();
            st.build = None;
            st.recorded = None;
        }
    }
    (dev.f.reset_command_pool.unwrap())(d, p, fl)
}

unsafe extern "system" fn layer_AllocateCommandBuffers(
    d: vk::Device,
    ai: *const vk::CommandBufferAllocateInfo,
    out: *mut vk::CommandBuffer,
) -> vk::Result {
    let dev = dev_of(d).unwrap();
    let r = (dev.f.allocate_command_buffers.unwrap())(d, ai, out);
    if r == vk::Result::SUCCESS {
        let mut m = cbs().write().unwrap();
        for i in 0..(*ai).command_buffer_count as usize {
            let cb = *out.add(i);
            let c = Cb {
                dev: dev.clone(),
                pool: (*ai).command_pool.as_raw(),
                primary: (*ai).level == vk::CommandBufferLevel::PRIMARY,
                alive: AtomicBool::new(true),
                st: Mutex::new(CbSt::default()),
            };
            if let Some(old) = m.insert(cb.as_raw(), Arc::new(c)) {
                old.alive.store(false, Ordering::Relaxed);
            }
        }
    }
    r
}

unsafe extern "system" fn layer_FreeCommandBuffers(
    d: vk::Device,
    p: vk::CommandPool,
    n: u32,
    list: *const vk::CommandBuffer,
) {
    let dev = dev_of(d).unwrap();
    {
        let mut m = cbs().write().unwrap();
        for i in 0..n as usize {
            if let Some(c) = m.remove(&(*list.add(i)).as_raw()) {
                c.alive.store(false, Ordering::Relaxed);
            }
        }
    }
    (dev.f.free_command_buffers.unwrap())(d, p, n, list)
}

unsafe extern "system" fn layer_BeginCommandBuffer(cb: vk::CommandBuffer, bi: *const vk::CommandBufferBeginInfo) -> vk::Result {
    let Some(c) = cb_of(cb) else {
        return (dev_for_cb(cb).f.begin_command_buffer.unwrap())(cb, bi);
    };
    let dev = &c.dev;
    let r = (dev.f.begin_command_buffer.unwrap())(cb, bi);
    if r != vk::Result::SUCCESS {
        return r;
    }
    let mut st = c.st.lock().unwrap();
    st.recorded = None;
    let mut b = Build::default();
    let family = dev.objects.read().unwrap().cmd_pools.get(&c.pool).copied();
    let ts_ok = family
        .and_then(|f| dev.families.get(f as usize))
        .is_some_and(|(flags, bits)| *bits > 0 && flags.intersects(vk::QueueFlags::GRAPHICS | vk::QueueFlags::COMPUTE));
    if c.primary && ts_ok && dev.record_window() {
        let mut warned = warned_pool(dev);
        let was = warned;
        if let Some(q) = b.alloc_pair(dev, cb, &mut warned) {
            (dev.f.cmd_write_timestamp.unwrap())(cb, vk::PipelineStageFlags::TOP_OF_PIPE, dev.pool, q);
            b.cb_q = q;
            b.instrumented = true;
            b.id = synclog::REC_ID.fetch_add(1, Ordering::Relaxed);
        }
        if warned && !was {
            set_warned_pool(dev);
        }
    }
    st.build = Some(b);
    r
}

unsafe extern "system" fn layer_EndCommandBuffer(cb: vk::CommandBuffer) -> vk::Result {
    let Some(c) = cb_of(cb) else {
        return (dev_for_cb(cb).f.end_command_buffer.unwrap())(cb);
    };
    let dev = &c.dev;
    let mut st = c.st.lock().unwrap();
    if let Some(b) = st.build.as_ref() {
        if b.instrumented {
            timestamp_after_all(dev, cb, b.cb_q + 1);
        }
    }
    let r = (dev.f.end_command_buffer.unwrap())(cb);
    if let Some(mut b) = st.build.take() {
        st.sec_draws = b.draws_total;
        st.sec_pipe = b.pipe_fold;
        st.sec_npipe = b.npipe_total;
        if b.instrumented && r == vk::Result::SUCCESS {
            st.recorded = Some(Arc::new(Recorded {
                id: b.id,
                regions: std::mem::take(&mut b.regions),
                cb_q: b.cb_q,
                chunks: std::mem::take(&mut b.chunks),
                owner: dev.chunks.clone(),
                pending: AtomicU32::new(0),
                submissions: AtomicU32::new(0),
                skipped: b.skipped,
            }));
        } else if !b.chunks.is_empty() {
            dev.chunks.give_back(&mut b.chunks);
        }
    }
    r
}

unsafe extern "system" fn layer_ResetCommandBuffer(cb: vk::CommandBuffer, fl: vk::CommandBufferResetFlags) -> vk::Result {
    let dev = dev_for_cb(cb);
    if let Some(c) = cb_of(cb) {
        let mut st = c.st.lock().unwrap();
        if let Some(mut b) = st.build.take() {
            dev.chunks.give_back(&mut b.chunks);
        }
        st.recorded = None;
    }
    (dev.f.reset_command_buffer.unwrap())(cb, fl)
}

// ---------------------------------------------------------------------------------------------------------
// Recorded commands.

unsafe extern "system" fn layer_CmdBeginRenderPass(cb: vk::CommandBuffer, bi: *const vk::RenderPassBeginInfo, sc: vk::SubpassContents) {
    if let Some(c) = cb_of(cb) {
        let fmt = c.dev.objects.read().unwrap().render_passes.get(&(*bi).render_pass.as_raw()).cloned().unwrap_or_else(|| Arc::from("?"));
        let e = (*bi).render_area.extent;
        synclog::note_rpbegin(&c, bi);
        begin_region(&c, cb, Kind::RenderPass, fmt, format!("{}x{}", e.width, e.height), 0);
        (c.dev.f.cmd_begin_render_pass.unwrap())(cb, bi, sc)
    } else {
        (dev_for_cb(cb).f.cmd_begin_render_pass.unwrap())(cb, bi, sc)
    }
}

unsafe extern "system" fn layer_CmdBeginRenderPass2(
    cb: vk::CommandBuffer,
    bi: *const vk::RenderPassBeginInfo,
    si: *const vk::SubpassBeginInfo,
) {
    if let Some(c) = cb_of(cb) {
        let fmt = c.dev.objects.read().unwrap().render_passes.get(&(*bi).render_pass.as_raw()).cloned().unwrap_or_else(|| Arc::from("?"));
        let e = (*bi).render_area.extent;
        synclog::note_rpbegin(&c, bi);
        begin_region(&c, cb, Kind::RenderPass, fmt, format!("{}x{}", e.width, e.height), 0);
        (c.dev.f.cmd_begin_render_pass2.unwrap())(cb, bi, si)
    } else {
        (dev_for_cb(cb).f.cmd_begin_render_pass2.unwrap())(cb, bi, si)
    }
}

unsafe extern "system" fn layer_CmdEndRenderPass(cb: vk::CommandBuffer) {
    let dev = dev_for_cb(cb);
    (dev.f.cmd_end_render_pass.unwrap())(cb);
    if let Some(c) = cb_of(cb) {
        end_region(&c, cb);
    }
}

unsafe extern "system" fn layer_CmdEndRenderPass2(cb: vk::CommandBuffer, ei: *const vk::SubpassEndInfo) {
    let dev = dev_for_cb(cb);
    (dev.f.cmd_end_render_pass2.unwrap())(cb, ei);
    if let Some(c) = cb_of(cb) {
        end_region(&c, cb);
    }
}

unsafe extern "system" fn layer_CmdBeginRendering(cb: vk::CommandBuffer, ri: *const vk::RenderingInfo) {
    let Some(c) = cb_of(cb) else {
        return (dev_for_cb(cb).f.cmd_begin_rendering.unwrap())(cb, ri);
    };
    let info = &*ri;
    // A resumed or suspending instance spans command buffers; timestamps may not be placed between its
    // parts, so it is not measured (counted as skipped).
    if info.flags.intersects(vk::RenderingFlags::RESUMING | vk::RenderingFlags::SUSPENDING) {
        if let Some(b) = c.st.lock().unwrap().build.as_mut() {
            if b.instrumented {
                b.skipped += 1;
            }
        }
        return (c.dev.f.cmd_begin_rendering.unwrap())(cb, ri);
    }
    let fmt = {
        let o = c.dev.objects.read().unwrap();
        let view = |a: *const vk::RenderingAttachmentInfo| -> Option<i32> {
            if a.is_null() || (*a).image_view == vk::ImageView::null() {
                None
            } else {
                Some(o.views.get(&(*a).image_view.as_raw()).copied().unwrap_or(-1))
            }
        };
        let mut list: Vec<i32> = (0..info.color_attachment_count as usize)
            .map(|k| view(info.p_color_attachments.add(k)).unwrap_or(0))
            .collect();
        if let Some(f) = view(info.p_depth_attachment) {
            list.push(f);
        }
        if let Some(f) = view(info.p_stencil_attachment) {
            if Some(f) != view(info.p_depth_attachment) {
                list.push(f);
            }
        }
        fmt_list(list.into_iter())
    };
    let e = info.render_area.extent;
    begin_region(&c, cb, Kind::Rendering, fmt, format!("{}x{}", e.width, e.height), 0);
    (c.dev.f.cmd_begin_rendering.unwrap())(cb, ri)
}

unsafe extern "system" fn layer_CmdEndRendering(cb: vk::CommandBuffer) {
    let dev = dev_for_cb(cb);
    (dev.f.cmd_end_rendering.unwrap())(cb);
    if let Some(c) = cb_of(cb) {
        end_region(&c, cb);
    }
}

unsafe extern "system" fn layer_CmdBindPipeline(cb: vk::CommandBuffer, bp: vk::PipelineBindPoint, p: vk::Pipeline) {
    let dev = dev_for_cb(cb);
    if let Some(c) = cb_of(cb) {
        let h = dev.objects.read().unwrap().pipes.get(&p.as_raw()).copied().unwrap_or(0);
        if let Some(b) = c.st.lock().unwrap().build.as_mut() {
            match bp {
                vk::PipelineBindPoint::GRAPHICS => b.gfx_pipe = h,
                vk::PipelineBindPoint::COMPUTE => b.cs_pipe = h,
                _ => {}
            }
        }
    }
    (dev.f.cmd_bind_pipeline.unwrap())(cb, bp, p)
}

fn count_draw(cb: vk::CommandBuffer) -> Arc<Dev> {
    match cb_of(cb) {
        Some(c) => {
            if let Some(b) = c.st.lock().unwrap().build.as_mut() {
                b.count_draw();
            }
            c.dev.clone()
        }
        None => dev_for_cb(cb),
    }
}

unsafe extern "system" fn layer_CmdDraw(cb: vk::CommandBuffer, vc: u32, ic: u32, fv: u32, fi: u32) {
    (count_draw(cb).f.cmd_draw.unwrap())(cb, vc, ic, fv, fi)
}
unsafe extern "system" fn layer_CmdDrawIndexed(cb: vk::CommandBuffer, ic: u32, inst: u32, fi: u32, vo: i32, fin: u32) {
    (count_draw(cb).f.cmd_draw_indexed.unwrap())(cb, ic, inst, fi, vo, fin)
}
unsafe extern "system" fn layer_CmdDrawIndirect(cb: vk::CommandBuffer, b: vk::Buffer, o: vk::DeviceSize, n: u32, s: u32) {
    (count_draw(cb).f.cmd_draw_indirect.unwrap())(cb, b, o, n, s)
}
unsafe extern "system" fn layer_CmdDrawIndexedIndirect(cb: vk::CommandBuffer, b: vk::Buffer, o: vk::DeviceSize, n: u32, s: u32) {
    (count_draw(cb).f.cmd_draw_indexed_indirect.unwrap())(cb, b, o, n, s)
}
unsafe extern "system" fn layer_CmdDrawIndirectCount(
    cb: vk::CommandBuffer,
    b: vk::Buffer,
    o: vk::DeviceSize,
    cbuf: vk::Buffer,
    co: vk::DeviceSize,
    m: u32,
    s: u32,
) {
    (count_draw(cb).f.cmd_draw_indirect_count.unwrap())(cb, b, o, cbuf, co, m, s)
}
unsafe extern "system" fn layer_CmdDrawIndexedIndirectCount(
    cb: vk::CommandBuffer,
    b: vk::Buffer,
    o: vk::DeviceSize,
    cbuf: vk::Buffer,
    co: vk::DeviceSize,
    m: u32,
    s: u32,
) {
    (count_draw(cb).f.cmd_draw_indexed_indirect_count.unwrap())(cb, b, o, cbuf, co, m, s)
}

unsafe fn dispatch_region(cb: vk::CommandBuffer, kind: Kind, extent: String, call: impl FnOnce(&Dev)) {
    match cb_of(cb) {
        Some(c) if config().dispatch_regions => {
            let pipe = c.st.lock().unwrap().build.as_ref().map(|b| b.cs_pipe).unwrap_or(0);
            begin_region(&c, cb, kind, Arc::from(""), extent, pipe);
            call(&c.dev);
            end_region(&c, cb);
        }
        Some(c) => call(&c.dev),
        None => call(&dev_for_cb(cb)),
    }
}

unsafe extern "system" fn layer_CmdDispatch(cb: vk::CommandBuffer, x: u32, y: u32, z: u32) {
    dispatch_region(cb, Kind::Dispatch, format!("{x}*{y}*{z}"), |d| (d.f.cmd_dispatch.unwrap())(cb, x, y, z))
}
unsafe extern "system" fn layer_CmdDispatchBase(cb: vk::CommandBuffer, bx: u32, by: u32, bz: u32, x: u32, y: u32, z: u32) {
    dispatch_region(cb, Kind::Dispatch, format!("{x}*{y}*{z}"), |d| {
        (d.f.cmd_dispatch_base.unwrap())(cb, bx, by, bz, x, y, z)
    })
}
unsafe extern "system" fn layer_CmdDispatchIndirect(cb: vk::CommandBuffer, b: vk::Buffer, o: vk::DeviceSize) {
    dispatch_region(cb, Kind::DispatchIndirect, "ind".into(), |d| (d.f.cmd_dispatch_indirect.unwrap())(cb, b, o))
}

unsafe extern "system" fn layer_CmdExecuteCommands(cb: vk::CommandBuffer, n: u32, list: *const vk::CommandBuffer) {
    let dev = dev_for_cb(cb);
    if let Some(c) = cb_of(cb) {
        let mut add = Vec::new();
        for i in 0..n as usize {
            if let Some(s) = cb_of(*list.add(i)) {
                let st = s.st.lock().unwrap();
                add.push((st.sec_draws, st.sec_pipe, st.sec_npipe));
            }
        }
        if let Some(b) = c.st.lock().unwrap().build.as_mut() {
            if let Some(i) = b.open {
                let r = &mut b.regions[i];
                for (dr, p, np) in add {
                    r.draws += dr;
                    r.pipe = fold(r.pipe, p);
                    r.npipe += np;
                }
            }
        }
    }
    (dev.f.cmd_execute_commands.unwrap())(cb, n, list)
}

// ---------------------------------------------------------------------------------------------------------
// Submission and present.

unsafe fn note_submit(dev: &Arc<Dev>, list: Vec<vk::CommandBuffer>) {
    if !dev.measuring {
        return;
    }
    let mut recs: Vec<Option<Arc<Recorded>>> = Vec::with_capacity(list.len());
    for cb in &list {
        recs.push(cb_of(*cb).and_then(|c| c.st.lock().unwrap().recorded.clone()));
    }
    let mut guard = dev.harvest.lock().unwrap();
    let h: &mut Harvest = &mut guard;
    if h.done {
        return;
    }
    // A recording submitted again: its previous execution must be complete and read before its queries
    // are reset for this one.
    let mut waits = 0;
    for r in recs.iter().flatten() {
        if r.submissions.fetch_add(1, Ordering::AcqRel) > 0 {
            if r.pending.load(Ordering::Acquire) > 0 {
                harvest(dev, h, Some(r));
            }
            let _ = read_pair(dev, r.cb_q, true);
            r.owner.reset(&r.chunks);
            waits += 1;
        }
    }
    let frame = dev.frame.load(Ordering::Relaxed);
    if dev.in_window(frame) {
        let fa = h.frames.entry(frame).or_default();
        fa.waits += waits;
        let mut items = Vec::new();
        for r in recs {
            match r {
                Some(r) => {
                    r.pending.fetch_add(1, Ordering::AcqRel);
                    items.push((r.clone(), fa.seq, fa.cbs + fa.unmeasured_cbs));
                    fa.seq += r.regions.len() as u32;
                    fa.skipped += r.skipped;
                    fa.cbs += 1;
                }
                None => fa.unmeasured_cbs += 1,
            }
        }
        if !items.is_empty() {
            fa.outstanding += 1;
            h.pending.push_back(Pending { frame, items });
        }
    }
    if config().per_submit {
        dev.frame.fetch_add(1, Ordering::Relaxed);
    }
}

unsafe fn after_submit(dev: &Dev) {
    if !dev.measuring {
        return;
    }
    let mut h = dev.harvest.lock().unwrap();
    if !h.done {
        harvest(dev, &mut h, None);
    }
}

unsafe extern "system" fn layer_QueueSubmit(q: vk::Queue, n: u32, si: *const vk::SubmitInfo, fence: vk::Fence) -> vk::Result {
    let dev = dev_of(q).unwrap();
    let mut list = Vec::new();
    for i in 0..n as usize {
        let s = &*si.add(i);
        let first = list.len();
        for k in 0..s.command_buffer_count as usize {
            list.push(*s.p_command_buffers.add(k));
        }
        synclog::note_queue_submit(&dev, q, &list[first..], s.wait_semaphore_count, s.signal_semaphore_count);
    }
    note_submit(&dev, list);
    let r = (dev.f.queue_submit.unwrap())(q, n, si, fence);
    after_submit(&dev);
    r
}

unsafe extern "system" fn layer_QueueSubmit2(q: vk::Queue, n: u32, si: *const vk::SubmitInfo2, fence: vk::Fence) -> vk::Result {
    let dev = dev_of(q).unwrap();
    let mut list = Vec::new();
    for i in 0..n as usize {
        let s = &*si.add(i);
        let first = list.len();
        for k in 0..s.command_buffer_info_count as usize {
            list.push((*s.p_command_buffer_infos.add(k)).command_buffer);
        }
        synclog::note_queue_submit(&dev, q, &list[first..], s.wait_semaphore_info_count, s.signal_semaphore_info_count);
    }
    note_submit(&dev, list);
    let r = (dev.f.queue_submit2.unwrap())(q, n, si, fence);
    after_submit(&dev);
    r
}

unsafe extern "system" fn layer_QueuePresentKHR(q: vk::Queue, pi: *const vk::PresentInfoKHR) -> vk::Result {
    let dev = dev_of(q).unwrap();
    if dev.measuring && !config().per_submit {
        let mut h = dev.harvest.lock().unwrap();
        dev.frame.fetch_add(1, Ordering::Relaxed);
        if !h.done {
            harvest(&dev, &mut h, None);
        }
    }
    (dev.f.queue_present.unwrap())(q, pi)
}

// ---------------------------------------------------------------------------------------------------------
// Proc-address resolution and exported layer entry points.

type Void = unsafe extern "system" fn();

macro_rules! hook {
    ($f:ident : $pfn:ident) => {
        Some(std::mem::transmute::<vk::$pfn, Void>($f as vk::$pfn))
    };
}

unsafe fn device_hook(name: &[u8]) -> Option<Void> {
    use synclog::*;
    match name {
        b"vkGetDeviceProcAddr" => hook!(vkGetDeviceProcAddr: PFN_vkGetDeviceProcAddr),
        b"vkDestroyDevice" => hook!(layer_DestroyDevice: PFN_vkDestroyDevice),
        b"vkQueueSubmit" => hook!(layer_QueueSubmit: PFN_vkQueueSubmit),
        b"vkQueueSubmit2" | b"vkQueueSubmit2KHR" => hook!(layer_QueueSubmit2: PFN_vkQueueSubmit2),
        b"vkQueuePresentKHR" => hook!(layer_QueuePresentKHR: PFN_vkQueuePresentKHR),
        b"vkCreateCommandPool" => hook!(layer_CreateCommandPool: PFN_vkCreateCommandPool),
        b"vkDestroyCommandPool" => hook!(layer_DestroyCommandPool: PFN_vkDestroyCommandPool),
        b"vkResetCommandPool" => hook!(layer_ResetCommandPool: PFN_vkResetCommandPool),
        b"vkAllocateCommandBuffers" => hook!(layer_AllocateCommandBuffers: PFN_vkAllocateCommandBuffers),
        b"vkFreeCommandBuffers" => hook!(layer_FreeCommandBuffers: PFN_vkFreeCommandBuffers),
        b"vkBeginCommandBuffer" => hook!(layer_BeginCommandBuffer: PFN_vkBeginCommandBuffer),
        b"vkEndCommandBuffer" => hook!(layer_EndCommandBuffer: PFN_vkEndCommandBuffer),
        b"vkResetCommandBuffer" => hook!(layer_ResetCommandBuffer: PFN_vkResetCommandBuffer),
        b"vkCreateShaderModule" => hook!(layer_CreateShaderModule: PFN_vkCreateShaderModule),
        b"vkDestroyShaderModule" => hook!(layer_DestroyShaderModule: PFN_vkDestroyShaderModule),
        b"vkCreateGraphicsPipelines" => hook!(layer_CreateGraphicsPipelines: PFN_vkCreateGraphicsPipelines),
        b"vkCreateComputePipelines" => hook!(layer_CreateComputePipelines: PFN_vkCreateComputePipelines),
        b"vkDestroyPipeline" => hook!(layer_DestroyPipeline: PFN_vkDestroyPipeline),
        b"vkCreateRenderPass" => hook!(layer_CreateRenderPass: PFN_vkCreateRenderPass),
        b"vkCreateRenderPass2" | b"vkCreateRenderPass2KHR" => hook!(layer_CreateRenderPass2: PFN_vkCreateRenderPass2),
        b"vkDestroyRenderPass" => hook!(layer_DestroyRenderPass: PFN_vkDestroyRenderPass),
        b"vkCreateImageView" => hook!(layer_CreateImageView: PFN_vkCreateImageView),
        b"vkDestroyImageView" => hook!(layer_DestroyImageView: PFN_vkDestroyImageView),
        b"vkCmdBeginRenderPass" => hook!(layer_CmdBeginRenderPass: PFN_vkCmdBeginRenderPass),
        b"vkCmdBeginRenderPass2" | b"vkCmdBeginRenderPass2KHR" => hook!(layer_CmdBeginRenderPass2: PFN_vkCmdBeginRenderPass2),
        b"vkCmdEndRenderPass" => hook!(layer_CmdEndRenderPass: PFN_vkCmdEndRenderPass),
        b"vkCmdEndRenderPass2" | b"vkCmdEndRenderPass2KHR" => hook!(layer_CmdEndRenderPass2: PFN_vkCmdEndRenderPass2),
        b"vkCmdBeginRendering" | b"vkCmdBeginRenderingKHR" => hook!(layer_CmdBeginRendering: PFN_vkCmdBeginRendering),
        b"vkCmdEndRendering" | b"vkCmdEndRenderingKHR" => hook!(layer_CmdEndRendering: PFN_vkCmdEndRendering),
        b"vkCmdBindPipeline" => hook!(layer_CmdBindPipeline: PFN_vkCmdBindPipeline),
        b"vkCmdDraw" => hook!(layer_CmdDraw: PFN_vkCmdDraw),
        b"vkCmdDrawIndexed" => hook!(layer_CmdDrawIndexed: PFN_vkCmdDrawIndexed),
        b"vkCmdDrawIndirect" => hook!(layer_CmdDrawIndirect: PFN_vkCmdDrawIndirect),
        b"vkCmdDrawIndexedIndirect" => hook!(layer_CmdDrawIndexedIndirect: PFN_vkCmdDrawIndexedIndirect),
        b"vkCmdDrawIndirectCount" | b"vkCmdDrawIndirectCountKHR" => {
            hook!(layer_CmdDrawIndirectCount: PFN_vkCmdDrawIndirectCount)
        }
        b"vkCmdDrawIndexedIndirectCount" | b"vkCmdDrawIndexedIndirectCountKHR" => {
            hook!(layer_CmdDrawIndexedIndirectCount: PFN_vkCmdDrawIndexedIndirectCount)
        }
        b"vkCmdDispatch" => hook!(layer_CmdDispatch: PFN_vkCmdDispatch),
        b"vkCmdDispatchBase" | b"vkCmdDispatchBaseKHR" => hook!(layer_CmdDispatchBase: PFN_vkCmdDispatchBase),
        b"vkCmdDispatchIndirect" => hook!(layer_CmdDispatchIndirect: PFN_vkCmdDispatchIndirect),
        b"vkCmdExecuteCommands" => hook!(layer_CmdExecuteCommands: PFN_vkCmdExecuteCommands),
        b"vkCmdPipelineBarrier" => hook!(layer_CmdPipelineBarrier: PFN_vkCmdPipelineBarrier),
        b"vkCmdPipelineBarrier2" | b"vkCmdPipelineBarrier2KHR" => hook!(layer_CmdPipelineBarrier2: PFN_vkCmdPipelineBarrier2),
        b"vkCmdWaitEvents" => hook!(layer_CmdWaitEvents: PFN_vkCmdWaitEvents),
        b"vkCmdWaitEvents2" | b"vkCmdWaitEvents2KHR" => hook!(layer_CmdWaitEvents2: PFN_vkCmdWaitEvents2),
        b"vkCmdSetEvent" => hook!(layer_CmdSetEvent: PFN_vkCmdSetEvent),
        b"vkCmdSetEvent2" | b"vkCmdSetEvent2KHR" => hook!(layer_CmdSetEvent2: PFN_vkCmdSetEvent2),
        b"vkCmdCopyBuffer" => hook!(layer_CmdCopyBuffer: PFN_vkCmdCopyBuffer),
        b"vkCmdCopyImage" => hook!(layer_CmdCopyImage: PFN_vkCmdCopyImage),
        b"vkCmdBlitImage" => hook!(layer_CmdBlitImage: PFN_vkCmdBlitImage),
        b"vkCmdResolveImage" => hook!(layer_CmdResolveImage: PFN_vkCmdResolveImage),
        b"vkCmdCopyBufferToImage" => hook!(layer_CmdCopyBufferToImage: PFN_vkCmdCopyBufferToImage),
        b"vkCmdCopyImageToBuffer" => hook!(layer_CmdCopyImageToBuffer: PFN_vkCmdCopyImageToBuffer),
        b"vkCmdFillBuffer" => hook!(layer_CmdFillBuffer: PFN_vkCmdFillBuffer),
        b"vkCmdUpdateBuffer" => hook!(layer_CmdUpdateBuffer: PFN_vkCmdUpdateBuffer),
        b"vkCmdClearColorImage" => hook!(layer_CmdClearColorImage: PFN_vkCmdClearColorImage),
        b"vkCmdClearDepthStencilImage" => hook!(layer_CmdClearDepthStencilImage: PFN_vkCmdClearDepthStencilImage),
        b"vkCmdClearAttachments" => hook!(layer_CmdClearAttachments: PFN_vkCmdClearAttachments),
        b"vkGetDeviceQueue" => hook!(layer_GetDeviceQueue: PFN_vkGetDeviceQueue),
        b"vkGetDeviceQueue2" => hook!(layer_GetDeviceQueue2: PFN_vkGetDeviceQueue2),
        b"vkCreateImage" => hook!(layer_CreateImage: PFN_vkCreateImage),
        b"vkAllocateMemory" => hook!(layer_AllocateMemory: PFN_vkAllocateMemory),
        b"vkBindImageMemory" => hook!(layer_BindImageMemory: PFN_vkBindImageMemory),
        b"vkBindImageMemory2" | b"vkBindImageMemory2KHR" => hook!(layer_BindImageMemory2: PFN_vkBindImageMemory2),
        _ => None,
    }
}

#[no_mangle]
pub unsafe extern "system" fn vkGetDeviceProcAddr(d: vk::Device, name: *const c_char) -> vk::PFN_vkVoidFunction {
    let Some(dev) = dev_of(d) else { return None };
    let next = (dev.gdpa)(d, name);
    next?;
    device_hook(CStr::from_ptr(name).to_bytes()).or(next)
}

unsafe fn instance_hook(name: &[u8]) -> Option<Void> {
    match name {
        b"vkGetInstanceProcAddr" => hook!(vkGetInstanceProcAddr: PFN_vkGetInstanceProcAddr),
        b"vkCreateInstance" => hook!(layer_CreateInstance: PFN_vkCreateInstance),
        b"vkDestroyInstance" => hook!(layer_DestroyInstance: PFN_vkDestroyInstance),
        b"vkCreateDevice" => hook!(layer_CreateDevice: PFN_vkCreateDevice),
        b"vkEnumerateInstanceLayerProperties" => {
            hook!(vkEnumerateInstanceLayerProperties: PFN_vkEnumerateInstanceLayerProperties)
        }
        b"vkEnumerateInstanceExtensionProperties" => {
            hook!(vkEnumerateInstanceExtensionProperties: PFN_vkEnumerateInstanceExtensionProperties)
        }
        b"vkEnumerateDeviceLayerProperties" => hook!(vkEnumerateDeviceLayerProperties: PFN_vkEnumerateDeviceLayerProperties),
        _ => None,
    }
}

#[no_mangle]
pub unsafe extern "system" fn vkGetInstanceProcAddr(inst: vk::Instance, name: *const c_char) -> vk::PFN_vkVoidFunction {
    let n = CStr::from_ptr(name).to_bytes();
    if let Some(h) = instance_hook(n) {
        return Some(h);
    }
    let i = inst_of(inst)?;
    let next = (i.gipa)(inst, name);
    next?;
    device_hook(n).or(next)
}

fn props() -> vk::LayerProperties {
    let mut p = vk::LayerProperties { spec_version: vk::make_api_version(0, 1, 3, 0), implementation_version: 2, ..Default::default() };
    for (d, s) in p.layer_name.iter_mut().zip(LAYER_NAME) {
        *d = *s as c_char;
    }
    for (d, s) in p.description.iter_mut().zip(LAYER_DESC) {
        *d = *s as c_char;
    }
    p
}

#[no_mangle]
pub unsafe extern "system" fn vkEnumerateInstanceLayerProperties(n: *mut u32, p: *mut vk::LayerProperties) -> vk::Result {
    if p.is_null() {
        *n = 1;
        return vk::Result::SUCCESS;
    }
    if *n < 1 {
        return vk::Result::INCOMPLETE;
    }
    *p = props();
    *n = 1;
    vk::Result::SUCCESS
}

#[no_mangle]
pub unsafe extern "system" fn vkEnumerateDeviceLayerProperties(
    _pd: vk::PhysicalDevice,
    n: *mut u32,
    p: *mut vk::LayerProperties,
) -> vk::Result {
    vkEnumerateInstanceLayerProperties(n, p)
}

fn is_us(l: *const c_char) -> bool {
    !l.is_null() && unsafe { CStr::from_ptr(l) }.to_bytes_with_nul() == LAYER_NAME
}

#[no_mangle]
pub unsafe extern "system" fn vkEnumerateInstanceExtensionProperties(
    l: *const c_char,
    n: *mut u32,
    _p: *mut vk::ExtensionProperties,
) -> vk::Result {
    if is_us(l) {
        *n = 0;
        return vk::Result::SUCCESS;
    }
    vk::Result::ERROR_LAYER_NOT_PRESENT
}

#[no_mangle]
pub unsafe extern "system" fn vkEnumerateDeviceExtensionProperties(
    pd: vk::PhysicalDevice,
    l: *const c_char,
    n: *mut u32,
    p: *mut vk::ExtensionProperties,
) -> vk::Result {
    if is_us(l) {
        *n = 0;
        return vk::Result::SUCCESS;
    }
    match inst_of(pd).and_then(|i| i.enum_dev_ext) {
        Some(f) => f(pd, l, n, p),
        None => vk::Result::ERROR_LAYER_NOT_PRESENT,
    }
}
