//! Media bridge: the session's audio devices, backed by Windows (protocol/wdp.h, "media channel").
//!
//! womarchy-mediad listens on the compositor's vsock port + 2 with its own session token and the usual
//! handshake (MEDIA_HELLO / MEDIA_WELCOME). It offers Linux apps a PipeWire sink and source while we are
//! connected:
//! - **Speakers.** What apps play arrives as AUDIO_PLAY_START, AUDIO_PLAY_DATA..., AUDIO_PLAY_STOP. A
//!   player thread plays it with WASAPI (shared mode) on Windows' default output device, and follows
//!   that device when it changes.
//! - **Microphone.** mediad sends AUDIO_CAPTURE_START only while a Linux app records from its source,
//!   and AUDIO_CAPTURE_STOP when none does; in between a recorder thread captures Windows' default
//!   input device with WASAPI and sends AUDIO_CAPTURE_DATA. So the microphone is open (and Windows
//!   shows it in use) exactly while something in Linux records.
//!
//! Clocks: Linux produces playback at its own pace, the sound card consumes at its own. The player keeps
//! ~30 ms queued and nudges WASAPI's sample rate (IAudioClockAdjustment) by up to ±0.5 % to hold that
//! level; if that is not available it drops excess audio once it is clearly too much. The capture side
//! is corrected in mediad.

use crate::net::Conn;
use crate::wdp;
use std::collections::VecDeque;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use std::thread;
use std::time::{Duration, Instant};
use windows::core::{GUID, PWSTR};
use windows::Win32::Foundation::{CloseHandle, HANDLE, WAIT_OBJECT_0};
use windows::Win32::Media::Audio::{
    eCapture, eConsole, eRender, EDataFlow, IAudioCaptureClient, IAudioClient, IAudioClockAdjustment, IAudioRenderClient, IMMDeviceEnumerator,
    MMDeviceEnumerator, AUDCLNT_BUFFERFLAGS_SILENT, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
    AUDCLNT_STREAMFLAGS_RATEADJUST, AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, WAVEFORMATEX,
};
use windows::Win32::System::Com::{CoCreateInstance, CoInitializeEx, CoTaskMemFree, CLSCTX_ALL, COINIT_MULTITHREADED};
use windows::Win32::System::Threading::{CreateEventW, WaitForSingleObject};

const WAVE_FORMAT_IEEE_FLOAT: u16 = 3;
/// WASAPI buffer we ask for (100 ns units): 20 ms.
const DEVICE_BUFFER_HNS: i64 = 200_000;
/// Playback queued on our side that the rate control aims for, and the most it lets build up.
const PLAY_TARGET: Duration = Duration::from_millis(30);
const PLAY_MAX: Duration = Duration::from_millis(200);
/// How often the threads check whether Windows' default device changed.
const DEVICE_POLL: Duration = Duration::from_secs(1);

/// What the reader thread hands the player: queued samples and the stream's state.
struct Playback {
    rate: u32,
    channels: u32,
    samples: VecDeque<f32>,
    /// between PLAY_START and PLAY_STOP
    active: bool,
    /// a player thread is running (it exits once stopped and drained)
    running: bool,
    /// bumped when the format changes: the player reopens the device
    generation: u64,
    /// the connection is gone: stop now
    quit: bool,
}

struct PlayShared {
    state: Mutex<Playback>,
    wake: Condvar,
}

/// Connect to womarchy-mediad and keep the connection for the session (it starts a little after the
/// compositor and may restart).
pub fn start(vm: GUID, port: u32, token: [u8; wdp::TOKEN_BYTES]) {
    thread::spawn(move || {
        let mut announced = false;
        loop {
            let Some(conn) = connect(vm, port, &token) else {
                if !announced {
                    eprintln!("[omarchy] media: womarchy-mediad did not answer; Linux audio uses its own fallback (WSLg)");
                }
                return;
            };
            if !announced {
                eprintln!("[omarchy] media: connected (speakers and microphone)");
                announced = true;
            }
            serve(conn);
            thread::sleep(Duration::from_millis(500));
        }
    });
}

/// Connect, prove our half of the token and check mediad's (up to two minutes of retries).
fn connect(vm: GUID, port: u32, token: &[u8; wdp::TOKEN_BYTES]) -> Option<Conn> {
    let start = Instant::now();
    while start.elapsed() < Duration::from_secs(120) {
        if let Ok(conn) = Conn::connect(vm, port) {
            conn.set_recv_timeout(Some(Duration::from_secs(5)));
            let mut payload = Vec::new();
            let ok = conn.send_msg(&wdp::media_hello(token, wdp::MEDIA_CAP_PLAYBACK | wdp::MEDIA_CAP_CAPTURE)).is_ok()
                && matches!(conn.recv_msg(&mut payload), Ok(wdp::MEDIA_WELCOME))
                && wdp::proves(token, payload.get(4..4 + wdp::PROOF_BYTES).unwrap_or(&[]));
            conn.set_recv_timeout(None);
            if ok {
                return Some(conn);
            }
            eprintln!("[omarchy] media: the media daemon failed to authenticate; not sharing audio with it");
            return None;
        }
        thread::sleep(Duration::from_millis(500));
    }
    None
}

/// Run one connection until it closes.
fn serve(conn: Conn) {
    let play = Arc::new(PlayShared {
        state: Mutex::new(Playback { rate: 48000, channels: 2, samples: VecDeque::new(), active: false, running: false, generation: 0, quit: false }),
        wake: Condvar::new(),
    });
    let mut capture: Option<Arc<AtomicBool>> = None; // the running recorder's stop flag
    let mut payload = Vec::new();
    while let Ok(ty) = conn.recv_msg(&mut payload) {
        match ty {
            wdp::AUDIO_PLAY_START => {
                let Some((rate, channels)) = wdp::parse_audio_format(&payload) else { continue };
                let mut st = play.state.lock().unwrap();
                if (st.rate, st.channels) != (rate, channels) {
                    st.samples.clear();
                    st.generation += 1;
                }
                st.rate = rate;
                st.channels = channels;
                st.active = true;
                if !st.running {
                    st.running = true;
                    let play = play.clone();
                    thread::spawn(move || player(play));
                }
                play.wake.notify_all();
            }
            wdp::AUDIO_PLAY_DATA if payload.len() <= wdp::MAX_AUDIO_DATA => {
                let mut st = play.state.lock().unwrap();
                if !st.active {
                    continue;
                }
                st.samples.extend(payload.as_chunks::<4>().0.iter().map(|b| f32::from_le_bytes(*b)));
                // the player is stuck (e.g. no output device): keep the newest second, no more
                let cap = (st.rate * st.channels) as usize;
                if st.samples.len() > cap {
                    let extra = st.samples.len() - cap;
                    st.samples.drain(..extra);
                }
                play.wake.notify_all();
            }
            wdp::AUDIO_PLAY_STOP => {
                play.state.lock().unwrap().active = false; // the player drains what is queued, then stops
                play.wake.notify_all();
            }
            wdp::AUDIO_CAPTURE_START => {
                let Some((rate, channels)) = wdp::parse_audio_format(&payload) else { continue };
                if let Some(stop) = capture.take() {
                    stop.store(true, Ordering::SeqCst);
                }
                let stop = Arc::new(AtomicBool::new(false));
                capture = Some(stop.clone());
                let conn = conn.clone();
                thread::spawn(move || recorder(conn, rate, channels, stop));
            }
            wdp::AUDIO_CAPTURE_STOP => {
                if let Some(stop) = capture.take() {
                    stop.store(true, Ordering::SeqCst);
                }
            }
            _ => {} // unknown: skip
        }
    }
    if let Some(stop) = capture.take() {
        stop.store(true, Ordering::SeqCst);
    }
    let mut st = play.state.lock().unwrap();
    st.quit = true;
    st.active = false;
    play.wake.notify_all();
}

// --- WASAPI

struct Event(HANDLE);
impl Drop for Event {
    fn drop(&mut self) {
        unsafe {
            let _ = CloseHandle(self.0);
        }
    }
}

fn com_init() {
    unsafe {
        let _ = CoInitializeEx(None, COINIT_MULTITHREADED);
    }
}

fn float_format(rate: u32, channels: u32) -> WAVEFORMATEX {
    let block = (channels * 4) as u16;
    WAVEFORMATEX {
        wFormatTag: WAVE_FORMAT_IEEE_FLOAT,
        nChannels: channels as u16,
        nSamplesPerSec: rate,
        nAvgBytesPerSec: rate * block as u32,
        nBlockAlign: block,
        wBitsPerSample: 32,
        cbSize: 0,
    }
}

/// The id of Windows' current default device for `flow`, to notice when the user switches devices.
fn default_device_id(e: &IMMDeviceEnumerator, flow: EDataFlow) -> Option<String> {
    unsafe {
        let dev = e.GetDefaultAudioEndpoint(flow, eConsole).ok()?;
        let id: PWSTR = dev.GetId().ok()?;
        let s = id.to_string().ok();
        CoTaskMemFree(Some(id.0 as *const _));
        s
    }
}

/// An initialised shared-mode client on the default device, with its wake-up event.
struct Stream {
    client: IAudioClient,
    event: Event,
    device_id: Option<String>,
    frames: u32,
}

fn open(e: &IMMDeviceEnumerator, flow: EDataFlow, rate: u32, channels: u32, rate_adjust: bool) -> windows::core::Result<Stream> {
    unsafe {
        let device = e.GetDefaultAudioEndpoint(flow, eConsole)?;
        let client: IAudioClient = device.Activate(CLSCTX_ALL, None)?;
        let fmt = float_format(rate, channels);
        let mut flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        if rate_adjust {
            flags |= AUDCLNT_STREAMFLAGS_RATEADJUST;
        }
        client.Initialize(AUDCLNT_SHAREMODE_SHARED, flags, DEVICE_BUFFER_HNS, 0, &fmt, None)?;
        let event = Event(CreateEventW(None, false, false, None)?);
        client.SetEventHandle(event.0)?;
        let frames = client.GetBufferSize()?;
        Ok(Stream { client, event, device_id: default_device_id(e, flow), frames })
    }
}

/// Plays the queued stream until it is stopped and drained (or the connection ends).
fn player(play: Arc<PlayShared>) {
    com_init();
    let enumerator: IMMDeviceEnumerator = match unsafe { CoCreateInstance(&MMDeviceEnumerator, None, CLSCTX_ALL) } {
        Ok(e) => e,
        Err(e) => {
            eprintln!("[omarchy] media: no audio device enumerator ({}); no sound", e);
            play.state.lock().unwrap().running = false;
            return;
        }
    };
    let mut warned = false;
    let (mut played, mut underruns) = (0u64, 0u32); // frames, for the log line at the end
    'device: loop {
        let (rate, channels, generation) = {
            let st = play.state.lock().unwrap();
            if st.quit || (!st.active && st.samples.is_empty()) {
                drop(st);
                break 'device;
            }
            (st.rate, st.channels, st.generation)
        };
        // rate adjustment if the audio engine allows it with format conversion, else without
        let stream = open(&enumerator, eRender, rate, channels, true).or_else(|_| open(&enumerator, eRender, rate, channels, false));
        let stream = match stream {
            Ok(s) => s,
            Err(e) => {
                if !warned {
                    eprintln!("[omarchy] media: cannot open the default output device ({}); retrying", e);
                    warned = true;
                }
                let mut st = play.state.lock().unwrap();
                st.samples.clear(); // keep up with Linux rather than play stale audio later
                let _ = play.wake.wait_timeout(st, Duration::from_secs(1));
                continue 'device;
            }
        };
        warned = false;
        let render: IAudioRenderClient = match unsafe { stream.client.GetService() } {
            Ok(r) => r,
            Err(_) => break 'device,
        };
        let clock: Option<IAudioClockAdjustment> = unsafe { stream.client.GetService().ok() };
        if clock.is_none() {
            eprintln!("[omarchy] media: no sample-rate adjustment on this device; drift is corrected by skipping audio");
        }
        let ch = channels as usize;
        let target = (rate as u64 * PLAY_TARGET.as_millis() as u64 / 1000) as usize; // frames
        let max = (rate as u64 * PLAY_MAX.as_millis() as u64 / 1000) as usize;
        let mut started = false;
        let mut priming = true; // wait for `target` frames before playing (and after an underrun)
        let mut fill_avg = target as f64;
        let mut last_ratio = 1.0f64;
        let mut last_poll = Instant::now();

        loop {
            unsafe {
                WaitForSingleObject(stream.event.0, 20);
            }
            if last_poll.elapsed() >= DEVICE_POLL {
                last_poll = Instant::now();
                if default_device_id(&enumerator, eRender) != stream.device_id {
                    eprintln!("[omarchy] media: the default output device changed; switching");
                    unsafe {
                        let _ = stream.client.Stop();
                    }
                    continue 'device;
                }
            }
            let Ok(padding) = (unsafe { stream.client.GetCurrentPadding() }) else {
                continue 'device; // device invalidated (unplugged, format changed, ...): reopen
            };
            let space = (stream.frames - padding) as usize;

            let mut st = play.state.lock().unwrap();
            if st.quit || st.generation != generation {
                drop(st);
                unsafe {
                    let _ = stream.client.Stop();
                }
                continue 'device;
            }
            let queued = st.samples.len() / ch;
            if !st.active && queued == 0 {
                if padding == 0 {
                    // stopped and played out
                    st.running = false;
                    drop(st);
                    unsafe {
                        let _ = stream.client.Stop();
                    }
                    eprintln!("[omarchy] media: played {:.1} s ({} underruns)", played as f64 / rate as f64, underruns);
                    return;
                }
                continue;
            }
            if priming && (queued >= target || !st.active) {
                priming = false;
            }
            if priming {
                continue;
            }
            if queued > max {
                // far behind (a stall, or no clock adjustment): jump to the target
                let drop_frames = queued - target;
                st.samples.drain(..drop_frames * ch);
            }
            let queued = st.samples.len() / ch;
            let n = queued.min(space);
            if n > 0 {
                unsafe {
                    if let Ok(buf) = render.GetBuffer(n as u32) {
                        let out = std::slice::from_raw_parts_mut(buf as *mut f32, n * ch);
                        for (o, s) in out.iter_mut().zip(st.samples.drain(..n * ch)) {
                            *o = s;
                        }
                        let _ = render.ReleaseBuffer(n as u32, 0);
                    }
                    played += n as u64;
                }
                if !started {
                    started = unsafe { stream.client.Start() }.is_ok();
                }
            } else if st.active && padding == 0 && started {
                priming = true; // underrun: build the cushion up again
                underruns += 1;
            }

            // hold the queue at `target` by playing slightly faster or slower than nominal
            if let Some(clock) = &clock {
                fill_avg += (st.samples.len() as f64 / ch as f64 - fill_avg) * 0.02;
                let error = (fill_avg - target as f64) / target as f64;
                let ratio = 1.0 + (error * 0.002).clamp(-0.005, 0.005);
                if (ratio - last_ratio).abs() > 0.0001 && unsafe { clock.SetSampleRate(rate as f32 * ratio as f32) }.is_ok() {
                    last_ratio = ratio;
                }
            }
        }
    }
    play.state.lock().unwrap().running = false;
}

/// Records the default input device and sends it to mediad until `stop`.
fn recorder(conn: Conn, rate: u32, channels: u32, stop: Arc<AtomicBool>) {
    com_init();
    let enumerator: IMMDeviceEnumerator = match unsafe { CoCreateInstance(&MMDeviceEnumerator, None, CLSCTX_ALL) } {
        Ok(e) => e,
        Err(e) => {
            let _ = conn.send_msg(&wdp::audio_capture_failed(e.code().0));
            return;
        }
    };
    let ch = channels as usize;
    let mut chunk: Vec<f32> = Vec::with_capacity(rate as usize * ch / 50);
    let mut failures = 0;
    'device: while !stop.load(Ordering::SeqCst) {
        let stream = match open(&enumerator, eCapture, rate, channels, false) {
            Ok(s) => s,
            Err(e) => {
                if failures == 0 {
                    eprintln!("[omarchy] media: cannot open the microphone ({})", e);
                    let _ = conn.send_msg(&wdp::audio_capture_failed(e.code().0));
                }
                failures += 1;
                for _ in 0..10 {
                    if stop.load(Ordering::SeqCst) {
                        return;
                    }
                    thread::sleep(Duration::from_millis(100));
                }
                continue 'device;
            }
        };
        failures = 0;
        let Ok(capture) = (unsafe { stream.client.GetService::<IAudioCaptureClient>() }) else { return };
        if unsafe { stream.client.Start() }.is_err() {
            return;
        }
        let mut last_poll = Instant::now();
        while !stop.load(Ordering::SeqCst) {
            unsafe {
                if WaitForSingleObject(stream.event.0, 100) != WAIT_OBJECT_0 && last_poll.elapsed() < DEVICE_POLL {
                    continue;
                }
            }
            if last_poll.elapsed() >= DEVICE_POLL {
                last_poll = Instant::now();
                if default_device_id(&enumerator, eCapture) != stream.device_id {
                    eprintln!("[omarchy] media: the default microphone changed; switching");
                    unsafe {
                        let _ = stream.client.Stop();
                    }
                    continue 'device;
                }
            }
            chunk.clear();
            loop {
                let Ok(packet) = (unsafe { capture.GetNextPacketSize() }) else {
                    continue 'device; // invalidated: reopen
                };
                if packet == 0 {
                    break;
                }
                let (mut data, mut frames, mut flags) = (std::ptr::null_mut::<u8>(), 0u32, 0u32);
                if unsafe { capture.GetBuffer(&mut data, &mut frames, &mut flags, None, None) }.is_err() {
                    continue 'device;
                }
                let n = frames as usize * ch;
                if flags & AUDCLNT_BUFFERFLAGS_SILENT.0 as u32 != 0 || data.is_null() {
                    chunk.resize(chunk.len() + n, 0.0);
                } else {
                    chunk.extend_from_slice(unsafe { std::slice::from_raw_parts(data as *const f32, n) });
                }
                unsafe {
                    let _ = capture.ReleaseBuffer(frames);
                }
            }
            if !chunk.is_empty() && conn.send_msg(&wdp::audio_capture_data(&chunk)).is_err() {
                return;
            }
        }
        unsafe {
            let _ = stream.client.Stop();
        }
    }
}
