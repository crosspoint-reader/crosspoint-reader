//! Widget geometry, asserted against the test doubles.
//!
//! These pin the parts that are easy to get subtly wrong and impossible to
//! eyeball: where a slider knob sits, which word a toggle shows, and whether a
//! modal's hit rects line up with the rows it drew.

use xpui::host::IconRef;
use xpui::testing::{
    self, MIN_TOUCH_SIZE, SCREEN_HEIGHT, SCREEN_WIDTH, SLIDER_KNOB_WIDTH, SLIDER_SIDE_INSET,
};
use xpui::view::{InputMask, Interactions, Trigger};
use xpui::{IconToggle, Image, List, ListRow, Modal, Point, Rect, Size, Slider, View, value_at};

fn available() -> Size {
    testing::install();
    Size::new(SCREEN_WIDTH, SCREEN_HEIGHT)
}

/// The host draws the slider, so the widget must hand it over rather than paint
/// anything itself. A widget that drew its own rectangles would sit beside a
/// C++ panel's slider looking nothing like it.
#[test]
fn a_slider_is_drawn_by_the_host() {
    testing::install();
    let mut slider = Slider::<()>::new(50, 100);
    slider.measure(Size::new(200, 60));
    testing::reset();
    slider.render(Point::ORIGIN);

    let drawn = testing::drawn_sliders();
    assert_eq!(drawn.len(), 1, "exactly one slider, drawn by the theme");

    let (rect, value, max) = drawn[0];
    assert_eq!((value, max), (50, 100), "the value goes through untouched");
    assert_eq!(rect.width(), 200, "the host gets the widget's own bounds");
    assert!(
        testing::drawn_rects().is_empty(),
        "and the widget paints nothing of its own"
    );
}

/// The knob belongs to the host now; the conversion from a touch to a value is
/// what the framework still owns. The property that mattered when it drew the
/// knob itself survives: both ends are reachable, and dragging right never
/// walks the value backwards.
#[test]
fn a_touch_spans_the_whole_range_without_dead_ends() {
    testing::install();
    let track = Rect::new(0, 0, 200, 60);
    let reachable = |x: i32| value_at(track, x, 100);

    assert_eq!(
        reachable(track.x() + SLIDER_SIDE_INSET + SLIDER_KNOB_WIDTH / 2),
        0,
        "the left end of the travel reads 0"
    );
    assert_eq!(
        reachable(track.x() + track.width() - SLIDER_SIDE_INSET - SLIDER_KNOB_WIDTH / 2),
        100,
        "the right end reads max"
    );

    let mut previous = i32::MIN;
    for x in (0..=track.width()).step_by(5) {
        let value = reachable(x);
        assert!(value >= previous, "the value went backwards at x={x}");
        previous = value;
    }
}

/// A tap outside the track clamps rather than producing an out-of-range value.
#[test]
fn slider_clamps_touches_outside_the_track() {
    let track = Rect::new(0, 0, 200, 60);

    assert_eq!(value_at(track, -500, 100), 0);
    assert_eq!(value_at(track, 5_000, 100), 100);
}

/// A toggle is a list row whose value reads as one of two words — there is no
/// switch graphic in this firmware, so there must not be one here either. The
/// row must ask the theme for the right word, and layout must not depend on it.
#[test]
fn toggle_row_shows_the_state_as_its_value() {
    let value_of = |on: bool| {
        let row: ListRow<()> = ListRow::toggle("Hyphenation", on, "On", "Off");
        // field 2 is the right-hand value; read it back through the same
        // accessor the theme callback uses.
        row.value_text().map(str::to_owned)
    };

    assert_eq!(value_of(true).as_deref(), Some("On"));
    assert_eq!(value_of(false).as_deref(), Some("Off"));

    let sized = |on: bool| {
        let mut list = List::<()>::new().push(ListRow::toggle("Hyphenation", on, "On", "Off"));
        list.measure(available());
        list.size()
    };
    assert_eq!(sized(true), sized(false), "state must not alter layout");
}

/// One interaction per option, disjoint, or a tap would choose the wrong row.
/// The rects come from the C++ popup's own layout, so this also pins that the
/// Rust side is reading that geometry correctly.
#[test]
fn modal_declares_one_interaction_per_option() {
    let mut modal = Modal::new("Orientation", ["Portrait", "Landscape CW", "Inverted"])
        .selected(1)
        .on_select(|index| index);
    View::<usize>::measure(&mut modal, available());

    let mut out = Interactions::new(0);
    modal.interactions(Point::ORIGIN, &mut out);

    let rects: Vec<_> = out.items().iter().map(|i| i.rect).collect();
    assert_eq!(rects.len(), 3, "one per option");
    for pair in rects.windows(2) {
        assert!(
            pair[0].bottom() <= pair[1].y(),
            "rows {:?} and {:?} overlap",
            pair[0],
            pair[1]
        );
    }
}

/// A tap resolves to the option it landed on, and the reported index matches.
#[test]
fn modal_maps_a_tap_to_its_option() {
    let mut modal = Modal::new("Orientation", ["Portrait", "Landscape CW", "Inverted"])
        .on_select(|index| index);
    View::<usize>::measure(&mut modal, available());

    let mut out = Interactions::new(0);
    modal.interactions(Point::ORIGIN, &mut out);

    for (index, item) in out.items().iter().enumerate() {
        let centre = Point::new(
            item.rect.x() + item.rect.width() / 2,
            item.rect.y() + item.rect.height() / 2,
        );
        assert!(item.rect.contains(centre));
        match &item.trigger {
            Trigger::Message(reported) => assert_eq!(*reported, index),
            _ => panic!("an option sends a plain message"),
        }
    }
}

/// A dialog captures input even before anything is chosen, so the list behind
/// it is unreachable the moment it appears.
#[test]
fn modal_captures_input() {
    let mut modal = Modal::new("Orientation", ["Portrait", "Landscape CW"])
        .selected(1)
        .on_select(|index| index);
    View::<usize>::measure(&mut modal, available());

    let mut out = Interactions::new(0);
    out.declare(
        Rect::new(0, 0, 10, 10),
        InputMask::DEFAULT,
        Trigger::Message(99),
    );
    modal.interactions(Point::ORIGIN, &mut out);

    assert_eq!(
        out.focusable_count(),
        2,
        "only the dialog's own options remain"
    );
    assert_eq!(
        out.captured_focus(),
        Some(1),
        "focus opens on the value already chosen"
    );
}

/// An empty dialog draws nothing and captures nothing, rather than asking the
/// theme for a zero-row popup or trapping input in something invisible.
#[test]
fn empty_modal_is_inert() {
    let mut modal = Modal::new("Nothing", Vec::<String>::new()).on_select(|index: usize| index);
    View::<usize>::measure(&mut modal, available());

    let mut out = Interactions::new(0);
    modal.interactions(Point::ORIGIN, &mut out);

    assert!(modal.is_empty());
    assert!(out.is_empty());
    assert_eq!(
        out.captured_focus(),
        None,
        "an empty dialog must not capture"
    );
}

/// A bitmap occupies exactly its own size, and a buffer too short for its
/// stated dimensions is refused rather than read past the end.
#[test]
fn image_measures_to_its_own_size_and_rejects_a_short_buffer() {
    static GOOD: [u8; 16] = [0; 16]; // 8x16: 1 byte per row
    static SHORT: [u8; 2] = [0; 2];

    let mut ok = Image::new(&GOOD, 8, 16);
    View::<()>::measure(&mut ok, available());
    assert_eq!(View::<()>::size(&ok), Size::new(8, 16));

    let mut bad = Image::new(&SHORT, 64, 64);
    View::<()>::measure(&mut bad, available());
    assert_eq!(
        View::<()>::size(&bad),
        Size::ZERO,
        "short buffer must not be drawn"
    );

    testing::reset();
    View::<()>::render(&bad, Point::ORIGIN);
    assert!(testing::drawn_text().is_empty());
}

/// A 32px glyph is a legitimate control but a poor finger target, and two of
/// them side by side used to need the screen to reserve space by hand. The
/// widget must do it: each reserves the theme minimum on both axes, so laying
/// two in a row cannot leave them competing for the same tap.
#[test]
fn icon_toggle_reserves_a_full_touch_target() {
    let mut toggle: IconToggle<()> = IconToggle::new(IconRef::new(0), true);
    toggle.measure(available());

    let size = View::<()>::size(&toggle);
    assert!(
        size.width >= MIN_TOUCH_SIZE && size.height >= MIN_TOUCH_SIZE,
        "control is {size:?}, under the {MIN_TOUCH_SIZE}px minimum touch target"
    );
}

/// The reported state is the one a tap moves *to*, so a screen never has to
/// write `!self.something`. The hit rect must cover the whole reserved control
/// rather than only the glyph drawn inside it.
#[test]
fn icon_toggle_reports_the_state_it_moves_to() {
    for was_on in [false, true] {
        let mut toggle = IconToggle::new(IconRef::new(0), was_on).on_change(|next| next);
        toggle.measure(available());

        let mut out = Interactions::new(0);
        toggle.interactions(Point::ORIGIN, &mut out);

        let declared = out
            .items()
            .first()
            .expect("a tappable icon declares a rect");
        match &declared.trigger {
            Trigger::Message(reported) => assert_eq!(
                *reported, !was_on,
                "tapping an icon showing {was_on} should report {}",
                !was_on
            ),
            _ => panic!("an icon toggle sends a plain message, not a value trigger"),
        }
        assert_eq!(
            declared.rect.size,
            View::<bool>::size(&toggle),
            "the hit rect must cover the whole control"
        );
    }
}
