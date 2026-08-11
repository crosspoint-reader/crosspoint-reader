//! Every `extern "C"` declaration, in one place.
//!
//! Each symbol is implemented in `src/activities/RustActivityStubs.cpp`. Keep
//! the two in step: a mismatch here is a link error at best and a corrupt call
//! frame at worst.
//!
//! Nothing outside `ffi` should call these directly — the sibling modules wrap
//! them in safe APIs.

use core::ffi::{c_char, c_void};

extern "C" {
    // -- renderer -----------------------------------------------------------
    pub fn cpp_screen_width() -> i32;
    pub fn cpp_screen_height() -> i32;
    pub fn cpp_clear_screen();
    pub fn cpp_draw_text(x: i32, y: i32, text: *const c_char, font_id: i32, style: u8);
    pub fn cpp_draw_rect(x: i32, y: i32, width: i32, height: i32, filled: u8, black: u8);
    pub fn cpp_draw_line(x1: i32, y1: i32, x2: i32, y2: i32);
    /// Fills with a 50% dither, which reads as grey on a 1-bit panel. The
    /// slider track needs it; a solid fill would be indistinguishable from the
    /// filled portion.
    pub fn cpp_fill_rect_dither(x: i32, y: i32, width: i32, height: i32, light: u8);
    /// Darkens a region without erasing it: ink on one checkerboard parity
    /// only, so roughly half of what was behind survives and the whole reads as
    /// grey. Distinct from `cpp_fill_rect_dither`, which clears first.
    pub fn cpp_scrim(x: i32, y: i32, width: i32, height: i32);
    /// Draws a 1-bpp bitmap. Format: row-major, MSB first, `(w + 7) / 8` bytes
    /// per row, and **bit 0 is ink** - inverted from the usual convention.
    pub fn cpp_draw_image(bitmap: *const u8, x: i32, y: i32, width: i32, height: i32);
    /// Draws an icon by role. Transparent and orientation-correct; the asset
    /// stays in the host's flash and the host chooses which one a role means.
    pub fn cpp_draw_icon(role: u8, filled: u8, size: i32, x: i32, y: i32);
    /// Edge length the host would actually draw for this role, or 0 when it
    /// ships nothing for it.
    pub fn cpp_icon_size(role: u8, filled: u8, size: i32) -> i32;

    // -- fonts --------------------------------------------------------------
    /// Resolves a family+size to the firmware's font id, or 0 when that font
    /// was compiled out (e.g. the `slim` build defines OMIT_FONTS).
    pub fn cpp_resolve_font(family: u8, size_pt: u8) -> i32;
    /// The reader font the user selected, including SD-card fonts.
    pub fn cpp_reader_font() -> i32;
    pub fn cpp_text_width(font_id: i32, text: *const c_char, style: u8) -> i32;
    pub fn cpp_line_height(font_id: i32) -> i32;

    // -- input --------------------------------------------------------------
    pub fn cpp_input_was_pressed(button: u8) -> u8;
    pub fn cpp_input_is_pressed(button: u8) -> u8;
    pub fn cpp_input_was_released(button: u8) -> u8;
    pub fn cpp_input_has_touch() -> u8;
    pub fn cpp_input_was_tapped(x: *mut i32, y: *mut i32) -> u8;
    /// True while a touch is down, reporting its current position - the drag
    /// signal a slider needs.
    pub fn cpp_input_touch_held(x: *mut i32, y: *mut i32) -> u8;
    pub fn cpp_input_touch_released() -> u8;
    pub fn cpp_input_swipe() -> u8;
    pub fn cpp_input_was_back_gesture() -> u8;
    pub fn cpp_input_was_home_gesture() -> u8;
    /// Milliseconds since boot. The only clock Rust has; the framework needs it
    /// for key auto-repeat, which ButtonNavigator does with millis() in C++.
    pub fn cpp_millis() -> u32;

    // -- theme --------------------------------------------------------------
    pub fn cpp_theme_metric(metric: u8) -> i32;
    pub fn cpp_theme_draw_header(title: *const c_char, subtitle: *const c_char);
    pub fn cpp_theme_draw_sub_header(
        x: i32,
        y: i32,
        width: i32,
        height: i32,
        label: *const c_char,
        right_label: *const c_char,
    );
    pub fn cpp_theme_draw_button_hints(
        back: *const c_char,
        confirm: *const c_char,
        previous: *const c_char,
        next: *const c_char,
    );
    pub fn cpp_theme_draw_progress_bar(
        x: i32,
        y: i32,
        width: i32,
        height: i32,
        current: u32,
        total: u32,
    );
    /// Draws the themed slider. The firmware owns the track, fill and knob.
    pub fn cpp_theme_draw_slider(x: i32, y: i32, width: i32, height: i32, value: i32, max: i32);
    /// Draws the themed modal. `option_text` is called back per row.
    pub fn cpp_theme_draw_option_popup(
        title: *const c_char,
        option_text: extern "C" fn(ctx: *mut c_void, index: i32) -> *const c_char,
        ctx: *mut c_void,
        count: i32,
        selected: i32,
    );
    /// Rect of one modal row, so hit-testing does not have to re-derive the
    /// dialog geometry. C++ owns that math; it is already duplicated between
    /// OptionPopup::getLayout and BaseTheme::drawOptionPopup and must not
    /// become a third copy.
    pub fn cpp_option_popup_row_rect(
        title: *const c_char,
        option_text: extern "C" fn(ctx: *mut c_void, index: i32) -> *const c_char,
        ctx: *mut c_void,
        count: i32,
        index: i32,
        out_xywh: *mut i32,
    ) -> u8;
    /// Draws the themed list. `row_text` is called back for each visible row;
    /// `field` selects title (0), subtitle (1) or value (2), and returning null
    /// omits that field.
    pub fn cpp_theme_draw_list(
        x: i32,
        y: i32,
        width: i32,
        height: i32,
        item_count: i32,
        selected_index: i32,
        row_text: extern "C" fn(ctx: *mut c_void, index: i32, field: i32) -> *const c_char,
        ctx: *mut c_void,
    );

    // -- device -------------------------------------------------------------
    pub fn cpp_device_firmware_version() -> *const c_char;
    pub fn cpp_device_name() -> *const c_char;
    pub fn cpp_device_battery_percent() -> i32;

    // -- frontlight ---------------------------------------------------------
    pub fn cpp_frontlight_present() -> u8;
    pub fn cpp_frontlight_has_color_temperature() -> u8;
    pub fn cpp_frontlight_brightness() -> i32;
    pub fn cpp_frontlight_warmth() -> i32;
    pub fn cpp_frontlight_is_on() -> u8;
    pub fn cpp_frontlight_set_brightness(percent: i32);
    pub fn cpp_frontlight_set_warmth(percent: i32);
    pub fn cpp_frontlight_set_on(on: u8);
    /// Persists brightness, warmth and on-state in one write, and only when a
    /// value actually changed. Throttled on the C++ side so a caller cannot
    /// wear out the flash by saving per interaction.
    pub fn cpp_frontlight_save();

    // -- developer settings -------------------------------------------------
    // Narrow on purpose: a general settings writer would hand every screen the
    // ability to write flash, and the write-throttling rule would then have to
    // be respected by each caller instead of enforced once.

    // -- display ------------------------------------------------------------
    pub fn cpp_display_is_inverted() -> u8;
    /// Flips inversion, persists it, and returns the new state.
    pub fn cpp_display_toggle_inverted() -> u8;

    // -- heap ---------------------------------------------------------------
    // Rust allocates through the firmware heap, so its cost is invisible to any
    // static measurement. These are the only way to see it.
    pub fn cpp_heap_free() -> i32;
    pub fn cpp_heap_largest_block() -> i32;
    pub fn cpp_heap_min_free() -> i32;

    // -- translations -------------------------------------------------------
    /// Looks a `STR_*` key up in the generated table. Returns the key itself
    /// when unknown, so a typo shows up on screen rather than crashing.
    pub fn cpp_tr(key: *const c_char) -> *const c_char;

    // -- activity -----------------------------------------------------------
    pub fn cpp_activity_title() -> *const c_char;
    pub fn cpp_activity_finish();
    pub fn cpp_activity_request_update();
}
