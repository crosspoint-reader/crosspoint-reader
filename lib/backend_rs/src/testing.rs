//! Host-side definitions of the C symbols this crate declares.
//!
//! `backend` is the only crate that names the C boundary, so it is the only one
//! that has to satisfy it when there is no firmware to link against. Enabled by
//! the `testing` feature; a device build never sees this.
//!
//! Layout is not exercised here — `xpui::testing` covers that with a fake host.
//! These exist purely so a test binary links, and to give the device-info
//! screens something recognisable to assert on.

use core::ffi::{c_char, c_void};

/// Values the device screens are expected to render, so a test can assert on
/// them without guessing.
const VERSION: &core::ffi::CStr = c"test-version";
const DEVICE: &core::ffi::CStr = c"test_device";
const BATTERY_PERCENT: i32 = 72;

macro_rules! stub {
    ($($name:ident($($arg:ident: $ty:ty),*) $(-> $ret:ty)?;)*) => {
        $(
            #[no_mangle]
            extern "C" fn $name($(_: $ty),*) $(-> $ret)? { Default::default() }
        )*
    };
}

stub! {
    cpp_screen_width() -> i32;
    cpp_screen_height() -> i32;
    cpp_clear_screen();
    cpp_draw_text(x: i32, y: i32, t: *const c_char, f: i32, s: u8);
    cpp_draw_rect(x: i32, y: i32, w: i32, h: i32, f: u8, b: u8);
    cpp_draw_line(a: i32, b: i32, c: i32, d: i32);
    cpp_fill_rect_dither(x: i32, y: i32, w: i32, h: i32, l: u8);
    cpp_draw_image(d: *const u8, x: i32, y: i32, w: i32, h: i32);
    cpp_set_clip(x: i32, y: i32, w: i32, h: i32);
    cpp_draw_icon(r: u8, v: u8, s: i32, x: i32, y: i32);
    cpp_icon_size(r: u8, v: u8, s: i32) -> i32;
    cpp_resolve_font(f: u8, s: u8) -> i32;
    cpp_reader_font() -> i32;
    cpp_text_width(f: i32, t: *const c_char, s: u8) -> i32;
    cpp_line_height(f: i32) -> i32;
    cpp_input_was_pressed(b: u8) -> u8;
    cpp_input_is_pressed(b: u8) -> u8;
    cpp_input_was_released(b: u8) -> u8;
    cpp_input_has_touch() -> u8;
    cpp_input_was_tapped(x: *mut i32, y: *mut i32) -> u8;
    cpp_input_touch_held(x: *mut i32, y: *mut i32) -> u8;
    cpp_input_touch_released() -> u8;
    cpp_input_swipe() -> u8;
    cpp_input_was_back_gesture() -> u8;
    cpp_input_was_home_gesture() -> u8;
    cpp_millis() -> u32;
    cpp_theme_metric(m: u8) -> i32;
    cpp_theme_draw_header(t: *const c_char, s: *const c_char);
    cpp_theme_draw_sub_header(x: i32, y: i32, w: i32, h: i32, l: *const c_char, r: *const c_char);
    cpp_theme_draw_button_hints(a: *const c_char, b: *const c_char, c: *const c_char, d: *const c_char);
    cpp_theme_draw_progress_bar(x: i32, y: i32, w: i32, h: i32, c: u32, t: u32);
    cpp_theme_draw_slider(x: i32, y: i32, w: i32, h: i32, v: i32, m: i32);
    cpp_theme_draw_scroll_indicator(x: i32, y: i32, w: i32, h: i32, c: i32, v: i32, o: i32);
    cpp_activity_finish();
    cpp_activity_request_update();
    cpp_heap_free() -> i32;
    cpp_heap_largest_block() -> i32;
    cpp_heap_min_free() -> i32;
}

// The rest return something a test can recognise.

#[no_mangle]
extern "C" fn cpp_device_firmware_version() -> *const u8 {
    VERSION.as_ptr().cast()
}

#[no_mangle]
extern "C" fn cpp_device_name() -> *const u8 {
    DEVICE.as_ptr().cast()
}

#[no_mangle]
extern "C" fn cpp_device_battery_percent() -> i32 {
    BATTERY_PERCENT
}

/// Echoes the key back, which is what the firmware does for an unknown one —
/// so a missing translation is visible rather than blank.
#[no_mangle]
extern "C" fn cpp_tr(key: *const u8) -> *const u8 {
    if key.is_null() {
        return c"".as_ptr().cast();
    }
    key
}

#[no_mangle]
extern "C" fn cpp_activity_title() -> *const u8 {
    c"Test".as_ptr().cast()
}

#[no_mangle]
extern "C" fn cpp_theme_draw_list(
    _x: i32,
    _y: i32,
    _w: i32,
    _h: i32,
    _count: i32,
    _selected: i32,
    _row: extern "C" fn(*mut c_void, i32, i32) -> *const c_char,
    _ctx: *mut c_void,
) {
}

#[no_mangle]
extern "C" fn cpp_theme_draw_option_popup(
    _title: *const c_char,
    _option: extern "C" fn(*mut c_void, i32) -> *const c_char,
    _ctx: *mut c_void,
    _count: i32,
    _selected: i32,
) {
}

#[no_mangle]
extern "C" fn cpp_option_popup_row_rect(
    _title: *const c_char,
    _option: extern "C" fn(*mut c_void, i32) -> *const c_char,
    _ctx: *mut c_void,
    _count: i32,
    _index: i32,
    _out: *mut i32,
) -> u8 {
    0
}
