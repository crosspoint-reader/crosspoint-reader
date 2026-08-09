//! Strings for the widgets the theme paints itself.
//!
//! The C side asks for each cell through a callback, and a callback handed a
//! borrowed `&str` cannot produce a NUL-terminated pointer. So every cell is
//! converted once before the call and the trampolines just index this — which
//! also means one conversion per string per frame rather than one per callback.

use alloc::ffi::CString;
use alloc::vec::Vec;
use core::ffi::{c_char, c_void};

use cpui::host::RowField;

use crate::firmware::c;

pub(crate) struct Cells {
    cells: Vec<Option<CString>>,
    stride: usize,
}

impl Cells {
    /// Every field of every row, in the order the theme asks for them.
    pub(crate) fn rows<'a>(rows: usize, row: &dyn Fn(usize, RowField) -> Option<&'a str>) -> Self {
        const FIELDS: [RowField; 3] = [RowField::Title, RowField::Subtitle, RowField::Value];
        let mut cells = Vec::with_capacity(rows * FIELDS.len());
        for index in 0..rows {
            for field in FIELDS {
                cells.push(row(index, field).map(c));
            }
        }
        Cells {
            cells,
            stride: FIELDS.len(),
        }
    }

    /// One label per option.
    pub(crate) fn options<'a>(count: usize, option: &dyn Fn(usize) -> Option<&'a str>) -> Self {
        let mut cells = Vec::with_capacity(count);
        for index in 0..count {
            cells.push(option(index).map(c));
        }
        Cells { cells, stride: 1 }
    }

    /// The context pointer the trampolines are handed. Valid only for the
    /// duration of the call this is passed to.
    pub(crate) fn as_ctx(&self) -> *mut c_void {
        self as *const Cells as *mut c_void
    }

    /// A borrowed C string, or null. Shared so every "optional text" argument
    /// crossing the boundary is spelled the same way.
    pub(crate) fn ptr(text: &Option<CString>) -> *const c_char {
        text.as_ref()
            .map_or(core::ptr::null(), |t| t.as_ptr().cast())
    }

    fn get(&self, index: usize, field: usize) -> *const c_char {
        self.cells
            .get(index * self.stride + field)
            .and_then(Option::as_ref)
            .map_or(core::ptr::null(), |text| text.as_ptr())
    }
}

/// Trampoline for `draw_list`. `ctx` is a [`Cells`] that outlives the call.
pub(crate) extern "C" fn row_cell(ctx: *mut c_void, index: i32, field: i32) -> *const c_char {
    if ctx.is_null() || index < 0 || field < 0 {
        return core::ptr::null();
    }
    // Safety: only called during a draw, where ctx borrows a live Cells.
    let cells = unsafe { &*(ctx as *const Cells) };
    cells.get(index as usize, field as usize)
}

/// Trampoline for the option popup.
pub(crate) extern "C" fn option_cell(ctx: *mut c_void, index: i32) -> *const c_char {
    if ctx.is_null() || index < 0 {
        return core::ptr::null();
    }
    let cells = unsafe { &*(ctx as *const Cells) };
    cells.get(index as usize, 0)
}
