//! Leaf views that draw content.
//!
//! A boolean setting is not a widget here: it is [`ListRow::toggle`], because
//! the firmware renders one as a list row with an On/Off value rather than a
//! switch graphic.

mod divider;
mod image;
mod list;
mod modal;
mod progress;
mod section;
mod slider;
mod stepper;
mod text;
mod toggle;

pub use divider::Divider;
pub use image::{Icon, Image};
pub use list::{List, ListRow};
pub use modal::Modal;
pub use progress::ProgressBar;
pub use section::Section;
pub use slider::Slider;
pub use stepper::Stepper;
pub use text::Text;
pub use toggle::Toggle;
