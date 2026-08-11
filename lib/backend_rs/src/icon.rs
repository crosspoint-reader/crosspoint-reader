//! Icons, named by role rather than by asset.

use super::raw;

/// What an icon *means*, not which file it comes from.
///
/// Roles rather than asset names for three reasons: this crate must not know a
/// product's asset set; the firmware already keeps a role table (`UIIcon` and
/// `listIconFor` in `UiAppHelpers.h`) that this reuses instead of adding a
/// fourth parallel mapping; and a role survives the assets being redrawn or
/// renamed, which a generated id does not.
#[repr(u8)]
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum IconRole {
    Book = 0,
    Bookmark = 1,
    Download = 2,
    File = 3,
    FileText = 4,
    Folder = 5,
    Image = 6,
    Library = 7,
    Moon = 8,
    Hotspot = 9,
    Search = 10,
    Sun = 11,
    Upload = 12,
    Wifi = 13,
}

/// A role, plus how it should be drawn.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub struct IconSpec {
    pub role: IconRole,
    /// Solid rather than outline, where the role has both. Sun and Moon do;
    /// asking for a filled variant that does not exist falls back to the
    /// outline rather than drawing nothing.
    pub filled: bool,
    /// Preferred edge length. The host picks the nearest size it ships.
    pub size: i32,
}

impl IconSpec {
    pub const DEFAULT_SIZE: i32 = 32;

    pub fn new(role: IconRole) -> Self {
        IconSpec {
            role,
            filled: false,
            size: Self::DEFAULT_SIZE,
        }
    }

    /// The size the host will actually draw, or zero when it ships nothing for
    /// this role — the same "compiled out" signal a missing font gives.
    pub fn resolved_size(&self) -> i32 {
        unsafe { raw::cpp_icon_size(self.role as u8, u8::from(self.filled), self.size) }
    }
}

impl From<IconRole> for xpui::host::IconRef {
    /// Roles are the product's vocabulary; the framework only ever sees an
    /// opaque handle, which is what keeps asset names out of `xpui`.
    fn from(role: IconRole) -> Self {
        xpui::host::IconRef {
            kind: role as u16,
            variant: 0,
            size: IconSpec::DEFAULT_SIZE,
        }
    }
}
