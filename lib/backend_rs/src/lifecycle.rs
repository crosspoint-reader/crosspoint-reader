//! The lifecycle the firmware calls into.
//!
//! `ActivityRs` owns a screen as an opaque handle and drives it through these
//! entry points. They live here rather than in `cpui` because they are the C
//! boundary, and because entering a screen is where the host gets installed.

use alloc::boxed::Box;
use core::ffi::c_void;

use cpui::screen::{Driver, Runtime, Screen};

use crate::FIRMWARE;

/// Installs the host, once, before any screen runs.
///
/// Done here rather than from C++ because `install` takes a Rust trait object.
/// Idempotent, so entering a second screen is free — and doing it on entry
/// rather than at static-init time means it cannot race the render task, which
/// does not exist yet.
fn ensure_host_installed() {
    if !cpui::host::is_installed() {
        // Safety: called from onEnter on the main task, before the screen is
        // measured or drawn, and never concurrently with a reader.
        unsafe { cpui::host::install(&FIRMWARE) };
    }
}

/// Wraps a screen for C++ to own, returning the opaque handle.
///
/// Boxed twice: `dyn Driver` is a fat pointer, so the inner box erases the type
/// and the outer gives a thin pointer that can cross the FFI.
pub fn into_handle<S: Screen + 'static>(screen: S) -> *mut c_void {
    let erased: Box<dyn Driver> = Box::new(Runtime::new(screen));
    Box::into_raw(Box::new(erased)) as *mut c_void
}

/// # Safety
/// `handle` must be null or a pointer from [`into_handle`] that is still live.
unsafe fn with(handle: *mut c_void, body: impl FnOnce(&mut dyn Driver)) {
    if handle.is_null() {
        return;
    }
    body((*(handle as *mut Box<dyn Driver>)).as_mut());
}

/// # Safety
/// `handle` must be null or a live handle from the screen's factory.
#[no_mangle]
pub unsafe extern "C" fn rust_activity_on_enter(handle: *mut c_void) {
    ensure_host_installed();
    with(handle, |driver| driver.on_enter())
}

/// # Safety
/// `handle` must be null or a live handle from the screen's factory.
#[no_mangle]
pub unsafe extern "C" fn rust_activity_loop(handle: *mut c_void) {
    with(handle, |driver| driver.loop_())
}

/// # Safety
/// `handle` must be null or a live handle from the screen's factory.
#[no_mangle]
pub unsafe extern "C" fn rust_activity_on_exit(handle: *mut c_void) {
    with(handle, |driver| driver.on_exit())
}

/// The renderer is reached through the globals `ActivityRs` binds, so the
/// pointer argument is unused.
///
/// # Safety
/// `handle` must be null or a live handle from the screen's factory.
#[no_mangle]
pub unsafe extern "C" fn rust_activity_render(handle: *mut c_void, _renderer: *const u8) {
    // Also installed here: a screen can be rendered before its onEnter has run
    // if the render task wakes first.
    ensure_host_installed();
    with(handle, |driver| driver.render())
}

/// # Safety
/// `handle` must be null or a live handle from the screen's factory.
#[no_mangle]
pub unsafe extern "C" fn rust_activity_home_gesture(handle: *mut c_void) -> u8 {
    if handle.is_null() {
        return 0;
    }
    let driver = &mut *(handle as *mut Box<dyn Driver>);
    u8::from(driver.handle_home_gesture())
}

/// # Safety
/// `handle` must be null, or a handle from the factory not already destroyed.
#[no_mangle]
pub unsafe extern "C" fn destroy_rust_activity(handle: *mut c_void) {
    if handle.is_null() {
        return;
    }
    drop(Box::from_raw(handle as *mut Box<dyn Driver>))
}

/// Exports a C factory for a screen.
///
/// ```rust,ignore
/// register_screen!(AboutScreen, create_about_activity);
/// ```
#[macro_export]
macro_rules! register_screen {
    ($screen:ty, $factory:ident) => {
        #[no_mangle]
        pub extern "C" fn $factory() -> *mut core::ffi::c_void {
            $crate::lifecycle::into_handle(<$screen>::new())
        }
    };
}
