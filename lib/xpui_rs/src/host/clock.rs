//! Time, for anything the framework paces itself with.

/// The host's monotonic clock.
pub trait Clock {
    /// Milliseconds since boot. Used for key auto-repeat; wrapping is handled
    /// by the caller, so a host may return a plain counter.
    fn millis(&self) -> u32;
}

/// Milliseconds since boot.
pub fn millis() -> u32 {
    super::current().millis()
}
