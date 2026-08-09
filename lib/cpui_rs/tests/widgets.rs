//! Widget geometry, asserted against the test doubles.
//!
//! These pin the parts that are easy to get subtly wrong and impossible to
//! eyeball: where a slider knob sits, which word a toggle shows, and whether a
//! modal's hit rects line up with the rows it drew.

use cpui::testing::{self, RectKind, SCREEN_HEIGHT, SCREEN_WIDTH};
use cpui::{value_at, Image, List, ListRow, Modal, Point, Rect, Size, Slider, View};

fn available() -> Size {
    testing::install();
    Size::new(SCREEN_WIDTH, SCREEN_HEIGHT)
}

/// The knob's *left edge* travels `width - knob_width`, so at 0% it sits flush
/// with the track start and at 100% its right edge meets the end — its centre
/// never reaches either extreme. Matching the C++ slider here is the point.
#[test]
fn slider_knob_travels_the_track_without_overhanging() {
    const WIDTH: i32 = 200;
    const SIDE_INSET: i32 = 8;
    const KNOB_WIDTH: i32 = 14;

    // Last recorded rect is the knob outline; the one before it is the knob fill.
    let knob_x = |value: i32| {
        let mut slider = Slider::<()>::new(value, 100);
        slider.measure(Size::new(WIDTH, 60));
        testing::reset();
        slider.render(Point::ORIGIN);

        let rects = testing::drawn_rects();
        let stroked = rects
            .iter()
            .rfind(|r| r.4 == RectKind::Stroked)
            .expect("knob outline");
        assert_eq!(stroked.2, KNOB_WIDTH, "knob width is fixed");
        stroked.0
    };

    let travel = WIDTH - SIDE_INSET * 2 - KNOB_WIDTH;

    assert_eq!(
        knob_x(0),
        SIDE_INSET,
        "at 0% the knob sits at the track start"
    );
    assert_eq!(
        knob_x(100),
        SIDE_INSET + travel,
        "at 100% the knob's right edge meets the track end"
    );
    assert_eq!(knob_x(50), SIDE_INSET + travel / 2, "50% is halfway along");

    // Monotonic: a larger value never moves the knob backwards.
    let mut previous = i32::MIN;
    for value in [0, 10, 25, 50, 75, 90, 100] {
        let x = knob_x(value);
        assert!(x >= previous, "knob went backwards at {value}%");
        previous = x;
    }
}

/// The track is dithered and the filled portion is solid, so the two are
/// distinguishable on a 1-bit panel.
#[test]
fn slider_draws_a_dithered_track_under_a_solid_fill() {
    let mut slider = Slider::<()>::new(50, 100);
    slider.measure(Size::new(200, 60));
    testing::reset();
    slider.render(Point::ORIGIN);

    let rects = testing::drawn_rects();
    assert_eq!(
        rects.first().map(|r| r.4),
        Some(RectKind::Dither),
        "the track must be drawn first, and dithered"
    );
    assert!(
        rects.iter().any(|r| r.4 == RectKind::Filled),
        "the value portion must be a solid fill"
    );
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

/// Hit rects must come back for every option and be disjoint, or a tap would
/// select the wrong row.
#[test]
fn modal_row_rects_cover_each_option_without_overlapping() {
    let mut modal: Modal =
        Modal::new("Orientation", ["Portrait", "Landscape CW", "Inverted"]).selected(1);
    View::<()>::measure(&mut modal, available());

    let rects: Vec<_> = (0..modal.len())
        .map(|i| modal.row_rect(i).expect("every option needs a hit rect"))
        .collect();

    assert_eq!(rects.len(), 3);
    for pair in rects.windows(2) {
        assert!(
            pair[0].bottom() <= pair[1].y(),
            "rows {:?} and {:?} overlap",
            pair[0],
            pair[1]
        );
    }

    assert!(
        modal.row_rect(99).is_none(),
        "out-of-range index must be None"
    );
}

/// A tap resolves to the row that contains it, and to nothing outside.
#[test]
fn modal_maps_a_tap_to_its_option() {
    let mut modal: Modal = Modal::new("Orientation", ["Portrait", "Landscape CW", "Inverted"]);
    View::<()>::measure(&mut modal, available());

    for index in 0..modal.len() {
        let rect = modal.row_rect(index).unwrap();
        let centre = Point::new(rect.x() + rect.width() / 2, rect.y() + rect.height() / 2);
        assert_eq!(modal.option_at(centre), Some(index));
    }

    assert_eq!(modal.option_at(Point::new(0, 0)), None);
}

/// An empty modal draws nothing rather than asking the theme for a zero-row
/// dialog.
#[test]
fn empty_modal_is_inert() {
    let mut modal: Modal = Modal::new("Nothing", Vec::<String>::new());
    View::<()>::measure(&mut modal, available());

    assert!(modal.is_empty());
    assert!(modal.row_rect(0).is_none());
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
