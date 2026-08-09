//! Geometry primitives shared by layout and drawing.
//!
//! All coordinates are logical screen pixels in the current orientation, the
//! same space `GfxRenderer` draws in. In portrait an X4 Pro is 480x800; in
//! landscape 800x480. Never assume either.

/// A position on screen.
#[derive(Copy, Clone, Debug, Default, PartialEq, Eq)]
pub struct Point {
    pub x: i32,
    pub y: i32,
}

impl Point {
    pub const ORIGIN: Point = Point { x: 0, y: 0 };

    pub const fn new(x: i32, y: i32) -> Self {
        Point { x, y }
    }

    /// This point moved by `dx`/`dy`.
    pub const fn offset(self, dx: i32, dy: i32) -> Self {
        Point::new(self.x + dx, self.y + dy)
    }
}

/// A width and height. Never negative: constructors clamp at zero so a
/// layout that over-subtracts cannot produce inverted rectangles.
#[derive(Copy, Clone, Debug, Default, PartialEq, Eq)]
pub struct Size {
    pub width: i32,
    pub height: i32,
}

impl Size {
    pub const ZERO: Size = Size {
        width: 0,
        height: 0,
    };

    pub fn new(width: i32, height: i32) -> Self {
        Size {
            width: width.max(0),
            height: height.max(0),
        }
    }

    /// This size shrunk by `dw`/`dh`, clamped at zero.
    pub fn shrink(self, dw: i32, dh: i32) -> Self {
        Size::new(self.width - dw, self.height - dh)
    }

    pub fn is_empty(self) -> bool {
        self.width == 0 || self.height == 0
    }
}

/// Space inset equally or individually on each edge.
#[derive(Copy, Clone, Debug, Default, PartialEq, Eq)]
pub struct Insets {
    pub top: i32,
    pub right: i32,
    pub bottom: i32,
    pub left: i32,
}

impl Insets {
    pub const ZERO: Insets = Insets {
        top: 0,
        right: 0,
        bottom: 0,
        left: 0,
    };

    /// The same inset on all four edges.
    pub const fn all(value: i32) -> Self {
        Insets {
            top: value,
            right: value,
            bottom: value,
            left: value,
        }
    }

    /// Independent horizontal and vertical insets.
    pub const fn symmetric(horizontal: i32, vertical: i32) -> Self {
        Insets {
            top: vertical,
            right: horizontal,
            bottom: vertical,
            left: horizontal,
        }
    }

    pub const fn horizontal(&self) -> i32 {
        self.left + self.right
    }

    pub const fn vertical(&self) -> i32 {
        self.top + self.bottom
    }
}

/// A positioned, sized region.
#[derive(Copy, Clone, Debug, Default, PartialEq, Eq)]
pub struct Rect {
    pub origin: Point,
    pub size: Size,
}

impl Rect {
    pub fn new(x: i32, y: i32, width: i32, height: i32) -> Self {
        Rect {
            origin: Point::new(x, y),
            size: Size::new(width, height),
        }
    }

    pub const fn x(&self) -> i32 {
        self.origin.x
    }

    pub const fn y(&self) -> i32 {
        self.origin.y
    }

    pub const fn width(&self) -> i32 {
        self.size.width
    }

    pub const fn height(&self) -> i32 {
        self.size.height
    }

    pub const fn right(&self) -> i32 {
        self.origin.x + self.size.width
    }

    pub const fn bottom(&self) -> i32 {
        self.origin.y + self.size.height
    }

    /// This rect pulled inward on every edge by `insets`.
    pub fn inset(&self, insets: Insets) -> Self {
        Rect {
            origin: self.origin.offset(insets.left, insets.top),
            size: self.size.shrink(insets.horizontal(), insets.vertical()),
        }
    }

    pub fn contains(&self, point: Point) -> bool {
        point.x >= self.origin.x
            && point.y >= self.origin.y
            && point.x < self.right()
            && point.y < self.bottom()
    }
}
