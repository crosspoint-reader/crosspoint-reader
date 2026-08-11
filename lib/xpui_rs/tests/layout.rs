//! Layout regression tests.
//!
//! These run against the deterministic doubles in `xpui::testing`,
//! so they need neither hardware nor the simulator. The property they defend is
//! the one that kept breaking by hand: everything a screen draws has to land
//! inside the region the theme reserved for it.

use xpui::testing::{
    self, CONTENT_BOTTOM, CONTENT_TOP, RectKind, SCREEN_HEIGHT, SCREEN_WIDTH, SIDE_PADDING,
};
use xpui::view::Interactions;
use xpui::{
    Font, HStack, NavigationScreen, OverlayPanel, Padding, Point, Scrim, Size, Spacer, Text,
    VStack, View, hstack, vstack,
};

fn screen_size() -> Size {
    testing::install();
    Size::new(SCREEN_WIDTH, SCREEN_HEIGHT)
}

/// A screen shaped like a real settings page: labelled fields, a flexible gap,
/// then a footer pinned to the bottom.
fn info_screen() -> NavigationScreen<()> {
    let field = |label: &str, value: &str| {
        vstack![4;
            Text::new(label).font(Font::ui_small()),
            Text::new(value).bold(),
        ]
    };

    NavigationScreen::new(vstack![20;
        field("Firmware Version", "1.5.0-x4pro"),
        field("Device", "xteink_x4_pro"),
        field("Battery", "72%"),
        Spacer::new(),
        Text::new("footer"),
    ])
}

fn layout_and_capture(view: &mut dyn View<()>) -> Vec<testing::TextDraw> {
    testing::reset();
    view.measure(screen_size());
    view.render(Point::ORIGIN);
    testing::drawn_text()
}

/// The regression that started all this: a flexible spacer used to be measured
/// against the whole screen on top of its fixed siblings, so the footer landed
/// hundreds of pixels below the panel and the renderer logged a write per pixel.
#[test]
fn every_draw_lands_inside_the_content_band() {
    let mut screen = info_screen();
    let draws = layout_and_capture(&mut screen);

    assert!(!draws.is_empty(), "the screen drew nothing at all");

    for (x, y, text, font_id, _style) in draws {
        let width = testing::text_width(&text, font_id);
        let bottom = y + testing::line_height(font_id);

        assert!(
            y >= CONTENT_TOP,
            "{text:?} at y={y} overlaps the header band (content starts at {CONTENT_TOP})"
        );
        assert!(
            bottom <= CONTENT_BOTTOM,
            "{text:?} ends at y={bottom}, under the button hints at {CONTENT_BOTTOM}"
        );
        assert!(
            x >= SIDE_PADDING,
            "{text:?} at x={x} ignores the theme side padding of {SIDE_PADDING}"
        );
        assert!(
            x + width <= SCREEN_WIDTH - SIDE_PADDING,
            "{text:?} runs past the right side padding"
        );
    }
}

/// A spacer must resolve against the content band, not the whole screen.
#[test]
fn spacer_pushes_the_footer_to_the_bottom_of_the_band() {
    let mut screen = info_screen();
    let draws = layout_and_capture(&mut screen);

    let (_, footer_y, footer_text, footer_font, _) = draws.last().unwrap().clone();
    assert_eq!(footer_text, "footer");

    let footer_bottom = footer_y + testing::line_height(footer_font);
    assert!(
        footer_bottom <= CONTENT_BOTTOM,
        "footer bottom {footer_bottom} spills past the band at {CONTENT_BOTTOM}"
    );
    assert!(
        footer_bottom > CONTENT_BOTTOM - 40,
        "footer at {footer_bottom} was not pushed down to the bottom of the band"
    );
}

/// Without a flexible child a stack reports its natural height rather than
/// stretching to whatever it was offered.
#[test]
fn stack_without_a_spacer_reports_its_natural_height() {
    let mut stack = VStack::<()>::new(10)
        .push(Text::new("one"))
        .push(Text::new("two"));

    stack.measure(screen_size());

    let line = testing::line_height(1003); // the UI family the doubles hand out
    assert_eq!(stack.size().height, line * 2 + 10);
}

/// Text is sized from the firmware's font metrics, not an estimate. Guessing
/// is what let content drift off the bottom of the screen.
#[test]
fn text_measures_with_real_font_metrics() {
    let mut text = Text::new("hello");
    View::<()>::measure(&mut text, screen_size());

    let font_id = 1003;
    let size = View::<()>::size(&text);
    assert_eq!(size.height, testing::line_height(font_id));
    assert_eq!(size.width, testing::text_width("hello", font_id));
}

/// Bold must actually change the style handed to the renderer. It used to be a
/// builder that returned `self` unchanged.
#[test]
fn bold_reaches_the_renderer() {
    let mut plain = Text::new("x");
    let mut bold = Text::new("x").bold();

    testing::reset();
    View::<()>::measure(&mut plain, screen_size());
    View::<()>::render(&plain, Point::ORIGIN);
    View::<()>::measure(&mut bold, screen_size());
    View::<()>::render(&bold, Point::ORIGIN);

    let draws = testing::drawn_text();
    assert_eq!(
        draws[0].4, 0,
        "plain text should draw with the regular style"
    );
    assert_eq!(draws[1].4, 1, "bold text should draw with the bold style");
}

/// Padding insets on every edge and grows the measured size accordingly.
#[test]
fn padding_insets_its_child() {
    let mut padded: Padding<()> = Padding::all(Text::new("x"), 12);
    padded.measure(screen_size());

    testing::reset();
    padded.render(Point::ORIGIN);
    let (x, y, ..) = testing::drawn_text()[0];

    assert_eq!((x, y), (12, 12));
    assert_eq!(
        View::<()>::size(&padded).height,
        testing::line_height(1003) + 24
    );
}

/// A font the build omitted must be inert rather than drawing with a bad id.
#[test]
fn unavailable_fonts_do_not_draw() {
    // A build that compiled this face out hands back UNAVAILABLE.
    let missing = Font::UNAVAILABLE;
    assert!(!missing.is_available());
    assert_eq!(missing.line_height(), 0);

    let mut text = Text::new("invisible").font(missing);
    View::<()>::measure(&mut text, screen_size());

    testing::reset();
    View::<()>::render(&text, Point::ORIGIN);
    assert!(
        testing::drawn_text().is_empty(),
        "text in an unavailable font should draw nothing"
    );
}

/// A `Spacer` is offered the full cross extent and records it. If that counted
/// towards the stack's size, a row holding one would be as tall as the space it
/// was offered — which is what made the frontlight panel's header row 750px
/// tall, pushed both sliders off the bottom of the screen, and left the overlay
/// covering the whole panel.
///
/// The existing spacer tests miss this: `every_draw_lands_inside_the_content_band`
/// uses a `VStack`, where filling the cross axis is harmless because siblings
/// already span the width.
#[test]
fn a_spacer_does_not_stretch_the_row_it_sits_in() {
    let text_height = {
        let mut text = Text::new("Brightness");
        View::<()>::measure(&mut text, screen_size());
        View::<()>::size(&text).height
    };

    let mut row: HStack<()> = hstack![10;
        Text::new("Brightness"),
        Spacer::new(),
        Text::new("60%"),
    ];
    row.measure(screen_size());

    assert_eq!(
        row.size().height,
        text_height,
        "a row should be as tall as its content, not as tall as the space offered"
    );
    assert_eq!(
        row.size().width,
        SCREEN_WIDTH,
        "the spacer should still absorb the width"
    );
}

/// An overlay's height must come from its content: the panel paints only its own
/// band and uses the same height as the threshold below which a tap dismisses it.
/// When it measured to the full screen, it wiped the framebuffer it exists to
/// preserve and no touch could ever reach the dismiss region.
#[test]
fn an_overlay_panel_is_sized_by_its_content() {
    let mut panel: OverlayPanel<()> = OverlayPanel::new(vstack![10;
        hstack![10; Text::new("Brightness"), Spacer::new(), Text::new("60%")],
        Text::new("Warmth"),
    ]);
    panel.measure(screen_size());

    let height = panel.size().height;
    assert!(
        height > CONTENT_TOP,
        "the panel must clear the header band, got {height}"
    );
    assert!(
        height < SCREEN_HEIGHT / 2,
        "two rows of content should not fill half the screen, got {height}"
    );
    assert_eq!(
        panel.size().width,
        SCREEN_WIDTH,
        "a drop-down spans the width"
    );
}

/// The scrim must cover exactly the strip the panel leaves showing, and must be
/// a scrim rather than a fill: a filled rect would erase the screen underneath,
/// which is the one thing an overlay exists to preserve.
#[test]
fn a_dimmed_overlay_scrims_exactly_the_area_below_it() {
    xpui::testing::reset();

    let mut panel: OverlayPanel<()> = OverlayPanel::new(vstack![10;
        Text::new("Brightness"),
        Text::new("Warmth"),
    ])
    .scrim(Scrim::Dim);
    panel.measure(screen_size());
    panel.render(Point::ORIGIN);

    let panel_height = panel.size().height;
    let scrims: Vec<_> = xpui::testing::drawn_rects()
        .into_iter()
        .filter(|(_, _, _, _, kind)| *kind == RectKind::Scrim)
        .collect();

    assert_eq!(
        scrims.len(),
        1,
        "expected exactly one scrim, got {scrims:?}"
    );
    assert_eq!(
        scrims[0],
        (
            0,
            panel_height,
            SCREEN_WIDTH,
            SCREEN_HEIGHT - panel_height,
            RectKind::Scrim
        ),
        "the scrim must start where the panel ends and reach the screen bottom"
    );
}

/// A screen that sets no title of its own must still get one: both roots defer
/// to the activity's translated title, and the framework has to resolve that
/// before it reaches the host. Passing `None` straight through meant "draw no
/// title", which painted an empty header band on the real device.
#[test]
fn a_root_without_an_explicit_title_draws_the_screens_own() {
    for (label, mut root) in [
        (
            "OverlayPanel",
            Box::new(OverlayPanel::new(Text::new("x"))) as Box<dyn View<()>>,
        ),
        (
            "NavigationScreen",
            Box::new(NavigationScreen::new(Text::new("x"))) as Box<dyn View<()>>,
        ),
    ] {
        testing::reset();
        root.measure(screen_size());
        root.render(Point::ORIGIN);

        assert_eq!(
            testing::drawn_headers(),
            vec![Some(xpui::host::ScreenChrome::screen_title().to_string())],
            "{label} should draw the screen's own title, not an empty header"
        );
    }
}

/// Dimming is opt-in: without it an overlay leaves the screen below untouched.
#[test]
fn an_overlay_does_not_dim_unless_asked() {
    xpui::testing::reset();

    let mut panel: OverlayPanel<()> = OverlayPanel::new(Text::new("Brightness"));
    panel.measure(screen_size());
    panel.render(Point::ORIGIN);

    assert!(
        !xpui::testing::drawn_rects()
            .iter()
            .any(|(_, _, _, _, kind)| *kind == RectKind::Scrim),
        "no scrim should be drawn by default"
    );
}

/// What looks dimmed and what dismisses on tap must be the same region — they
/// are derived from one another, and this pins that they stay so.
#[test]
fn the_dimmed_area_is_the_area_that_dismisses() {
    xpui::testing::reset();

    let mut panel = OverlayPanel::new(Text::new("Brightness"))
        .scrim(Scrim::Dim)
        .on_scrim_tap(());
    panel.measure(screen_size());
    panel.render(Point::ORIGIN);

    let scrim = xpui::testing::drawn_rects()
        .into_iter()
        .find(|(_, _, _, _, kind)| *kind == RectKind::Scrim)
        .expect("a dimmed panel draws a scrim");

    let mut interactions = Interactions::new(0);
    panel.interactions(Point::ORIGIN, &mut interactions);
    let tap = interactions
        .items()
        .first()
        .expect("a dismissable panel declares the region below it");

    assert_eq!(
        (scrim.0, scrim.1, scrim.2, scrim.3),
        (
            tap.rect.x(),
            tap.rect.y(),
            tap.rect.width(),
            tap.rect.height()
        ),
        "the dimmed strip and the dismiss target must coincide"
    );
}
