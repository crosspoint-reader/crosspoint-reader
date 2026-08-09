//! Bare-metal runtime glue: heap and panic handling.
//!
//! Only compiled for the ESP32 targets. On the host, `std` supplies both an
//! allocator and a panic handler, and defining our own would collide.

use core::alloc::{GlobalAlloc, Layout};
use core::ffi::{c_char, c_void};

extern "C" {
    fn malloc(size: usize) -> *mut c_void;
    fn free(ptr: *mut c_void);
    fn cpp_panic(message: *const c_char) -> !;
}

/// Routes Rust allocations to the firmware heap so Rust and C++ share one
/// allocator and one free-heap figure.
struct FirmwareAlloc;

unsafe impl GlobalAlloc for FirmwareAlloc {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        // ESP-IDF's malloc returns memory aligned for any scalar type. Larger
        // alignments are not requested by this crate; refuse rather than
        // silently hand back misaligned memory (the ESP32-C3 faults on
        // unaligned multi-byte access).
        if layout.align() > core::mem::size_of::<usize>() * 2 {
            return core::ptr::null_mut();
        }
        malloc(layout.size()) as *mut u8
    }

    unsafe fn dealloc(&self, ptr: *mut u8, _layout: Layout) {
        free(ptr as *mut c_void);
    }
}

#[global_allocator]
static ALLOCATOR: FirmwareAlloc = FirmwareAlloc;

/// Reports the panic through the firmware log, then aborts.
///
/// The message is not forwarded: formatting it would pull `core::fmt` machinery
/// into every build and needs an allocation on a path where the heap may be the
/// thing that failed. The location is enough to find it in the map file.
#[panic_handler]
fn on_panic(_info: &core::panic::PanicInfo) -> ! {
    unsafe { cpp_panic(c"Rust panic in UI framework".as_ptr()) }
}
