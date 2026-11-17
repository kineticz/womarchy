//! Windows cameras for the session (protocol/wdp-media.h, CAMERA_*).
//!
//! At connect we list the video cameras (Media Foundation's video capture devices; Windows Hello's IR
//! camera is a different category and not among them) with the modes we can deliver: their native
//! frame sizes and rates up to 1080p, always as NV12. mediad offers each camera as a PipeWire video
//! source and sends CAMERA_START with the mode an app negotiated when one starts streaming, CAMERA_STOP
//! when none streams any more. In between a capture thread reads frames with a source reader (which
//! decodes MJPG and converts YUY2 to NV12) and sends them as CAMERA_FRAME. So the camera, and its light,
//! are on exactly while a Linux app uses it.

use crate::net::Conn;
use crate::wdp::{self, CameraMode};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Once};
use std::time::Instant;
use windows::core::{Interface, GUID, HSTRING, PWSTR};
use windows::Win32::Media::MediaFoundation::*;
use windows::Win32::System::Com::{CoInitializeEx, CoTaskMemFree, COINIT_MULTITHREADED};

const READER_VIDEO: u32 = MF_SOURCE_READER_FIRST_VIDEO_STREAM.0 as u32;
const MIN_FPS: f64 = 5.0;
const MAX_FPS: f64 = 60.0;

#[derive(Clone, Debug)]
pub struct Camera {
    pub id: u32,
    pub name: String,
    symlink: String,
    pub modes: Vec<CameraMode>,
}

fn startup() {
    static MF: Once = Once::new();
    unsafe {
        let _ = CoInitializeEx(None, COINIT_MULTITHREADED);
    }
    MF.call_once(|| unsafe {
        let _ = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    });
}

fn split(v: u64) -> (u32, u32) {
    ((v >> 32) as u32, v as u32)
}

unsafe fn attr_string(a: &IMFActivate, key: &GUID) -> Option<String> {
    let mut p = PWSTR::null();
    let mut n = 0u32;
    a.GetAllocatedString(key, &mut p, &mut n).ok()?;
    let s = p.to_string().ok();
    CoTaskMemFree(Some(p.0 as *const _));
    s
}

/// A source reader on the camera with this symbolic link (opens the device, but starts no stream).
unsafe fn open_reader(symlink: &str) -> windows::core::Result<(IMFMediaSource, IMFSourceReader)> {
    let mut attrs: Option<IMFAttributes> = None;
    MFCreateAttributes(&mut attrs, 2)?;
    let attrs = attrs.unwrap();
    attrs.SetGUID(&MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID)?;
    attrs.SetString(&MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &HSTRING::from(symlink))?;
    let source = MFCreateDeviceSource(&attrs)?;
    let mut rattrs: Option<IMFAttributes> = None;
    MFCreateAttributes(&mut rattrs, 1)?;
    let rattrs = rattrs.unwrap();
    rattrs.SetUINT32(&MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, 1)?;
    match MFCreateSourceReaderFromMediaSource(&source, &rattrs) {
        Ok(reader) => Ok((source, reader)),
        Err(e) => {
            let _ = source.Shutdown();
            Err(e)
        }
    }
}

/// The camera's native types as (mode, subtype), in the driver's order.
unsafe fn native_types(reader: &IMFSourceReader) -> Vec<(CameraMode, GUID, IMFMediaType)> {
    let mut out = Vec::new();
    for i in 0.. {
        let Ok(t) = reader.GetNativeMediaType(READER_VIDEO, i) else { break };
        let (Ok(size), Ok(rate), Ok(sub)) = (t.GetUINT64(&MF_MT_FRAME_SIZE), t.GetUINT64(&MF_MT_FRAME_RATE), t.GetGUID(&MF_MT_SUBTYPE)) else {
            continue;
        };
        let ((width, height), (fps_num, fps_den)) = (split(size), split(rate));
        out.push((CameraMode { width, height, fps_num, fps_den }, sub, t));
    }
    out
}

fn deliverable(m: &CameraMode) -> bool {
    let fps = m.fps_num as f64 / m.fps_den.max(1) as f64;
    m.width <= wdp::MAX_CAMERA_WIDTH && m.height <= wdp::MAX_CAMERA_HEIGHT && m.width % 2 == 0 && m.height % 2 == 0 && m.width >= 160 && (MIN_FPS..=MAX_FPS).contains(&fps)
}

/// Windows' video cameras and the modes each can deliver (largest first, 720p first if there is one:
/// what an app that takes the first offer gets).
pub fn list() -> Vec<Camera> {
    startup();
    let mut cameras = Vec::new();
    unsafe {
        let mut attrs: Option<IMFAttributes> = None;
        if MFCreateAttributes(&mut attrs, 1).is_err() {
            return cameras;
        }
        let attrs = attrs.unwrap();
        let _ = attrs.SetGUID(&MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        let mut devices: *mut Option<IMFActivate> = std::ptr::null_mut();
        let mut count = 0u32;
        if MFEnumDeviceSources(&attrs, &mut devices, &mut count).is_err() || devices.is_null() {
            return cameras;
        }
        for i in 0..count as usize {
            let Some(dev) = (*devices.add(i)).take() else { continue };
            if cameras.len() >= wdp::MAX_CAMERAS {
                continue;
            }
            let (Some(name), Some(symlink)) =
                (attr_string(&dev, &MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME), attr_string(&dev, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK))
            else {
                continue;
            };
            let modes = match open_reader(&symlink) {
                Ok((source, reader)) => {
                    let mut modes: Vec<CameraMode> = native_types(&reader).into_iter().map(|(m, _, _)| m).filter(deliverable).collect();
                    let _ = source.Shutdown();
                    let key = |m: &CameraMode| {
                        let preferred = (m.width, m.height) == (1280, 720);
                        (!preferred, std::cmp::Reverse(m.width * m.height), std::cmp::Reverse(m.fps_num * 1000 / m.fps_den.max(1)))
                    };
                    modes.sort_by_key(key);
                    modes.dedup();
                    modes.truncate(wdp::MAX_CAMERA_MODES);
                    modes
                }
                Err(e) => {
                    eprintln!("[omarchy] camera: cannot open {} ({})", name, e);
                    continue;
                }
            };
            if !modes.is_empty() {
                cameras.push(Camera { id: cameras.len() as u32, name, symlink, modes });
            }
        }
        CoTaskMemFree(Some(devices as *const _));
    }
    cameras
}

/// Streams `camera` in `mode` as NV12 frames to mediad until `stop`.
pub fn capture(conn: Conn, camera: Camera, mode: CameraMode, stop: Arc<AtomicBool>) {
    startup();
    let fail = |e: windows::core::Error| {
        eprintln!("[omarchy] camera: cannot start {} ({})", camera.name, e);
        let _ = conn.send_msg(&wdp::camera_failed(camera.id, e.code().0));
    };
    let (source, reader) = match unsafe { open_reader(&camera.symlink) } {
        Ok(r) => r,
        Err(e) => return fail(e),
    };
    let result = unsafe { stream(&conn, &camera, mode, &reader, &stop) };
    unsafe {
        let _ = source.Shutdown();
    }
    if let Err(e) = result {
        fail(e);
    }
}

unsafe fn stream(conn: &Conn, camera: &Camera, mode: CameraMode, reader: &IMFSourceReader, stop: &AtomicBool) -> windows::core::Result<()> {
    // the native type for this mode, uncompressed if the camera has one (no MJPG decoding)
    let rank = |sub: &GUID| match *sub {
        s if s == MFVideoFormat_NV12 => 0,
        s if s == MFVideoFormat_YUY2 => 1,
        s if s == MFVideoFormat_MJPG => 3,
        _ => 2,
    };
    let native = native_types(reader)
        .into_iter()
        .filter(|(m, _, _)| *m == mode)
        .min_by_key(|(_, sub, _)| rank(sub))
        .ok_or_else(|| windows::core::Error::from(MF_E_INVALIDMEDIATYPE))?;
    reader.SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS.0 as u32, false)?;
    reader.SetStreamSelection(READER_VIDEO, true)?;
    reader.SetCurrentMediaType(READER_VIDEO, None, &native.2)?;
    let out = MFCreateMediaType()?;
    out.SetGUID(&MF_MT_MAJOR_TYPE, &MFMediaType_Video)?;
    out.SetGUID(&MF_MT_SUBTYPE, &MFVideoFormat_NV12)?;
    out.SetUINT64(&MF_MT_FRAME_SIZE, ((mode.width as u64) << 32) | mode.height as u64)?;
    out.SetUINT64(&MF_MT_FRAME_RATE, ((mode.fps_num as u64) << 32) | mode.fps_den as u64)?;
    reader.SetCurrentMediaType(READER_VIDEO, None, &out)?;
    let current = reader.GetCurrentMediaType(READER_VIDEO)?;
    let (w, h) = split(current.GetUINT64(&MF_MT_FRAME_SIZE)?);
    if (w, h) != (mode.width, mode.height) {
        return Err(MF_E_INVALIDMEDIATYPE.into());
    }
    let default_stride = current.GetUINT32(&MF_MT_DEFAULT_STRIDE).map(|s| s as i32).unwrap_or(w as i32);
    eprintln!("[omarchy] camera: {} on, {}x{} at {:.0} fps", camera.name, w, h, mode.fps_num as f64 / mode.fps_den as f64);

    let (w, h) = (w as usize, h as usize);
    let mut nv12 = vec![0u8; w * h * 3 / 2];
    let start = Instant::now();
    let mut frames = 0u64;
    while !stop.load(Ordering::SeqCst) {
        let mut flags = 0u32;
        let mut sample: Option<IMFSample> = None;
        reader.ReadSample(READER_VIDEO, 0, None, Some(&mut flags), None, Some(&mut sample))?;
        if flags & (MF_SOURCE_READERF_ERROR.0 | MF_SOURCE_READERF_ENDOFSTREAM.0) as u32 != 0 {
            return Err(MF_E_VIDEO_RECORDING_DEVICE_INVALIDATED.into());
        }
        let Some(sample) = sample else { continue }; // a stream tick (gap)
        let buffer = sample.ConvertToContiguousBuffer()?;
        let len = buffer.GetCurrentLength()? as usize;
        // NV12 planes: Y rows of `pitch` bytes, then the interleaved UV rows; the Y plane may have
        // padding rows (decoders align heights), which the buffer length reveals
        let copied = if let Ok(b2) = buffer.cast::<IMF2DBuffer>() {
            let (mut scan0, mut pitch) = (std::ptr::null_mut::<u8>(), 0i32);
            b2.Lock2D(&mut scan0, &mut pitch)?;
            let ok = copy_nv12(scan0, pitch, len, w, h, &mut nv12);
            let _ = b2.Unlock2D();
            ok
        } else {
            let mut p = std::ptr::null_mut::<u8>();
            buffer.Lock(&mut p, None, None)?;
            let ok = copy_nv12(p, default_stride, len, w, h, &mut nv12);
            let _ = buffer.Unlock();
            ok
        };
        if !copied {
            continue;
        }
        let ts = start.elapsed().as_nanos() as u64;
        if conn.send_msg(&wdp::camera_frame(camera.id, w as u32, h as u32, ts, &nv12)).is_err() {
            return Ok(()); // the connection is gone
        }
        frames += 1;
    }
    let secs = start.elapsed().as_secs_f64();
    eprintln!("[omarchy] camera: {} off after {:.1} s ({:.1} frames/s)", camera.name, secs, frames as f64 / secs.max(0.001));
    Ok(())
}

/// Copy an NV12 image with row pitch `pitch` (buffer `len` bytes) into the packed `out`.
unsafe fn copy_nv12(src: *const u8, pitch: i32, len: usize, w: usize, h: usize, out: &mut [u8]) -> bool {
    if src.is_null() || pitch <= 0 || (pitch as usize) < w {
        return false;
    }
    let pitch = pitch as usize;
    let fits = |uv: usize| uv + pitch * (h / 2 - 1) + w <= len;
    // the UV plane follows the Y plane, which may have padding rows (decoders align heights): the
    // buffer holds 1.5 planes, so its length tells; else assume none
    let padded = pitch * (len / pitch * 2 / 3).max(h);
    let uv_start = match (fits(padded), fits(pitch * h)) {
        (true, _) => padded,
        (false, true) => pitch * h,
        _ => return false,
    };
    let src = std::slice::from_raw_parts(src, len);
    for y in 0..h {
        out[y * w..(y + 1) * w].copy_from_slice(&src[y * pitch..y * pitch + w]);
    }
    for y in 0..h / 2 {
        let o = w * h + y * w;
        out[o..o + w].copy_from_slice(&src[uv_start + y * pitch..uv_start + y * pitch + w]);
    }
    true
}
