//! A declarative UI framework for e-ink firmware screens.
//!
//! Screens are described as a tree of [`View`]s and painted through whatever
//! host the firmware installs. The API is deliberately SwiftUI-shaped:
//!
//! ```rust,ignore
//! NavigationScreen::new(vstack![20;
//!     Text::new("Firmware"),
//!     Text::new(version).bold(),
//!     Spacer::new(),
//! ])
//! ```
//!
//! See this crate's `README.md` for a walkthrough, and `docs/architecture.md`
//! for how a frame runs.
//!
//! # Layers
//!
//! - [`host`] — the traits a firmware implements, and the façades widgets call.
//!   The crate's only `unsafe` is here, around the installed-host global.
//! - [`geometry`] — [`Point`], [`Size`], [`Rect`], [`Insets`].
//! - [`view`] — the [`View`] trait and the interaction model.
//! - [`layout`] — containers that position children: stacks, spacer, modifiers.
//! - [`widgets`] — leaves that draw: text, lists, sliders, icons.
//! - [`screen`] — the [`Screen`] contract, its runtime, and the root views.
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
    Button, Font, FontId, FontRole, FontStyle, Hint, IconRef, Input, Renderer, ScreenChrome,
    SwipeDir, Theme, ThemeMetric, finish_screen, millis, request_update,
};
pub use layout::{
    Alignment, Flexible, Frame, HStack, Modifiers, Padding, ScrollView, Spacer, Tappable, VStack,
};
pub use screen::{NavigationScreen, OverlayPanel, Screen};
pub use view::{InputMask, Interaction, Interactions, Scrim, Trigger, View, ViewExt, value_at};
pub use widgets::{
    Divider, Icon, IconToggle, Image, List, ListRow, Modal, ProgressBar, Section, Slider, Stepper,
    Text, Toggle,
};
