//! Interaction regression tests.
//!
//! Widgets declare their interactive regions in a walk that mirrors `render`.
//! Nothing in the type system keeps those two in step, so these tests are what
//! catch a container whose implementations have drifted apart — the failure
//! mode being touches that land on the control next door.

use cpui::testing::{MIN_TOUCH_SIZE, SCREEN_HEIGHT, SCREEN_WIDTH};
use cpui::{
    hstack, value_at, vstack, Alignment, HStack, InputMask, Interactions, List, ListRow, Modifiers,
    Point, Size, Slider, Spacer, Stepper, Text, Toggle, VStack, View,
};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Msg {
    First,
    Second,
    Third,
    Value(i32),
    Step(i32),
    Toggled(bool),
}

fn screen() -> Size {
    Size::new(SCREEN_WIDTH, SCREEN_HEIGHT)
}

/// Lays a tree out and collects its interactions, as the runtime does.
fn collect(view: &mut dyn View<Msg>, focus: usize) -> Interactions<Msg> {
    cpui::testing::install();
    view.measure(screen());
    let mut out = Interactions::new(focus);
    view.interactions(Point::ORIGIN, &mut out);
    out
}

/// The message a touch at `point` produces, scanning back to front so the
/// innermost control wins — the same order the runtime uses.
fn touch(interactions: &Interactions<Msg>, point: Point, kind: InputMask) -> Option<Msg> {
    interactions
        .items()
        .iter()
        .rev()
        .find(|item| item.mask.contains(kind) && item.rect.contains(point))
        .map(|item| item.trigger.resolve(item.rect, point.x))
}

/// A touch must resolve through nested containers to the control actually under
/// it, not to whichever sibling was declared first.
#[test]
fn a_nested_tree_resolves_to_the_control_under_the_touch() {
    let mut tree: VStack<Msg> = vstack![10;
        Text::new("Heading"),
        hstack![8;
            Text::new("left").on_tap(Msg::First),
            Text::new("middle").on_tap(Msg::Second),
            Text::new("right").on_tap(Msg::Third),
        ],
    ];
    let interactions = collect(&mut tree, 0);

    let heading_height = {
        let mut heading = Text::new("Heading");
        View::<Msg>::measure(&mut heading, screen());
        View::<Msg>::size(&heading).height
    };
    let row_y = heading_height + 10;

    let mut cursor = 0;
    for (label, expected) in [
        ("left", Msg::First),
        ("middle", Msg::Second),
        ("right", Msg::Third),
    ] {
        let mut probe = Text::new(label);
        View::<Msg>::measure(&mut probe, screen());
        let width = View::<Msg>::size(&probe).width;
        let point = Point::new(cursor + width / 2, row_y + 2);

        assert_eq!(
            touch(&interactions, point, InputMask::TAP),
            Some(expected),
            "touch at {point:?} hit the wrong control"
        );
        cursor += width + 8;
    }
}

/// A touch on nothing interactive produces nothing, rather than the nearest
/// control — otherwise tapping a screen's background would fire something.
#[test]
fn a_miss_produces_nothing() {
    let mut tree: VStack<Msg> =
        vstack![10; Text::new("label"), Text::new("tap").on_tap(Msg::First)];
    let interactions = collect(&mut tree, 0);

    assert_eq!(
        touch(
            &interactions,
            Point::new(SCREEN_WIDTH - 1, SCREEN_HEIGHT - 1),
            InputMask::TAP
        ),
        None
    );
}

/// A view with no `on_tap` declares nothing at all.
#[test]
fn an_untagged_view_is_not_interactive() {
    let mut text = Text::new("just a label");
    let interactions = collect(&mut text, 0);
    assert!(interactions.is_empty());
}

/// Held frames must reach a slider and nothing else. This is the property that
/// stops a finger resting on a button re-firing it every tick.
#[test]
fn only_a_drag_control_accepts_held_frames() {
    let mut row: HStack<Msg> = hstack![8;
        Text::new("x").on_tap(Msg::First),
        Slider::new(50, 100).on_change(Msg::Value).flexible(),
    ];
    let interactions = collect(&mut row, 0);

    let button = interactions
        .items()
        .iter()
        .find(|item| matches!(item.trigger.resolve(item.rect, 0), Msg::First))
        .expect("the button should declare an interaction");
    assert!(
        !button.mask.contains(InputMask::DRAG),
        "a plain button must not receive held frames"
    );

    let slider = interactions
        .items()
        .iter()
        .find(|item| item.mask.contains(InputMask::DRAG))
        .expect("a slider must accept drags");
    assert!(slider.mask.contains(InputMask::TAP), "and taps too");
}

/// The rect a slider declares must be the one `value_at` was written against,
/// or a drag maps to the wrong percentage.
#[test]
fn a_slider_touch_converts_to_the_expected_value() {
    let mut slider = Slider::new(0, 100).on_change(Msg::Value);
    let interactions = collect(&mut slider, 0);

    let item = &interactions.items()[0];
    let rect = item.rect;

    assert_eq!(item.trigger.resolve(rect, rect.x()), Msg::Value(0));
    assert_eq!(
        item.trigger.resolve(rect, rect.x() + rect.width()),
        Msg::Value(100)
    );

    let middle = value_at(rect, rect.x() + rect.width() / 2, 100);
    assert!(
        (48..=52).contains(&middle),
        "the midpoint should read as about half, got {middle}"
    );
}

/// A list declares one interaction per interactive row, in reading order, so a
/// touch produces that row's own message.
#[test]
fn a_list_row_produces_its_own_message() {
    let mut list: List<Msg> = List::new()
        .push(ListRow::new("First").on_tap(Msg::First))
        .push(ListRow::new("Second").on_tap(Msg::Second))
        .push(ListRow::new("Third").on_tap(Msg::Third));
    let interactions = collect(&mut list, 0);

    assert_eq!(interactions.focusable_count(), 3);

    let height = View::<Msg>::size(&list).height / 3;
    for (index, expected) in [Msg::First, Msg::Second, Msg::Third]
        .into_iter()
        .enumerate()
    {
        let point = Point::new(20, index as i32 * height + height / 2);
        assert_eq!(touch(&interactions, point, InputMask::TAP), Some(expected));
    }
}

/// A read-out row carries no message, so it neither responds nor takes focus.
#[test]
fn a_row_without_a_message_is_inert() {
    let mut list: List<Msg> = List::new()
        .push(ListRow::new("read only"))
        .push(ListRow::new("tap me").on_tap(Msg::First));
    let interactions = collect(&mut list, 0);

    assert_eq!(interactions.focusable_count(), 1);
}

/// A glyph narrower than a fingertip must still be reachable: the hit area
/// grows without moving what is drawn.
#[test]
fn a_small_control_still_gets_a_full_size_touch_target() {
    let mut glyph = Text::new("-").on_tap(Msg::First);
    let interactions = collect(&mut glyph, 0);

    let declared = interactions.items()[0].rect;
    assert!(
        declared.width() >= MIN_TOUCH_SIZE,
        "a tiny control should be widened to {MIN_TOUCH_SIZE}, got {}",
        declared.width()
    );
    assert!(
        View::<Msg>::size(&glyph).width < MIN_TOUCH_SIZE,
        "and the drawn size must stay small"
    );
}

/// Focus follows tree order, and a widget learns its own state as it declares
/// itself — which is what lets a list highlight the focused row with no screen
/// code at all.
#[test]
fn focus_follows_tree_order() {
    let build = || -> List<Msg> {
        List::new()
            .push(ListRow::new("a").on_tap(Msg::First))
            .push(ListRow::new("b").on_tap(Msg::Second))
            .push(ListRow::new("c").on_tap(Msg::Third))
    };

    for focus in 0..3 {
        let mut list = build();
        let interactions = collect(&mut list, focus);
        assert_eq!(interactions.focusable_count(), 3);

        // The focused entry is the one Confirm would fire.
        let expected = [Msg::First, Msg::Second, Msg::Third][focus];
        let item = &interactions.items()[focus];
        assert_eq!(item.trigger.resolve(item.rect, 0), expected);
    }
}

/// A tap and Confirm on the same control must produce the identical message,
/// so touch and buttons are never subtly different.
#[test]
fn a_tap_and_confirm_agree() {
    let mut list: List<Msg> = List::new().push(ListRow::new("only").on_tap(Msg::Second));
    let interactions = collect(&mut list, 0);

    let item = &interactions.items()[0];
    let by_confirm = item.trigger.resolve(item.rect, 0);
    let centre = Point::new(
        item.rect.x() + item.rect.width() / 2,
        item.rect.y() + item.rect.height() / 2,
    );
    let by_touch = touch(&interactions, centre, InputMask::TAP).unwrap();

    assert_eq!(by_confirm, by_touch);
}

/// A toggle reports the state it is moving to, so a screen never writes `!x`.
#[test]
fn a_toggle_reports_the_next_state() {
    for current in [false, true] {
        let mut toggle = Toggle::new("Hyphenation", current, "On", "Off").on_change(Msg::Toggled);
        let interactions = collect(&mut toggle, 0);

        let item = &interactions.items()[0];
        assert_eq!(item.trigger.resolve(item.rect, 0), Msg::Toggled(!current));
    }
}

/// A stepper's end glyphs carry the sign, and its track carries the value.
#[test]
fn a_stepper_declares_steps_and_a_track() {
    let mut stepper = Stepper::new(50).on_change(Msg::Value).on_step(Msg::Step);
    let interactions = collect(&mut stepper, 0);

    let messages: Vec<Msg> = interactions
        .items()
        .iter()
        .map(|item| item.trigger.resolve(item.rect, item.rect.x()))
        .collect();

    assert!(messages.contains(&Msg::Step(-1)), "expected a minus step");
    assert!(messages.contains(&Msg::Step(1)), "expected a plus step");
    assert!(
        messages.iter().any(|m| matches!(m, Msg::Value(_))),
        "expected a draggable track"
    );
}

/// Centre alignment moves children across the axis; the interaction walk has to
/// apply the same offset `render` does, or touches miss by half the row.
#[test]
fn centre_alignment_moves_the_touch_area_with_the_drawing() {
    let mut row: HStack<Msg> = hstack![8;
        Slider::new(50, 100),
        Text::new("x").on_tap(Msg::First),
    ]
    .align(Alignment::Center);
    let interactions = collect(&mut row, 0);

    let declared = interactions.items()[0].rect;
    let row_height = View::<Msg>::size(&row).height;

    // Assert on the centre, not the edges: the glyph is shorter than the
    // minimum touch target, so its declared area is grown around it and may
    // legitimately start above the row. What centring guarantees is that the
    // two midpoints line up.
    let declared_centre = declared.y() + declared.height() / 2;
    let row_centre = row_height / 2;
    assert!(
        (declared_centre - row_centre).abs() <= 1,
        "a centred child's touch area should sit on the row's midline: \
         child centre {declared_centre}, row centre {row_centre}"
    );
}

/// A spacer declares nothing, so it can never swallow a touch meant for a
/// sibling.
#[test]
fn a_spacer_is_never_interactive() {
    let mut row: HStack<Msg> = hstack![8; Text::new("a"), Spacer::new(), Text::new("b")];
    let interactions = collect(&mut row, 0);
    assert!(interactions.is_empty());
}

/// A stepper is **one** focus stop, not three. Before this, Up/Down walked
/// through the `-` glyph, the track and the `+` glyph instead of moving between
/// settings — which made button navigation useless on the frontlight panel.
#[test]
fn a_stepper_is_a_single_focus_stop() {
    let mut stepper = Stepper::new(50).on_change(Msg::Value).on_step(Msg::Step);
    let interactions = collect(&mut stepper, 0);

    assert_eq!(
        interactions.focusable_count(),
        1,
        "a stepper should be one stop for buttons, however many touch targets it has"
    );
    assert!(
        interactions.items().len() > 1,
        "and it should still offer separate touch targets"
    );
}

/// Two steppers are two stops, so Up/Down move between settings.
#[test]
fn stacked_steppers_are_one_stop_each() {
    let mut panel: VStack<Msg> = vstack![10;
        Stepper::new(60).on_change(Msg::Value).on_step(Msg::Step),
        Stepper::new(40).on_change(Msg::Value).on_step(Msg::Step),
    ];
    let interactions = collect(&mut panel, 0);
    assert_eq!(interactions.focusable_count(), 2);
}

/// Left/Right nudge whatever holds focus, so one pair of keys drives every
/// adjustable control.
#[test]
fn the_focused_stepper_takes_the_nudge() {
    let mut stepper = Stepper::new(50).on_change(Msg::Value).on_step(Msg::Step);
    let interactions = collect(&mut stepper, 0);

    let adjustable = interactions
        .items()
        .iter()
        .find(|item| item.mask.contains(InputMask::ADJUST))
        .expect("a stepper must accept adjustment");

    assert_eq!(adjustable.trigger.resolve_step(-1), Some(Msg::Step(-1)));
    assert_eq!(adjustable.trigger.resolve_step(1), Some(Msg::Step(1)));
}

/// A touch-only control stays out of the focus order, so buttons never stop on
/// something they cannot activate.
#[test]
fn a_touch_only_control_is_not_focusable() {
    let mut row: HStack<Msg> = hstack![8;
        Text::new("icon").on_touch(Msg::First),
        Text::new("row").on_tap(Msg::Second),
    ];
    let interactions = collect(&mut row, 0);

    assert_eq!(interactions.focusable_count(), 1, "only the tappable row");
    assert_eq!(interactions.items().len(), 2, "but both still take a touch");
}
