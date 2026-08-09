//! Device and firmware facts.
//!
//! Every value comes from the host firmware rather than being duplicated here.
//! The version in particular is a compile-time macro that Rust cannot see, so
//! restating it in Rust guarantees it will eventually disagree.

use super::{borrow_cstr, raw};

/// The running firmware version string, as the build defined it.
pub fn firmware_version() -> &'static str {
    unsafe { borrow_cstr(raw::cpp_device_firmware_version()) }
}

/// The board's profile name, identifying the exact hardware variant.
pub fn name() -> &'static str {
    unsafe { borrow_cstr(raw::cpp_device_name()) }
}

/// Battery charge, 0-100. The firmware caches this and repolls at most every
/// 1.5 s, so calling it per frame is cheap.
pub fn battery_percent() -> i32 {
    unsafe { raw::cpp_device_battery_percent() }.clamp(0, 100)
}

/// A snapshot of the firmware heap.
///
/// Rust and C++ share one allocator, so these cover both. This is the only
/// visibility there is into what Rust costs at run time — the build-time size
/// report measures static sections, which Rust barely uses.
#[derive(Copy, Clone, Debug, Default, PartialEq, Eq)]
pub struct Heap {
    /// Bytes currently free across the heap.
    pub free: i32,
    /// Largest single allocation that would currently succeed.
    ///
    /// Watch this one. Fragmentation, not total usage, is what kills a device
    /// with no MMU: `free` can look healthy while the largest block is too
    /// small for the next buffer.
    pub largest_block: i32,
    /// Lowest `free` has ever been since boot — the true high-water mark.
    pub min_free: i32,
}

/// Read the current heap state.
pub fn heap() -> Heap {
    unsafe {
        Heap {
            free: raw::cpp_heap_free(),
            largest_block: raw::cpp_heap_largest_block(),
            min_free: raw::cpp_heap_min_free(),
        }
    }
}
