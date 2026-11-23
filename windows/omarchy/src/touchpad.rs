//! Precision touchpads: two-finger scrolling as touchpad (finger) scrolling, and 3+ finger swipes.
//!
//! Windows turns touchpad scrolling into WM_MOUSEWHEEL messages with small deltas (not multiples of
//! WHEEL_DELTA) and keeps sending them after the fingers lift (inertia). Passed on as wheel clicks,
//! Hyprland's discrete-scroll emulation turned every small step, including the backward jitter at the
//! end of a scroll, into a whole click. Windows gives applications no gesture messages for 3+ fingers.
//!
//! So the viewer also reads the touchpad's own HID reports (Raw Input, usage page 0x0D, usage 0x05):
//!  * while fingers are on the pad, wheel messages are sent as FINGER scrolling (smooth, Hyprland's
//!    `input:touchpad:scroll_factor`); when the fingers lift, an axis stop lets clients do their own
//!    kinetic scrolling, as on Linux, and Windows' inertia that follows is dropped;
//!  * 3+ fingers moving together become swipe begin/update/end (Hyprland's `gesture` bindings).
//!    Windows' own three- and four-finger gestures must be off (Settings > Bluetooth & devices >
//!    Touchpad), or Windows acts on them as well.
//!
//! Without a precision touchpad (no reports), wheel deltas that are not whole clicks are sent as
//! CONTINUOUS (smooth) scrolling and whole clicks as WHEEL.
//!
//! The logic (frame assembly, gestures, scroll classification) is plain Rust and unit tested; only
//! `raw` touches Win32. Everything runs on the UI thread.

use crate::wdp;

/// Swipe motion in pointer units per millimetre of finger travel (libinput reports gestures in
/// about-pixel units; Hyprland's workspace swipe distance defaults to 300 of them).
const SWIPE_UNITS_PER_MM: f64 = 20.0;
/// Finger travel before 3+ resting fingers count as a swipe.
const SWIPE_START_MM: f64 = 2.0;
/// Smooth scroll distance per WHEEL_DELTA (120): what Hyprland uses for one wheel click.
const SCROLL_PER_WHEEL_DELTA: f64 = 15.0;
/// Wheel messages closer together than this after the fingers lifted are Windows' inertia.
const INERTIA_GAP_MS: u32 = 250;

/// A finger on the pad, in millimetres from the pad's top left corner.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Contact {
    pub id: u32,
    pub x: f64,
    pub y: f64,
}

/// A contact slot as read from one HID report.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Slot {
    pub contact: Contact,
    pub touching: bool, // the tip switch
}

// ------------------------------------------------------------------ frame assembly

/// Precision touchpads send each scan as one report (parallel mode) or as several (hybrid mode): the
/// first carries the scan's contact count, the rest a count of 0. Collects one scan's contacts.
#[derive(Default)]
pub struct Assembler {
    expected: usize,
    pending: Vec<Slot>,
}

impl Assembler {
    /// One report: its contact count and its contact slots in report order. Returns the touching
    /// contacts once the scan is complete.
    pub fn report(&mut self, contact_count: u32, slots: &[Slot]) -> Option<Vec<Contact>> {
        if contact_count > 0 {
            self.expected = contact_count as usize;
            self.pending.clear();
        }
        if self.expected == 0 {
            return None;
        }
        let take = slots.len().min(self.expected - self.pending.len());
        self.pending.extend_from_slice(&slots[..take]);
        if self.pending.len() < self.expected {
            return None;
        }
        self.expected = 0;
        Some(self.pending.drain(..).filter(|s| s.touching).map(|s| s.contact).collect())
    }
}

// ------------------------------------------------------------------ swipes

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum SwipeEvent {
    Begin(u32),
    Update(u32, f64, f64),
    End { cancelled: bool },
}

/// libinput-style swipes from touchpad scans: BEGIN once 3+ fingers moved together, UPDATE with the
/// motion of their centre, END when the finger count changes (cancelled when a finger was added).
#[derive(Default)]
pub struct Swipes {
    prev: Vec<Contact>,
    active: Option<u32>,      // fingers of the running swipe
    candidate: Option<u32>,   // 3+ fingers resting, not yet moved far enough
    travel: (f64, f64),       // the candidate's motion so far, mm
}

/// Average motion of the contacts present in both scans.
fn centre_motion(prev: &[Contact], now: &[Contact]) -> (f64, f64) {
    let (mut dx, mut dy, mut n) = (0.0, 0.0, 0);
    for c in now {
        if let Some(p) = prev.iter().find(|p| p.id == c.id) {
            dx += c.x - p.x;
            dy += c.y - p.y;
            n += 1;
        }
    }
    if n == 0 {
        (0.0, 0.0)
    } else {
        (dx / n as f64, dy / n as f64)
    }
}

impl Swipes {
    pub fn scan(&mut self, contacts: &[Contact]) -> Vec<SwipeEvent> {
        let mut out = Vec::new();
        let n = contacts.len() as u32;
        let (dx, dy) = centre_motion(&self.prev, contacts);
        if let Some(f) = self.active {
            if n == f {
                if dx != 0.0 || dy != 0.0 {
                    out.push(SwipeEvent::Update(f, dx * SWIPE_UNITS_PER_MM, dy * SWIPE_UNITS_PER_MM));
                }
            } else {
                out.push(SwipeEvent::End { cancelled: n > f });
                self.active = None;
                self.candidate = None;
            }
        } else if n >= 3 {
            if self.candidate == Some(n) {
                self.travel.0 += dx;
                self.travel.1 += dy;
                if self.travel.0.hypot(self.travel.1) >= SWIPE_START_MM {
                    out.push(SwipeEvent::Begin(n));
                    out.push(SwipeEvent::Update(n, self.travel.0 * SWIPE_UNITS_PER_MM, self.travel.1 * SWIPE_UNITS_PER_MM));
                    self.active = Some(n);
                    self.candidate = None;
                }
            } else {
                self.candidate = Some(n);
                self.travel = (0.0, 0.0);
            }
        } else {
            self.candidate = None;
        }
        self.prev = contacts.to_vec();
        out
    }

    /// The viewer lost focus or the touchpad went away: end a running swipe.
    pub fn reset(&mut self) -> Option<SwipeEvent> {
        self.prev.clear();
        self.candidate = None;
        self.active.take().map(|_| SwipeEvent::End { cancelled: true })
    }
}

// ------------------------------------------------------------------ scrolling

/// A scroll message for the compositor.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Axis {
    pub axis: u32, // 0 = vertical, 1 = horizontal
    pub delta: f64,
    pub discrete120: i32,
    pub source: u32,
}

/// Decides how each WM_MOUSEWHEEL/WM_MOUSEHWHEEL reaches the compositor (see the module comment).
#[derive(Default)]
pub struct Scroll {
    touchpad: bool,     // a precision touchpad has sent reports
    fingers: u32,       // fingers on it now
    scrolled: [bool; 2], // axes scrolled since the fingers went down (they need a stop)
    last_touch_wheel: Option<u32>, // time of the last wheel message that belonged to the touchpad
}

impl Scroll {
    /// A complete touchpad scan with `fingers` touching. Returns the axis stops to send.
    pub fn fingers(&mut self, fingers: u32) -> Vec<Axis> {
        self.touchpad = true;
        let was = self.fingers;
        self.fingers = fingers;
        if was > 0 && (fingers == 0 || fingers >= 3) {
            self.stops()
        } else {
            Vec::new()
        }
    }

    fn stops(&mut self) -> Vec<Axis> {
        let mut out = Vec::new();
        for axis in 0..2 {
            if std::mem::take(&mut self.scrolled[axis]) {
                out.push(Axis { axis: axis as u32, delta: 0.0, discrete120: 0, source: wdp::AXIS_SOURCE_FINGER });
            }
        }
        out
    }

    /// One wheel message: `d120` in WHEEL_DELTA units, positive = down/right. None = drop it.
    pub fn wheel(&mut self, now_ms: u32, axis: u32, d120: i32) -> Option<Axis> {
        let delta = d120 as f64 / 120.0 * SCROLL_PER_WHEEL_DELTA;
        let smooth = d120 % 120 != 0;
        if self.touchpad && self.fingers > 0 {
            self.last_touch_wheel = Some(now_ms);
            self.scrolled[(axis as usize).min(1)] = true;
            return Some(Axis { axis, delta, discrete120: 0, source: wdp::AXIS_SOURCE_FINGER });
        }
        if self.touchpad && smooth && self.last_touch_wheel.is_some_and(|t| now_ms.wrapping_sub(t) < INERTIA_GAP_MS) {
            self.last_touch_wheel = Some(now_ms); // inertia continues the chain
            return None;
        }
        self.last_touch_wheel = None;
        if smooth {
            Some(Axis { axis, delta, discrete120: 0, source: wdp::AXIS_SOURCE_CONTINUOUS })
        } else {
            Some(Axis { axis, delta, discrete120: d120, source: wdp::AXIS_SOURCE_WHEEL })
        }
    }

    /// The viewer lost focus: the fingers are no longer ours.
    pub fn reset(&mut self) -> Vec<Axis> {
        self.fingers = 0;
        self.last_touch_wheel = None;
        self.stops()
    }
}

// ------------------------------------------------------------------ Win32

pub mod raw {
    use super::*;
    use std::cell::RefCell;
    use std::collections::HashMap;
    use windows::Win32::Devices::HumanInterfaceDevice::*;
    use windows::Win32::Foundation::{HANDLE, HWND, LPARAM};
    use windows::Win32::UI::Input::*;

    const PAGE_GENERIC: u16 = 0x01;
    const PAGE_DIGITIZER: u16 = 0x0D;
    const USAGE_TOUCHPAD: u16 = 0x05;
    const USAGE_X: u16 = 0x30;
    const USAGE_Y: u16 = 0x31;
    const USAGE_TIP: u16 = 0x42;
    const USAGE_CONTACT_ID: u16 = 0x51;
    const USAGE_CONTACT_COUNT: u16 = 0x54;

    struct Range {
        min: i32,
        max: i32,
        mm: f64, // physical length of the logical range
    }

    impl Range {
        fn to_mm(&self, v: u32) -> f64 {
            let span = (self.max - self.min).max(1) as f64;
            (v as i32 - self.min) as f64 / span * self.mm
        }
    }

    struct Finger {
        link: u16,
        x: Range,
        y: Range,
    }

    struct Device {
        preparsed: Vec<u64>, // PHIDP_PREPARSED_DATA points into this (u64 for alignment)
        count_link: u16,
        fingers: Vec<Finger>,
        assembler: Assembler,
    }

    impl Device {
        fn data(&self) -> PHIDP_PREPARSED_DATA {
            PHIDP_PREPARSED_DATA(self.preparsed.as_ptr() as isize)
        }
    }

    thread_local! {
        // None = not a usable precision touchpad (don't ask again)
        static DEVICES: RefCell<HashMap<isize, Option<Device>>> = RefCell::new(HashMap::new());
    }

    /// Ask for the touchpad's reports while one of our windows has the keyboard focus.
    pub fn register() {
        let dev = RAWINPUTDEVICE { usUsagePage: PAGE_DIGITIZER, usUsage: USAGE_TOUCHPAD, dwFlags: RAWINPUTDEVICE_FLAGS(0), hwndTarget: HWND::default() };
        if let Err(e) = unsafe { RegisterRawInputDevices(&[dev], std::mem::size_of::<RAWINPUTDEVICE>() as u32) } {
            eprintln!("[omarchy] no touchpad gestures (cannot register for touchpad input: {e})");
        }
    }

    /// Physical length of a value's logical range, in mm (a typical pad's size when not reported).
    fn length_mm(c: &HIDP_VALUE_CAPS, fallback: f64) -> f64 {
        let mut exp = (c.UnitsExp & 0xF) as i32;
        if exp > 7 {
            exp -= 16;
        }
        let per_unit = match c.Units & 0xFFFF {
            0x11 => 10.0, // SI linear: centimetres
            0x13 => 25.4, // English linear: inches
            _ => return fallback,
        };
        let mm = (c.PhysicalMax - c.PhysicalMin) as f64 * 10f64.powi(exp) * per_unit;
        if (20.0..=400.0).contains(&mm) {
            mm
        } else {
            fallback
        }
    }

    unsafe fn open(h: HANDLE) -> Option<Device> {
        let mut size = 0u32;
        GetRawInputDeviceInfoW(Some(h), RIDI_PREPARSEDDATA, None, &mut size);
        if size == 0 || size > 1 << 20 {
            return None;
        }
        let mut preparsed = vec![0u64; (size as usize).div_ceil(8)];
        if GetRawInputDeviceInfoW(Some(h), RIDI_PREPARSEDDATA, Some(preparsed.as_mut_ptr().cast()), &mut size) == u32::MAX {
            return None;
        }
        let data = PHIDP_PREPARSED_DATA(preparsed.as_ptr() as isize);
        let mut caps = HIDP_CAPS::default();
        if HidP_GetCaps(data, &mut caps) != HIDP_STATUS_SUCCESS {
            return None;
        }
        let mut n = caps.NumberInputValueCaps;
        let mut values = vec![HIDP_VALUE_CAPS::default(); n as usize];
        if n == 0 || HidP_GetValueCaps(HidP_Input, values.as_mut_ptr(), &mut n, data) != HIDP_STATUS_SUCCESS {
            return None;
        }
        values.truncate(n as usize);
        let usage = |c: &HIDP_VALUE_CAPS| if c.IsRange { c.Anonymous.Range.UsageMin } else { c.Anonymous.NotRange.Usage };
        let find = |link: u16, page: u16, u: u16| values.iter().find(|c| c.LinkCollection == link && c.UsagePage == page && usage(c) == u);

        let count_link = values.iter().find(|c| c.UsagePage == PAGE_DIGITIZER && usage(c) == USAGE_CONTACT_COUNT)?.LinkCollection;
        let mut fingers = Vec::new();
        for c in values.iter().filter(|c| c.UsagePage == PAGE_DIGITIZER && usage(c) == USAGE_CONTACT_ID) {
            let (Some(x), Some(y)) = (find(c.LinkCollection, PAGE_GENERIC, USAGE_X), find(c.LinkCollection, PAGE_GENERIC, USAGE_Y)) else { continue };
            let width = length_mm(x, 100.0);
            let height = length_mm(y, width * (y.LogicalMax - y.LogicalMin).max(1) as f64 / (x.LogicalMax - x.LogicalMin).max(1) as f64);
            fingers.push(Finger {
                link: c.LinkCollection,
                x: Range { min: x.LogicalMin, max: x.LogicalMax, mm: width },
                y: Range { min: y.LogicalMin, max: y.LogicalMax, mm: height },
            });
        }
        if fingers.is_empty() {
            return None;
        }
        Some(Device { preparsed, count_link, fingers, assembler: Assembler::default() })
    }

    /// The contacts of one report, in report order.
    unsafe fn slots(dev: &Device, report: &mut [u8]) -> (u32, Vec<Slot>) {
        let data = dev.data();
        let mut count = 0u32;
        let _ = HidP_GetUsageValue(HidP_Input, PAGE_DIGITIZER, Some(dev.count_link), USAGE_CONTACT_COUNT, &mut count, data, report);
        let mut out = Vec::with_capacity(dev.fingers.len());
        for f in &dev.fingers {
            let (mut id, mut x, mut y) = (0u32, 0u32, 0u32);
            if HidP_GetUsageValue(HidP_Input, PAGE_DIGITIZER, Some(f.link), USAGE_CONTACT_ID, &mut id, data, report) != HIDP_STATUS_SUCCESS
                || HidP_GetUsageValue(HidP_Input, PAGE_GENERIC, Some(f.link), USAGE_X, &mut x, data, report) != HIDP_STATUS_SUCCESS
                || HidP_GetUsageValue(HidP_Input, PAGE_GENERIC, Some(f.link), USAGE_Y, &mut y, data, report) != HIDP_STATUS_SUCCESS
            {
                continue;
            }
            let mut usages = [0u16; 16];
            let mut n = usages.len() as u32;
            let touching = HidP_GetUsages(HidP_Input, PAGE_DIGITIZER, Some(f.link), usages.as_mut_ptr(), &mut n, data, report) == HIDP_STATUS_SUCCESS
                && usages[..n as usize].contains(&USAGE_TIP);
            out.push(Slot { contact: Contact { id, x: f.x.to_mm(x), y: f.y.to_mm(y) }, touching });
        }
        (count, out)
    }

    /// WM_INPUT: the touching contacts of every scan the message completes.
    pub fn read(lparam: LPARAM) -> Vec<Vec<Contact>> {
        let mut scans = Vec::new();
        unsafe {
            let h = HRAWINPUT(lparam.0 as *mut _);
            let header = std::mem::size_of::<RAWINPUTHEADER>() as u32;
            let mut size = 0u32;
            if GetRawInputData(h, RID_INPUT, None, &mut size, header) != 0 || size < header + 8 {
                return scans;
            }
            let mut buf = vec![0u64; (size as usize).div_ceil(8)];
            if GetRawInputData(h, RID_INPUT, Some(buf.as_mut_ptr().cast()), &mut size, header) != size {
                return scans;
            }
            let bytes = std::slice::from_raw_parts_mut(buf.as_mut_ptr().cast::<u8>(), size as usize);
            let hdr = &*(bytes.as_ptr() as *const RAWINPUTHEADER);
            if hdr.dwType != RIM_TYPEHID.0 {
                return scans;
            }
            let device = hdr.hDevice;
            // RAWHID: dwSizeHid, dwCount, then dwCount reports of dwSizeHid bytes
            let at = header as usize;
            let report_size = u32::from_le_bytes(bytes[at..at + 4].try_into().unwrap()) as usize;
            let reports = u32::from_le_bytes(bytes[at + 4..at + 8].try_into().unwrap()) as usize;
            let first = at + 8;
            if report_size == 0 || first + report_size * reports > bytes.len() {
                return scans;
            }
            DEVICES.with(|d| {
                let mut d = d.borrow_mut();
                let Some(dev) = d.entry(device.0 as isize).or_insert_with(|| open(device)) else { return };
                for r in 0..reports {
                    let report = &mut bytes[first + r * report_size..first + (r + 1) * report_size];
                    let (count, slots) = slots(dev, report);
                    if let Some(scan) = dev.assembler.report(count, &slots) {
                        scans.push(scan);
                    }
                }
            });
        }
        scans
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn c(id: u32, x: f64, y: f64) -> Contact {
        Contact { id, x, y }
    }

    fn slot(id: u32, x: f64, touching: bool) -> Slot {
        Slot { contact: c(id, x, 0.0), touching }
    }

    #[test]
    fn assembler_parallel_and_hybrid_reports() {
        let mut a = Assembler::default();
        // parallel: one report, 5 slots, 2 contacts; unused slots are ignored
        let s = [slot(1, 1.0, true), slot(2, 2.0, true), slot(0, 0.0, false), slot(0, 0.0, false), slot(0, 0.0, false)];
        assert_eq!(a.report(2, &s), Some(vec![c(1, 1.0, 0.0), c(2, 2.0, 0.0)]));
        // hybrid: 3 contacts, one per report; continuation reports carry count 0
        assert_eq!(a.report(3, &[slot(1, 1.0, true)]), None);
        assert_eq!(a.report(0, &[slot(2, 2.0, true)]), None);
        assert_eq!(a.report(0, &[slot(3, 3.0, false)]), Some(vec![c(1, 1.0, 0.0), c(2, 2.0, 0.0)]));
        // a stray continuation report without a scan is ignored
        assert_eq!(a.report(0, &[slot(1, 1.0, true)]), None);
    }

    fn three(x: f64) -> Vec<Contact> {
        vec![c(1, x, 10.0), c(2, x + 10.0, 10.0), c(3, x + 20.0, 10.0)]
    }

    #[test]
    fn swipe_begins_after_travel_and_ends_on_lift() {
        let mut s = Swipes::default();
        assert!(s.scan(&three(10.0)).is_empty());
        assert!(s.scan(&three(11.0)).is_empty()); // 1 mm: not yet
        let ev = s.scan(&three(12.5));
        assert_eq!(ev[0], SwipeEvent::Begin(3));
        assert!(matches!(ev[1], SwipeEvent::Update(3, dx, dy) if (dx - 2.5 * SWIPE_UNITS_PER_MM).abs() < 1e-9 && dy == 0.0));
        assert!(matches!(s.scan(&three(10.5))[..], [SwipeEvent::Update(3, dx, _)] if dx < 0.0));
        // one finger lifts first: the swipe ends normally, and 2 fingers don't start anything
        assert_eq!(s.scan(&three(10.5)[..2]), vec![SwipeEvent::End { cancelled: false }]);
        assert!(s.scan(&[]).is_empty());
    }

    #[test]
    fn swipe_cancelled_by_extra_finger_and_by_reset() {
        let mut s = Swipes::default();
        s.scan(&three(10.0));
        s.scan(&three(15.0));
        let mut four = three(15.0);
        four.push(c(4, 50.0, 10.0));
        assert_eq!(s.scan(&four), vec![SwipeEvent::End { cancelled: true }]);
        s.scan(&three(10.0));
        s.scan(&three(15.0));
        assert_eq!(s.reset(), Some(SwipeEvent::End { cancelled: true }));
        assert_eq!(s.reset(), None);
    }

    #[test]
    fn two_fingers_never_swipe() {
        let mut s = Swipes::default();
        for x in 0..20 {
            assert!(s.scan(&three(x as f64 * 3.0)[..2]).is_empty());
        }
    }

    #[test]
    fn scroll_without_touchpad_reports() {
        let mut s = Scroll::default();
        let w = s.wheel(0, 0, 120).unwrap();
        assert_eq!((w.source, w.discrete120, w.delta), (wdp::AXIS_SOURCE_WHEEL, 120, 15.0));
        let w = s.wheel(1, 0, -7).unwrap();
        assert_eq!((w.source, w.discrete120), (wdp::AXIS_SOURCE_CONTINUOUS, 0));
    }

    #[test]
    fn scroll_with_fingers_then_stop_then_inertia_dropped() {
        let mut s = Scroll::default();
        assert!(s.fingers(2).is_empty());
        let w = s.wheel(0, 0, 18).unwrap();
        assert_eq!((w.source, w.discrete120, w.delta), (wdp::AXIS_SOURCE_FINGER, 0, 18.0 / 8.0));
        // the backward jitter is passed on as the small step it is
        assert_eq!(s.wheel(10, 0, -3).unwrap().delta, -3.0 / 8.0);
        let stops = s.fingers(0);
        assert_eq!(stops, vec![Axis { axis: 0, delta: 0.0, discrete120: 0, source: wdp::AXIS_SOURCE_FINGER }]);
        // Windows' inertia after the lift
        assert_eq!(s.wheel(50, 0, 12), None);
        assert_eq!(s.wheel(200, 0, 6), None);
        // a real wheel click later
        assert_eq!(s.wheel(2000, 0, 120).unwrap().source, wdp::AXIS_SOURCE_WHEEL);
        // no stop without scrolling
        s.fingers(1);
        assert!(s.fingers(0).is_empty());
    }

    #[test]
    fn scroll_stop_when_a_third_finger_lands_and_on_reset() {
        let mut s = Scroll::default();
        s.fingers(2);
        s.wheel(0, 1, 30);
        assert_eq!(s.fingers(3).len(), 1);
        s.fingers(2);
        s.wheel(0, 0, 30);
        assert_eq!(s.reset().len(), 1);
    }
}
