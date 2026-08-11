//! What the UI needs from whatever hosts it.
//!
//! `xpui` never talks to the firmware. It declares this contract and a host
//! installs an implementation once at startup, so the framework can be built,
//! tested and reasoned about without a device — and so nothing in the UI can
//! reach for the C boundary by accident.
//!
//! ```rust,ignore
//! xpui::host::install(&FIRMWARE);   // once, as the screen is entered
//! ```

mod canvas;
mod chrome;
mod clock;
mod input;
mod metrics;

pub use canvas::{Canvas, IconRef, Renderer};
pub use chrome::{
    finish_screen, request_update, Chrome, Hint, RowField, ScreenChrome, Theme, ThemeMetric,
};
pub use clock::{millis, Clock};
pub use input::{Button, Input, InputSource, SwipeDir};
pub use metrics::{Font, FontId, FontRole, FontStyle, TextMetrics};

/// Everything a host must provide. One object implements all five, so a screen
/// installs a single value and the framework keeps one pointer.
pub trait Host: Canvas + TextMetrics + Chrome + InputSource + Clock + Sync {}

impl<T> Host for T where T: Canvas + TextMetrics + Chrome + InputSource + Clock + Sync {}

/// The installed host.
///
/// Written once before the first frame and only read afterwards. A plain static
/// rather than a lock: the two callers are separate FreeRTOS tasks, but neither
/// writes, and taking a lock on every text measurement would cost more than the
/// whole layout pass.
static mut HOST: Option<&'static dyn Host> = None;

/// Installs the host. Call once, before any view is measured or drawn.
///
/// # Safety
/// Must be called before the first `measure`/`render`/`interactions`, and never
/// concurrently with them. In practice that means from the activity's entry
/// point, on the main task, before the render task is started.
pub unsafe fn install(host: &'static dyn Host) {
    HOST = Some(host);
}

/// The installed host, for the framework's own use.
///
/// # Panics
/// If nothing was installed. That is a wiring mistake, not a runtime condition:
/// a screen cannot be measured before its host exists.
pub(crate) fn current() -> &'static dyn Host {
    // Safety: written once by `install` before any reader exists.
    if let Some(host) = unsafe { HOST } {
        return host;
    }

    // In a test build, fall back to the fake rather than making every test
    // remember to install before it constructs its first widget. Widgets
    // resolve fonts in their constructors, so the ordering trap is easy to hit
    // and the failure looks nothing like its cause.
    #[cfg(any(test, feature = "testing"))]
    {
        crate::testing::install();
        if let Some(host) = unsafe { HOST } {
            return host;
        }
    }

    panic!("xpui::host::install was never called")
}

/// Whether a host has been installed, so tests can assert wiring.
pub fn is_installed() -> bool {
    unsafe { HOST }.is_some()
}
