//! Declarative shorthand for building layout trees.
//!
//! These expand to the same builder calls you would write by hand, so there is
//! no hidden behaviour — they only remove the repeated `.push(`. Use whichever
//! reads better: the macro suits a fixed tree, the builder suits one assembled
//! conditionally or in a loop.

/// A vertical stack.
///
/// ```rust,ignore
/// vstack![20;
///     Text::new("Title").bold(),
///     Text::new("Body"),
///     Spacer::new(),
/// ]
/// ```
///
/// Equivalent to `VStack::new(20).push(..).push(..).push(..)`.
#[macro_export]
macro_rules! vstack {
    ($spacing:expr_2021 $(,)?) => {
        $crate::VStack::new($spacing)
    };
    ($spacing:expr_2021; $($child:expr_2021),+ $(,)?) => {
        $crate::VStack::new($spacing)$(.push($child))+
    };
}

/// A horizontal stack. Same shape as [`vstack!`].
///
/// ```rust,ignore
/// hstack![8; Text::new("Battery"), Spacer::new(), Text::new("72%")]
/// ```
#[macro_export]
macro_rules! hstack {
    ($spacing:expr_2021 $(,)?) => {
        $crate::HStack::new($spacing)
    };
    ($spacing:expr_2021; $($child:expr_2021),+ $(,)?) => {
        $crate::HStack::new($spacing)$(.push($child))+
    };
}

/// A themed list.
///
/// ```rust,ignore
/// list![selected;
///     ListRow::new("Wi-Fi").value("On"),
///     ListRow::new("Bluetooth").value("Off"),
/// ]
/// ```
#[macro_export]
macro_rules! list {
    ($selected:expr_2021 $(,)?) => {
        $crate::List::new().selected($selected)
    };
    ($selected:expr_2021; $($row:expr_2021),+ $(,)?) => {
        $crate::List::new().selected($selected)$(.push($row))+
    };
}
