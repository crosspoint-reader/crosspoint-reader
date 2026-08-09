//! The firmware's services, as Rust sees them.
//!
//! Everything crossing the C++ boundary lives in this crate: the `extern "C"`
//! declarations, the screen lifecycle the firmware calls into, the allocator
//! and panic handler, translations, and the icon roles the product ships.
//!
//! `cpui` depends on none of it. It declares the ports it needs and this crate
//! implements them, so the UI never learns where its data comes from.
//!
//! This is the only crate containing `unsafe`.

#![cfg_attr(target_os = "none", no_std)]

extern crate alloc;

use alloc::string::String;
use core::ffi::{c_char, CStr};

pub use firmware::{Firmware, FIRMWARE};

#[cfg(any(test, feature = "testing"))]
pub mod testing;

/// Translation keys, generated from the YAML by `build.rs`.
pub mod strings {
    include!(concat!(env!("OUT_DIR"), "/strings.rs"));
}

#[cfg(target_os = "none")]
mod runtime;

mod cells;
mod firmware;
mod icon;
mod raw;

// The five `cpui::host` traits, each implemented beside the concern it serves.
mod font;
mod input;
mod renderer;
mod theme;

pub mod device;
pub mod frontlight;
pub mod i18n;
pub mod lifecycle;

pub use frontlight::{Display, Frontlight};
pub use icon::{IconRole, IconSpec};

/// Borrows a C string the firmware owns.
///
/// Returns `""` for null. Safe to treat as `'static`: every pointer handed
/// across this boundary points into a compiled-in table (translations, board
/// profile names, version literals) that lives for the whole program. Callers
/// must not retain the result across a language change, since the *content*
/// shown would be stale even though the pointer stays valid.
///
/// # Safety
/// `ptr` must be null or a valid NUL-terminated C string.
pub(crate) unsafe fn borrow_cstr(ptr: *const c_char) -> &'static str {
    if ptr.is_null() {
        return "";
    }
    CStr::from_ptr(ptr).to_str().unwrap_or("")
}

/// Copies a C string the firmware owns into an allocation Rust controls.
///
/// # Safety
/// `ptr` must be null or a valid NUL-terminated C string.
#[allow(dead_code)]
pub(crate) unsafe fn copy_cstr(ptr: *const c_char) -> String {
    String::from(borrow_cstr(ptr))
}

/// Milliseconds since boot.
///
/// The only clock Rust has. The runtime needs it for key auto-repeat, which
/// `ButtonNavigator` does with `millis()` on the C++ side.
pub fn millis() -> u32 {
    unsafe { raw::cpp_millis() }
}

/// Translates a key from `lib/I18n/translations/english.yaml`.
///
/// ```rust,ignore
/// Text::new(tr!(STR_FRONTLIGHT))
/// ```
///
/// Reads like the C++ `tr(STR_FRONTLIGHT)` on purpose. An unknown key is a compile
/// error because the constants are generated from the YAML, and a key used in
/// source but absent from the YAML fails `scripts/gen_i18n.py`.
///
/// The key name appears literally at the call site, which is what lets the
/// generator's usage scan see it and keep the string out of the "unused" set.
#[macro_export]
macro_rules! tr {
    ($key:ident) => {
        $crate::i18n::tr($crate::strings::$key)
    };
}
