//! One row of a themed list.

use alloc::string::String;

use crate::host::RowField;

/// One row: a title, and optionally a subtitle beneath it and a value on the
/// right. Strings are converted once here rather than on every frame.
pub struct ListRow<M> {
    pub(super) title: String,
    pub(super) subtitle: Option<String>,
    pub(super) value: Option<String>,
    /// Sent when the row is tapped or activated with Confirm. A row without
    /// one is a read-out and never takes focus.
    pub(super) message: Option<M>,
}

impl<M> ListRow<M> {
    pub fn new(title: impl Into<String>) -> Self {
        ListRow {
            title: title.into(),
            subtitle: None,
            value: None,
            message: None,
        }
    }

    /// Makes the row interactive: tapping it, or focusing it and pressing
    /// Confirm, sends `message`.
    pub fn on_tap(mut self, message: M) -> Self {
        self.message = Some(message);
        self
    }

    /// A boolean setting: a row whose value reads as one of two words.
    ///
    /// This is what a toggle *is* in this firmware — there is no switch
    /// graphic anywhere in the C++ settings screens, and drawing one here
    /// would look foreign beside them. Put it in the screen's [`List`] like any
    /// other row, so the theme marks the focused one.
    ///
    /// Both words are supplied by the caller because they vary by setting
    /// (On/Off, Show/Hide) and only the caller can translate them.
    ///
    /// ```rust,ignore
    /// ListRow::toggle("Hyphenation", on, "On", "Off")
    /// ```
    pub fn toggle(
        title: impl Into<String>,
        on: bool,
        on_label: impl Into<String>,
        off_label: impl Into<String>,
    ) -> Self {
        let value = if on {
            on_label.into()
        } else {
            off_label.into()
        };
        ListRow::new(title).value(value)
    }

    /// Secondary text below the title. Rows get taller when any row has one.
    pub fn subtitle(mut self, subtitle: impl Into<String>) -> Self {
        self.subtitle = Some(subtitle.into());
        self
    }

    /// Right-aligned text, for the current setting of an option row.
    pub fn value(mut self, value: impl Into<String>) -> Self {
        self.value = Some(value.into());
        self
    }

    /// The right-hand value as text, if the row has one. Mainly for tests:
    /// the theme reads it through the callback below.
    pub fn value_text(&self) -> Option<&str> {
        self.value.as_deref()
    }

    pub(super) fn field(&self, field: RowField) -> Option<&str> {
        match field {
            RowField::Title => Some(self.title.as_str()),
            RowField::Subtitle => self.subtitle.as_deref(),
            RowField::Value => self.value.as_deref(),
        }
    }
}
