//! Synchronisation and resource log (`<prefix>-<pid>-d<N>-sync.csv`): what the application asks the driver to
//! order and where, so the time between and around the measured regions can be explained per driver.
//!
//! Rows (`ev,frame,rec,ridx,inrp,detail...`; `frame` is the device's present count at recording time, `rec` the
//! recording id that the main file's `cb` row carries as `rec=`, `ridx` the number of regions of that recording
//! begun before the command, so it sits between regions `ridx-1` and `ridx` of the recording; `inrp` 1 inside a
//! render pass instance):
//! * creation (outside any recording, frame/rec/ridx/inrp empty): `family` (index, flags, count), `qcreate`
//!   (family, count the application requested), `mtype` (index, property flags, heap), `queue` (handle, family,
//!   index), `img` (handle, format, extent, mips, layers, samples, tiling, usage, flags), `mem` (handle, size,
//!   type, dedicated image, export handle types), `bind` (image, memory, offset), `rpdef` (render pass:
//!   attachments `format/samples/load/store/stencil load/stencil store/initial/final layout`, then subpass
//!   dependencies `src>dst:src stages/dst stages/src access/dst access/flags`);
//! * recorded inside the frame window: `bar1` / `bar2` (one row per memory / buffer / image barrier, or one `x`
//!   row for an execution-only barrier: struct kind, src stages, src access, dst stages, dst access, old layout,
//!   new layout, object handle, src/dst queue family, aspect and subresource range), `wait1` / `wait2` /
//!   `set1` / `set2` (events, same fields), `rpbegin` (render pass, framebuffer, clear value count), `xfer`
//!   (copy/clear/blit/resolve/fill/update command: name, source, destination, region count);
//! * `sub` per submission inside the frame window: queue handle, family, index, the recording ids of its
//!   command buffers (0 = not measured), wait and signal semaphore counts.

use super::*;

pub(crate) static REC_ID: AtomicU64 = AtomicU64::new(1);

pub(crate) fn write(dev: &Dev, line: &str) {
    if !dev.measuring {
        return;
    }
    if let Ok(mut h) = dev.harvest.lock() {
        if let Some(o) = h.out.as_mut() {
            let _ = o.sync.write_all(line.as_bytes());
            let _ = o.sync.write_all(b"\n");
        }
    }
}

/// Position of a command being recorded: `frame,rec,ridx,inrp`, or None when the recording is not measured.
fn pos(c: &Cb) -> Option<String> {
    let st = c.st.lock().unwrap();
    let b = st.build.as_ref()?;
    if !b.instrumented {
        return None;
    }
    Some(format!(
        "{},{},{},{}",
        c.dev.frame.load(Ordering::Relaxed),
        b.id,
        b.regions.len(),
        u32::from(b.open.is_some_and(|i| b.regions[i].kind == Kind::RenderPass || b.regions[i].kind == Kind::Rendering))
    ))
}

fn range(r: &vk::ImageSubresourceRange) -> String {
    format!("a{:x}:m{}+{}:l{}+{}", r.aspect_mask.as_raw(), r.base_mip_level, r.level_count, r.base_array_layer, r.layer_count)
}

/// Rows of a synchronisation-1 command (barrier, wait events); `stages` = (src, dst).
#[allow(clippy::too_many_arguments)]
unsafe fn sync1_rows(
    c: &Cb,
    ev: &str,
    src: vk::PipelineStageFlags,
    dst: vk::PipelineStageFlags,
    flags: u32,
    nm: u32,
    pm: *const vk::MemoryBarrier,
    nb: u32,
    pb: *const vk::BufferMemoryBarrier,
    ni: u32,
    pi: *const vk::ImageMemoryBarrier,
) {
    let Some(p) = pos(c) else { return };
    let (s, d) = (src.as_raw(), dst.as_raw());
    let mut rows = Vec::new();
    for k in 0..nm as usize {
        let m = &*pm.add(k);
        rows.push(format!("{ev},{p},m,{s:x},{:x},{d:x},{:x},,,,,,f{flags:x}", m.src_access_mask.as_raw(), m.dst_access_mask.as_raw()));
    }
    for k in 0..nb as usize {
        let m = &*pb.add(k);
        rows.push(format!(
            "{ev},{p},b,{s:x},{:x},{d:x},{:x},,,{:x},{}>{},o{}+{},f{flags:x}",
            m.src_access_mask.as_raw(),
            m.dst_access_mask.as_raw(),
            m.buffer.as_raw(),
            m.src_queue_family_index as i32,
            m.dst_queue_family_index as i32,
            m.offset,
            m.size as i64
        ));
    }
    for k in 0..ni as usize {
        let m = &*pi.add(k);
        rows.push(format!(
            "{ev},{p},i,{s:x},{:x},{d:x},{:x},{},{},{:x},{}>{},{},f{flags:x}",
            m.src_access_mask.as_raw(),
            m.dst_access_mask.as_raw(),
            m.old_layout.as_raw(),
            m.new_layout.as_raw(),
            m.image.as_raw(),
            m.src_queue_family_index as i32,
            m.dst_queue_family_index as i32,
            range(&m.subresource_range)
        ));
    }
    if rows.is_empty() {
        rows.push(format!("{ev},{p},x,{s:x},0,{d:x},0,,,,,,f{flags:x}"));
    }
    for r in rows {
        write(&c.dev, &r);
    }
}

unsafe fn dep_rows(c: &Cb, ev: &str, di: *const vk::DependencyInfo) {
    let Some(p) = pos(c) else { return };
    let di = &*di;
    let flags = di.dependency_flags.as_raw();
    let mut rows = Vec::new();
    for k in 0..di.memory_barrier_count as usize {
        let m = &*di.p_memory_barriers.add(k);
        rows.push(format!(
            "{ev},{p},m,{:x},{:x},{:x},{:x},,,,,,f{flags:x}",
            m.src_stage_mask.as_raw(),
            m.src_access_mask.as_raw(),
            m.dst_stage_mask.as_raw(),
            m.dst_access_mask.as_raw()
        ));
    }
    for k in 0..di.buffer_memory_barrier_count as usize {
        let m = &*di.p_buffer_memory_barriers.add(k);
        rows.push(format!(
            "{ev},{p},b,{:x},{:x},{:x},{:x},,,{:x},{}>{},o{}+{},f{flags:x}",
            m.src_stage_mask.as_raw(),
            m.src_access_mask.as_raw(),
            m.dst_stage_mask.as_raw(),
            m.dst_access_mask.as_raw(),
            m.buffer.as_raw(),
            m.src_queue_family_index as i32,
            m.dst_queue_family_index as i32,
            m.offset,
            m.size as i64
        ));
    }
    for k in 0..di.image_memory_barrier_count as usize {
        let m = &*di.p_image_memory_barriers.add(k);
        rows.push(format!(
            "{ev},{p},i,{:x},{:x},{:x},{:x},{},{},{:x},{}>{},{},f{flags:x}",
            m.src_stage_mask.as_raw(),
            m.src_access_mask.as_raw(),
            m.dst_stage_mask.as_raw(),
            m.dst_access_mask.as_raw(),
            m.old_layout.as_raw(),
            m.new_layout.as_raw(),
            m.image.as_raw(),
            m.src_queue_family_index as i32,
            m.dst_queue_family_index as i32,
            range(&m.subresource_range)
        ));
    }
    if rows.is_empty() {
        rows.push(format!("{ev},{p},x,0,0,0,0,,,,,,f{flags:x}"));
    }
    for r in rows {
        write(&c.dev, &r);
    }
}

// --- recorded synchronisation ------------------------------------------------------------------------------

#[allow(clippy::too_many_arguments)]
pub(crate) unsafe extern "system" fn layer_CmdPipelineBarrier(
    cb: vk::CommandBuffer,
    src: vk::PipelineStageFlags,
    dst: vk::PipelineStageFlags,
    dep: vk::DependencyFlags,
    nm: u32,
    pm: *const vk::MemoryBarrier,
    nb: u32,
    pb: *const vk::BufferMemoryBarrier,
    ni: u32,
    pi: *const vk::ImageMemoryBarrier,
) {
    if let Some(c) = cb_of(cb) {
        sync1_rows(&c, "bar1", src, dst, dep.as_raw(), nm, pm, nb, pb, ni, pi);
    }
    (dev_for_cb(cb).f.cmd_pipeline_barrier.unwrap())(cb, src, dst, dep, nm, pm, nb, pb, ni, pi)
}

pub(crate) unsafe extern "system" fn layer_CmdPipelineBarrier2(cb: vk::CommandBuffer, di: *const vk::DependencyInfo) {
    if let Some(c) = cb_of(cb) {
        dep_rows(&c, "bar2", di);
    }
    (dev_for_cb(cb).f.cmd_pipeline_barrier2.unwrap())(cb, di)
}

#[allow(clippy::too_many_arguments)]
pub(crate) unsafe extern "system" fn layer_CmdWaitEvents(
    cb: vk::CommandBuffer,
    ne: u32,
    pe: *const vk::Event,
    src: vk::PipelineStageFlags,
    dst: vk::PipelineStageFlags,
    nm: u32,
    pm: *const vk::MemoryBarrier,
    nb: u32,
    pb: *const vk::BufferMemoryBarrier,
    ni: u32,
    pi: *const vk::ImageMemoryBarrier,
) {
    if let Some(c) = cb_of(cb) {
        sync1_rows(&c, "wait1", src, dst, ne, nm, pm, nb, pb, ni, pi);
    }
    (dev_for_cb(cb).f.cmd_wait_events.unwrap())(cb, ne, pe, src, dst, nm, pm, nb, pb, ni, pi)
}

pub(crate) unsafe extern "system" fn layer_CmdWaitEvents2(
    cb: vk::CommandBuffer,
    ne: u32,
    pe: *const vk::Event,
    di: *const vk::DependencyInfo,
) {
    if let Some(c) = cb_of(cb) {
        for k in 0..ne as usize {
            dep_rows(&c, "wait2", di.add(k));
        }
    }
    (dev_for_cb(cb).f.cmd_wait_events2.unwrap())(cb, ne, pe, di)
}

pub(crate) unsafe extern "system" fn layer_CmdSetEvent(cb: vk::CommandBuffer, e: vk::Event, st: vk::PipelineStageFlags) {
    if let Some(c) = cb_of(cb) {
        if let Some(p) = pos(&c) {
            write(&c.dev, &format!("set1,{p},x,{:x},0,0,0,,,{:x},,,", st.as_raw(), e.as_raw()));
        }
    }
    (dev_for_cb(cb).f.cmd_set_event.unwrap())(cb, e, st)
}

pub(crate) unsafe extern "system" fn layer_CmdSetEvent2(cb: vk::CommandBuffer, e: vk::Event, di: *const vk::DependencyInfo) {
    if let Some(c) = cb_of(cb) {
        dep_rows(&c, "set2", di);
    }
    (dev_for_cb(cb).f.cmd_set_event2.unwrap())(cb, e, di)
}

pub(crate) unsafe fn note_rpbegin(c: &Cb, bi: *const vk::RenderPassBeginInfo) {
    if let Some(p) = pos(c) {
        // the region was begun just before: report it as the region this begin opens
        let bi = &*bi;
        write(
            &c.dev,
            &format!("rpbegin,{p},{:x},{:x},{}", bi.render_pass.as_raw(), bi.framebuffer.as_raw(), bi.clear_value_count),
        );
    }
}

fn xfer(cb: vk::CommandBuffer, name: &str, src: u64, dst: u64, n: u32) {
    if let Some(c) = cb_of(cb) {
        if let Some(p) = pos(&c) {
            write(&c.dev, &format!("xfer,{p},{name},{src:x},{dst:x},{n}"));
        }
    }
}

pub(crate) unsafe extern "system" fn layer_CmdCopyBuffer(
    cb: vk::CommandBuffer,
    s: vk::Buffer,
    d: vk::Buffer,
    n: u32,
    r: *const vk::BufferCopy,
) {
    xfer(cb, "copy_buffer", s.as_raw(), d.as_raw(), n);
    (dev_for_cb(cb).f.cmd_copy_buffer.unwrap())(cb, s, d, n, r)
}
#[allow(clippy::too_many_arguments)]
pub(crate) unsafe extern "system" fn layer_CmdCopyImage(
    cb: vk::CommandBuffer,
    s: vk::Image,
    sl: vk::ImageLayout,
    d: vk::Image,
    dl: vk::ImageLayout,
    n: u32,
    r: *const vk::ImageCopy,
) {
    xfer(cb, "copy_image", s.as_raw(), d.as_raw(), n);
    (dev_for_cb(cb).f.cmd_copy_image.unwrap())(cb, s, sl, d, dl, n, r)
}
#[allow(clippy::too_many_arguments)]
pub(crate) unsafe extern "system" fn layer_CmdBlitImage(
    cb: vk::CommandBuffer,
    s: vk::Image,
    sl: vk::ImageLayout,
    d: vk::Image,
    dl: vk::ImageLayout,
    n: u32,
    r: *const vk::ImageBlit,
    f: vk::Filter,
) {
    xfer(cb, "blit_image", s.as_raw(), d.as_raw(), n);
    (dev_for_cb(cb).f.cmd_blit_image.unwrap())(cb, s, sl, d, dl, n, r, f)
}
#[allow(clippy::too_many_arguments)]
pub(crate) unsafe extern "system" fn layer_CmdResolveImage(
    cb: vk::CommandBuffer,
    s: vk::Image,
    sl: vk::ImageLayout,
    d: vk::Image,
    dl: vk::ImageLayout,
    n: u32,
    r: *const vk::ImageResolve,
) {
    xfer(cb, "resolve_image", s.as_raw(), d.as_raw(), n);
    (dev_for_cb(cb).f.cmd_resolve_image.unwrap())(cb, s, sl, d, dl, n, r)
}
pub(crate) unsafe extern "system" fn layer_CmdCopyBufferToImage(
    cb: vk::CommandBuffer,
    s: vk::Buffer,
    d: vk::Image,
    dl: vk::ImageLayout,
    n: u32,
    r: *const vk::BufferImageCopy,
) {
    xfer(cb, "copy_buffer_to_image", s.as_raw(), d.as_raw(), n);
    (dev_for_cb(cb).f.cmd_copy_buffer_to_image.unwrap())(cb, s, d, dl, n, r)
}
pub(crate) unsafe extern "system" fn layer_CmdCopyImageToBuffer(
    cb: vk::CommandBuffer,
    s: vk::Image,
    sl: vk::ImageLayout,
    d: vk::Buffer,
    n: u32,
    r: *const vk::BufferImageCopy,
) {
    xfer(cb, "copy_image_to_buffer", s.as_raw(), d.as_raw(), n);
    (dev_for_cb(cb).f.cmd_copy_image_to_buffer.unwrap())(cb, s, sl, d, n, r)
}
pub(crate) unsafe extern "system" fn layer_CmdFillBuffer(
    cb: vk::CommandBuffer,
    d: vk::Buffer,
    o: vk::DeviceSize,
    s: vk::DeviceSize,
    v: u32,
) {
    xfer(cb, "fill_buffer", 0, d.as_raw(), 1);
    (dev_for_cb(cb).f.cmd_fill_buffer.unwrap())(cb, d, o, s, v)
}
pub(crate) unsafe extern "system" fn layer_CmdUpdateBuffer(
    cb: vk::CommandBuffer,
    d: vk::Buffer,
    o: vk::DeviceSize,
    s: vk::DeviceSize,
    p: *const c_void,
) {
    xfer(cb, "update_buffer", 0, d.as_raw(), 1);
    (dev_for_cb(cb).f.cmd_update_buffer.unwrap())(cb, d, o, s, p)
}
pub(crate) unsafe extern "system" fn layer_CmdClearColorImage(
    cb: vk::CommandBuffer,
    i: vk::Image,
    l: vk::ImageLayout,
    v: *const vk::ClearColorValue,
    n: u32,
    r: *const vk::ImageSubresourceRange,
) {
    xfer(cb, "clear_color_image", 0, i.as_raw(), n);
    (dev_for_cb(cb).f.cmd_clear_color_image.unwrap())(cb, i, l, v, n, r)
}
pub(crate) unsafe extern "system" fn layer_CmdClearDepthStencilImage(
    cb: vk::CommandBuffer,
    i: vk::Image,
    l: vk::ImageLayout,
    v: *const vk::ClearDepthStencilValue,
    n: u32,
    r: *const vk::ImageSubresourceRange,
) {
    xfer(cb, "clear_depth_stencil_image", 0, i.as_raw(), n);
    (dev_for_cb(cb).f.cmd_clear_depth_stencil_image.unwrap())(cb, i, l, v, n, r)
}
pub(crate) unsafe extern "system" fn layer_CmdClearAttachments(
    cb: vk::CommandBuffer,
    na: u32,
    a: *const vk::ClearAttachment,
    nr: u32,
    r: *const vk::ClearRect,
) {
    xfer(cb, "clear_attachments", 0, 0, na);
    (dev_for_cb(cb).f.cmd_clear_attachments.unwrap())(cb, na, a, nr, r)
}

// --- submissions and queues --------------------------------------------------------------------------------

pub(crate) unsafe fn note_queue_submit(dev: &Dev, q: vk::Queue, list: &[vk::CommandBuffer], waits: u32, signals: u32) {
    if !dev.in_window(dev.frame.load(Ordering::Relaxed)) {
        return;
    }
    let (fam, idx) = dev.objects.read().unwrap().queues.get(&q.as_raw()).copied().unwrap_or((u32::MAX, u32::MAX));
    let mut ids = String::new();
    for cb in list {
        let id = cb_of(*cb).and_then(|c| c.st.lock().unwrap().recorded.as_ref().map(|r| r.id)).unwrap_or(0);
        let _ = write!(ids, "{}{id}", if ids.is_empty() { "" } else { " " });
    }
    write(
        dev,
        &format!("sub,{},,,,{:x},{},{},{ids},{waits},{signals}", dev.frame.load(Ordering::Relaxed), q.as_raw(), fam as i32, idx as i32),
    );
}

pub(crate) unsafe extern "system" fn layer_GetDeviceQueue(d: vk::Device, fam: u32, idx: u32, out: *mut vk::Queue) {
    let dev = dev_of(d).unwrap();
    (dev.f.get_device_queue.unwrap())(d, fam, idx, out);
    dev.objects.write().unwrap().queues.insert((*out).as_raw(), (fam, idx));
    write(&dev, &format!("queue,,,,,{:x},{fam},{idx}", (*out).as_raw()));
}

pub(crate) unsafe extern "system" fn layer_GetDeviceQueue2(d: vk::Device, qi: *const vk::DeviceQueueInfo2, out: *mut vk::Queue) {
    let dev = dev_of(d).unwrap();
    (dev.f.get_device_queue2.unwrap())(d, qi, out);
    let (fam, idx) = ((*qi).queue_family_index, (*qi).queue_index);
    dev.objects.write().unwrap().queues.insert((*out).as_raw(), (fam, idx));
    write(&dev, &format!("queue,,,,,{:x},{fam},{idx},f{:x}", (*out).as_raw(), (*qi).flags.as_raw()));
}

// --- resources ---------------------------------------------------------------------------------------------

pub(crate) unsafe extern "system" fn layer_CreateImage(
    d: vk::Device,
    ci: *const vk::ImageCreateInfo,
    a: *const vk::AllocationCallbacks,
    out: *mut vk::Image,
) -> vk::Result {
    let dev = dev_of(d).unwrap();
    let r = (dev.f.create_image.unwrap())(d, ci, a, out);
    if r == vk::Result::SUCCESS {
        let c = &*ci;
        write(
            &dev,
            &format!(
                "img,,,,,{:x},{},{}x{}x{},{},{},{},{},{:x},{:x}",
                (*out).as_raw(),
                c.format.as_raw(),
                c.extent.width,
                c.extent.height,
                c.extent.depth,
                c.mip_levels,
                c.array_layers,
                c.samples.as_raw(),
                c.tiling.as_raw(),
                c.usage.as_raw(),
                c.flags.as_raw()
            ),
        );
    }
    r
}

pub(crate) unsafe extern "system" fn layer_AllocateMemory(
    d: vk::Device,
    ai: *const vk::MemoryAllocateInfo,
    a: *const vk::AllocationCallbacks,
    out: *mut vk::DeviceMemory,
) -> vk::Result {
    let dev = dev_of(d).unwrap();
    let r = (dev.f.allocate_memory.unwrap())(d, ai, a, out);
    if r == vk::Result::SUCCESS {
        let mut dedicated = 0u64;
        let mut export = 0u32;
        let mut p = (*ai).p_next as *const vk::BaseInStructure;
        while !p.is_null() {
            match (*p).s_type {
                vk::StructureType::MEMORY_DEDICATED_ALLOCATE_INFO => {
                    dedicated = (*(p as *const vk::MemoryDedicatedAllocateInfo)).image.as_raw();
                }
                vk::StructureType::EXPORT_MEMORY_ALLOCATE_INFO => {
                    export = (*(p as *const vk::ExportMemoryAllocateInfo)).handle_types.as_raw();
                }
                _ => {}
            }
            p = (*p).p_next;
        }
        write(
            &dev,
            &format!(
                "mem,,,,,{:x},{},{},{dedicated:x},{export:x}",
                (*out).as_raw(),
                (*ai).allocation_size,
                (*ai).memory_type_index
            ),
        );
    }
    r
}

pub(crate) unsafe extern "system" fn layer_BindImageMemory(
    d: vk::Device,
    i: vk::Image,
    m: vk::DeviceMemory,
    o: vk::DeviceSize,
) -> vk::Result {
    let dev = dev_of(d).unwrap();
    let r = (dev.f.bind_image_memory.unwrap())(d, i, m, o);
    write(&dev, &format!("bind,,,,,{:x},{:x},{o}", i.as_raw(), m.as_raw()));
    r
}

pub(crate) unsafe extern "system" fn layer_BindImageMemory2(
    d: vk::Device,
    n: u32,
    bi: *const vk::BindImageMemoryInfo,
) -> vk::Result {
    let dev = dev_of(d).unwrap();
    let r = (dev.f.bind_image_memory2.unwrap())(d, n, bi);
    for k in 0..n as usize {
        let b = &*bi.add(k);
        write(&dev, &format!("bind,,,,,{:x},{:x},{}", b.image.as_raw(), b.memory.as_raw(), b.memory_offset));
    }
    r
}

pub(crate) unsafe fn note_render_pass(dev: &Dev, rp: vk::RenderPass, att: &[String], deps: &[String]) {
    write(dev, &format!("rpdef,,,,,{:x},{},{}", rp.as_raw(), att.join(" "), deps.join(" ")));
}

pub(crate) fn rp1_rows(ci: &vk::RenderPassCreateInfo) -> (Vec<String>, Vec<String>) {
    let att = unsafe { std::slice::from_raw_parts(ci.p_attachments, ci.attachment_count as usize) };
    let deps = unsafe { std::slice::from_raw_parts(ci.p_dependencies, ci.dependency_count as usize) };
    (
        att.iter()
            .map(|a| {
                format!(
                    "{}/{}/{}/{}/{}/{}/{}/{}",
                    a.format.as_raw(),
                    a.samples.as_raw(),
                    a.load_op.as_raw(),
                    a.store_op.as_raw(),
                    a.stencil_load_op.as_raw(),
                    a.stencil_store_op.as_raw(),
                    a.initial_layout.as_raw(),
                    a.final_layout.as_raw()
                )
            })
            .collect(),
        deps.iter()
            .map(|d| {
                format!(
                    "{}>{}:{:x}/{:x}/{:x}/{:x}/{:x}",
                    d.src_subpass as i32,
                    d.dst_subpass as i32,
                    d.src_stage_mask.as_raw(),
                    d.dst_stage_mask.as_raw(),
                    d.src_access_mask.as_raw(),
                    d.dst_access_mask.as_raw(),
                    d.dependency_flags.as_raw()
                )
            })
            .collect(),
    )
}

pub(crate) fn rp2_rows(ci: &vk::RenderPassCreateInfo2) -> (Vec<String>, Vec<String>) {
    let att = unsafe { std::slice::from_raw_parts(ci.p_attachments, ci.attachment_count as usize) };
    let deps = unsafe { std::slice::from_raw_parts(ci.p_dependencies, ci.dependency_count as usize) };
    (
        att.iter()
            .map(|a| {
                format!(
                    "{}/{}/{}/{}/{}/{}/{}/{}",
                    a.format.as_raw(),
                    a.samples.as_raw(),
                    a.load_op.as_raw(),
                    a.store_op.as_raw(),
                    a.stencil_load_op.as_raw(),
                    a.stencil_store_op.as_raw(),
                    a.initial_layout.as_raw(),
                    a.final_layout.as_raw()
                )
            })
            .collect(),
        deps.iter()
            .map(|d| {
                format!(
                    "{}>{}:{:x}/{:x}/{:x}/{:x}/{:x}",
                    d.src_subpass as i32,
                    d.dst_subpass as i32,
                    d.src_stage_mask.as_raw(),
                    d.dst_stage_mask.as_raw(),
                    d.src_access_mask.as_raw(),
                    d.dst_access_mask.as_raw(),
                    d.dependency_flags.as_raw()
                )
            })
            .collect(),
    )
}
