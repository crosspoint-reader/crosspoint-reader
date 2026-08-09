//! CrossPoint screens written in Rust.
//!
//! Built on [`cpui`], which stays free of anything CrossPoint
//! specific. The module tree mirrors `src/activities/` on the C++ side so the
//! two read the same way: a screen in `activities/settings/` here corresponds
//! to one in `src/activities/settings/` there.
//!
//! Translations come from [`backend`]: `use backend::tr;` then `tr!(STR_FRONTLIGHT)`.
//!
//! See `docs/rust-ui-framework.md` for a walkthrough of adding a screen.

#![cfg_attr(target_os = "none", no_std)]

extern crate alloc;

// Linked for its side-effecting items rather than for any path used here: the
// `#[panic_handler]`, the `#[global_allocator]`, and the `rust_activity_*`
// entry points C++ calls. Screens reach `backend` through their own `use`, but
// this crate must not depend on one existing — without the anchor a device
// build fails with "`#[panic_handler]` function required, but not found",
// because an rlib nothing references is never pulled into the staticlib.
extern crate backend as _;

pub mod activities;
