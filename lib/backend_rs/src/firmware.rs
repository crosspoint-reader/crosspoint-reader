//! The value the firmware installs as `xpui`'s host.
//!
//! The trait implementations live beside the concern they serve —
//! [`Canvas`](xpui::host::Canvas) in `renderer`, [`TextMetrics`] in `font`,
//! [`InputSource`] in `input`, [`Chrome`] in `theme` — so each file stays one
//! job. Only the shared pieces are here.

use alloc::ffi::CString;

use xpui::host::Clock;

use crate::raw;

/// CrossPoint's host. Stateless: everything it needs is reachable through the
/// globals `ActivityRs` binds for the running activity.
pub struct Firmware;

/// The installed instance. `install` takes a `&'static` and this has no state
/// to configure, so one shared value is enough.
pub static FIRMWARE: Firmware = Firmware;

/// Borrows a `&str` as a C string for one call.
///
/// The C side copies or draws immediately, so a temporary is safe. An interior
/// NUL cannot come from our own widgets, and truncating beats refusing to draw.
pub(crate) fn c(text: &str) -> CString {
    CString::new(text).unwrap_or_default()
}

impl Clock for Firmware {
    fn millis(&self) -> u32 {
        raw::cpp_millis()
    }
}

// The blanket impl in xpui makes `Firmware` a `Host` once all five are in
// place; this asserts it here rather than at the install site.
const _: fn() = || {
    fn assert_host<T: xpui::host::Host>() {}
    assert_host::<Firmware>();
};
