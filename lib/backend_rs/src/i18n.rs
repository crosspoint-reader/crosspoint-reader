//! Translations.
//!
//! Strings are looked up by their `STR_*` key name. The numeric ids the C++
//! side uses are positional — inserting a key in the middle of the YAML
//! renumbers everything after it — so a Rust mirror of those ids would
//! silently mistranslate. Key names are stable; ids are not.

use super::{borrow_cstr, raw};
use core::ffi::CStr;

/// Translates a `STR_*` key into the active language.
///
/// Unknown keys come back as the key itself, so a typo is visible on screen
/// instead of blank or crashing. Pass the key as a C string literal:
///
/// ```rust,ignore
/// Text::new(tr(c"STR_FRONTLIGHT"))
/// ```
pub fn tr(key: &CStr) -> &'static str {
    unsafe { borrow_cstr(raw::cpp_tr(key.as_ptr())) }
}
