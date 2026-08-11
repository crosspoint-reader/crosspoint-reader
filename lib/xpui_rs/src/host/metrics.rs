//! Text measurement.
//!
//! The framework never estimates glyph sizes — an estimate drifts from what is
//! painted and pushes content off the panel. Every width and height comes from
//! the host's own font engine.

/// A font the host has registered. Opaque: only the host knows what it means,
/// and `0` means "this build does not ship that font".
#[derive(Copy, Clone, Debug, Default, PartialEq, Eq)]
pub struct FontId(pub i32);

impl FontId {
    /// The font a build compiled out. Measures zero and draws nothing.
    pub const UNAVAILABLE: FontId = FontId(0);

    pub fn is_available(self) -> bool {
        self.0 != 0
    }
}

/// Weight and slant, as a host-independent value.
#[derive(Copy, Clone, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum FontStyle {
    #[default]
    Regular = 0,
    Bold = 1,
    Italic = 2,
    BoldItalic = 3,
}

/// What a piece of text is *for*, rather than which typeface it uses.
///
/// The host decides what each role means. Naming families here — NotoSerif,
/// NotoSans — would tie the framework to one product's assets, and a role
/// survives those assets being changed.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum FontRole {
    /// Interface text: labels, list rows, values.
    Ui,
    /// Smaller interface text, for captions and secondary labels.
    UiSmall,
    /// The face the user chose for reading, including fonts loaded from storage.
    Reader,
}

/// Font lookup and measurement.
pub trait TextMetrics {
    /// The font for a role, or [`FontId::UNAVAILABLE`] when this build does not
    /// ship one — the `slim` build compiles most fonts out.
    fn font(&self, role: FontRole) -> FontId;

    fn text_width(&self, font: FontId, text: &str, style: FontStyle) -> i32;

    fn line_height(&self, font: FontId) -> i32;
}

/// A font plus the style it is drawn in, as widgets reach for it.
///
/// ```rust,ignore
/// Text::new("Battery").font(Font::ui_small())
/// Text::new(value).font(Font::ui().bold())
/// ```
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub struct Font {
    id: FontId,
    style: FontStyle,
}

impl Font {
    /// A font this build does not ship. Measures zero and draws nothing, so a
    /// missing face degrades quietly rather than painting garbage.
    pub const UNAVAILABLE: Font = Font {
        id: FontId::UNAVAILABLE,
        style: FontStyle::Regular,
    };

    pub fn role(role: FontRole) -> Self {
        Font {
            id: super::current().font(role),
            style: FontStyle::Regular,
        }
    }

    /// Interface text: the default for every widget.
    pub fn ui() -> Self {
        Self::role(FontRole::Ui)
    }

    /// Smaller interface text, for captions and secondary labels.
    pub fn ui_small() -> Self {
        Self::role(FontRole::UiSmall)
    }

    /// The face the user chose for reading.
    pub fn reader() -> Self {
        Self::role(FontRole::Reader)
    }

    pub fn bold(self) -> Self {
        self.with_style(FontStyle::Bold)
    }

    pub fn italic(self) -> Self {
        self.with_style(FontStyle::Italic)
    }

    pub fn with_style(mut self, style: FontStyle) -> Self {
        self.style = style;
        self
    }

    pub fn id(self) -> FontId {
        self.id
    }

    pub fn style(self) -> FontStyle {
        self.style
    }

    pub fn is_available(self) -> bool {
        self.id.is_available()
    }

    pub fn line_height(self) -> i32 {
        if !self.is_available() {
            return 0;
        }
        super::current().line_height(self.id)
    }

    pub fn text_width(self, text: &str) -> i32 {
        if !self.is_available() {
            return 0;
        }
        super::current().text_width(self.id, text, self.style)
    }
}
