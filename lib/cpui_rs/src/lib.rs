//! A declarative UI framework for e-ink firmware screens.
//!
//! Screens are described as a tree of [`View`]s and rendered through the host
//! firmware's C++ renderer over FFI. The API is deliberately SwiftUI-shaped:
//!
//! ```rust,ignore
//! NavigationScreen::new(
//!     VStack::new(20)
//!         .push(Text::new("Firmware").font(Font::ui_label()))
//!         .push(Text::new(version).font(Font::ui_value()))
//!         .push(Spacer::new())
//!         .boxed(),
//! )
//! ```
//!
//! # Layers
//!
//! - [`ffi`] — the only `unsafe` in the crate; safe wrappers over the C++
//!   renderer, fonts, input, theme, device info and translations.
//! - [`geometry`] — [`Point`], [`Size`], [`Rect`], [`Insets`].
//! - [`layout`] — containers that position children: stacks, spacer, padding.
//! - [`widgets`] — leaves that draw: text, divider.
//! - [`screen`] — roots that own a whole screen's chrome.
//! - [`activity`] — the screen lifecycle and its C++ bridge.
//!
//! # Portability
//!
//! Builds `no_std` for the ESP32 targets (using `alloc` against the firmware's
//! heap) and against `std` on the host so the simulator and `cargo test` work
//! unchanged. Nothing in this crate may reference a specific product — device
//! screens belong in the consuming crate.

#![cfg_attr(target_os = "none", no_std)]

extern crate alloc;

pub mod geometry;
pub mod host;
pub mod layout;
pub mod screen;
pub mod view;
pub mod widgets;

#[cfg(any(test, feature = "testing"))]
pub mod testing;

pub use geometry::{Insets, Point, Rect, Size};
pub use host::{
    finish_screen, millis, request_update, Button, Font, FontId, FontRole, FontStyle, Hint,
    IconRef, Input, Renderer, ScreenChrome, SwipeDir, Theme, ThemeMetric,
};
pub use layout::{
    Alignment, Flexible, Frame, HStack, Modifiers, Padding, Spacer, Tappable, VStack,
};
pub use screen::{NavigationScreen, OverlayPanel, Screen, Scrim};
pub use view::{value_at, InputMask, Interaction, Interactions, Trigger, View, ViewExt};
pub use widgets::{
    Divider, Icon, Image, List, ListRow, Modal, ProgressBar, Section, Slider, Stepper, Text, Toggle,
};
