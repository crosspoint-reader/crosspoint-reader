//! A themed progress bar.

use crate::geometry::{Point, Rect, Size};
use crate::host::{Theme, ThemeMetric};
use crate::view::View;

/// Shows `current` out of `total`, drawn by the theme so it matches the reading
/// progress bar elsewhere in the firmware.
///
/// Spans the width it is given; its height comes from the theme unless
/// overridden with [`height`](ProgressBar::height).
///
/// ```rust,ignore
/// ProgressBar::new(page, total)
/// ProgressBar::percent(72)
/// ```
pub struct ProgressBar {
    current: u32,
    total: u32,
    height: Option<i32>,
    measured: Size,
}

impl ProgressBar {
    /// A bar at `current`/`total`. A zero `total` renders empty rather than
    /// dividing by zero.
    pub fn new(current: u32, total: u32) -> Self {
        ProgressBar {
            current,
            total,
            height: None,
            measured: Size::ZERO,
        }
    }

    /// A bar at `percent` of the way along, clamped to 0-100.
    pub fn percent(percent: u32) -> Self {
        ProgressBar::new(percent.min(100), 100)
    }

    /// Overrides the theme's bar height.
    pub fn height(mut self, height: i32) -> Self {
        self.height = Some(height);
        self
    }
}

impl<M> View<M> for ProgressBar {
    fn measure(&mut self, available: Size) {
        let height = self
            .height
            .unwrap_or_else(|| Theme::metric(ThemeMetric::ProgressBarHeight));
        self.measured = Size::new(available.width, height);
    }

    fn size(&self) -> Size {
        self.measured
    }

    fn render(&self, origin: Point) {
        if self.total == 0 {
            return;
        }
        Theme::draw_progress_bar(
            Rect {
                origin,
                size: self.measured,
            },
            self.current,
            self.total,
        );
    }
}
