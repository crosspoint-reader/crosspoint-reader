# SD-card theme creation

CrossPoint ships one built-in base theme, Lyra. Additional themes live on the SD card and are selected from Settings. A downloaded theme is just a folder containing a `theme.json` and optional assets such as 1-bit BMP icons.

CrossPoint ignores unknown JSON fields. Other readers, such as CrossInk, can add their own fields under a namespaced object like `extensions.crossink` without breaking CrossPoint.

## Folder layout

Manual install paths:

```text
/.themes/<theme-id>/theme.json   # hidden folder used by the downloader
/themes/<theme-id>/theme.json    # visible folder for manual installs
```

Hosted theme packages live in the tools repo under:

```text
../crosspoint-tools/public/themes/<theme-id>/theme.json
../crosspoint-tools/public/themes/<theme-id>/icons/*.bmp
```

Theme ids must be path-safe: letters, numbers, `-`, and `_` only. Spaces are not accepted because ids are used in folder names, URLs, and settings.

## Minimal theme

```json
{
  "schema": 1,
  "id": "my-theme",
  "name": "My Theme",
  "description": "Short user-facing description shown in the downloader.",
  "inherits": "lyra",
  "metrics": {
    "homeTopPadding": 48,
    "menuRowHeight": 42
  },
  "components": {
    "homeMenu": {
      "font": "medium",
      "style": "regular",
      "centeredText": true,
      "selectionStyle": "underline",
      "showIcons": false
    }
  },
  "devices": {
    "x3": {
      "constraints": {
        "screenWidth": 480,
        "screenHeight": 800,
        "frontButtons": 4,
        "sideButtons": "up-down"
      }
    },
    "x4": {
      "constraints": {
        "screenWidth": 480,
        "screenHeight": 800,
        "frontButtons": 0,
        "sideButtons": "up-down"
      }
    }
  }
}
```

Top-level fields:

- `schema`: currently `1`.
- `id`: stable id used for settings, folder name, and downloads.
- `name`: display name shown in Settings and the downloader.
- `description`: short downloader text.
- `inherits`: `lyra` for normal SD themes. `classic` is accepted for manually installed themes that intentionally build from the Classic renderer.
- `metrics`: layout numbers shared across screens.
- `components`: style rules for themeable UI surfaces.
- `assets.icons`: optional icon file map.
- `devices`: optional per-device overrides keyed by `x3` or `x4`.
- `requires`: optional metadata for other tooling. CrossPoint currently ignores it.
- `extensions`: optional namespaced metadata for other firmware/apps. CrossPoint currently ignores it.

## Device overrides

The active device id is `x3` or `x4`. Any supported field under `devices.<device-id>` overrides the top-level value:

```json
{
  "metrics": {
    "homeCoverHeight": 300
  },
  "components": {
    "homeRecents": {
      "maxBooks": 3
    }
  },
  "devices": {
    "x3": {
      "metrics": {
        "homeCoverHeight": 280
      },
      "components": {
        "homeRecents": {
          "maxBooks": 3
        }
      }
    }
  }
}
```

Use `constraints` to document intended screen and button assumptions for builders and compatible apps:

```json
"constraints": {
  "screenWidth": 480,
  "screenHeight": 800,
  "frontButtons": 4,
  "sideButtons": "up-down"
}
```

CrossPoint parses these constraints but does not reject themes when they do not match.

## Metrics

Metrics tune global spacing and layout. Any omitted metric keeps Lyra's default.

Common home/list metrics:

- `topPadding`: top inset above normal page headers.
- `headerHeight`: default header band height for non-home screens.
- `verticalSpacing`: default vertical gap between major screen regions.
- `contentSidePadding`: left/right inset used by default list and menu renderers.
- `listRowHeight`: row height for single-line lists.
- `listWithSubtitleRowHeight`: row height for two-line lists such as Recent Books.
- `menuRowHeight`: height of one home menu tile. In `launcherGrid`, this is used by `drawButtonMenu` inside each grid cell; it is not the gap between grid cells.
- `menuSpacing`: vertical spacing between items when rendering a plain one-column home menu. It does not affect `launcherGrid`, because each grid cell is rendered as a one-item menu.
- `tabSpacing`: spacing between tab labels.
- `tabBarHeight`: height of the settings tab bar.
- `scrollBarWidth`: list scrollbar width.
- `scrollBarRightOffset`: list scrollbar inset from the right edge.
- `homeTopPadding`: top inset before the home cover/recent-books area in the legacy home renderer.
- `homeCoverHeight`: cover image height used by home recents.
- `homeCoverTileHeight`: total home recents tile/slot height, including cover title space when applicable.
- `homeRecentBooksCount`: number of recent books to request/render on home.
- `homeContinueReadingInMenu`: whether Continue Reading is part of the home launcher/menu actions.
- `homeShowContinueReadingHeader`: whether the current book title can appear in the home header.
- `homeMenuTopOffset`: legacy/manual home menu offset below the cover area. SD `screens.home.layout` themes should prefer explicit layout slots such as `carouselMenuGap`.
- `buttonHintsHeight`: bottom button-hint band height.
- `sideButtonHintsWidth`: side button-hint band width.

Other supported metric groups:

- Battery: `batteryWidth`, `batteryHeight`, `batteryBarHeight`
- Reader progress/status: `progressBarHeight`, `progressBarMarginTop`, `statusBarHorizontalMargin`, `statusBarVerticalMargin`
- Keyboard: `keyboardKeyWidth`, `keyboardKeyHeight`, `keyboardKeySpacing`, `keyboardBottomKeyHeight`, `keyboardBottomKeySpacing`, `keyboardBottomAligned`, `keyboardCenteredText`, `keyboardVerticalOffset`, `keyboardTextFieldWidthPercent`, `keyboardWidthPercent`, `keyboardKeyCornerRadius`, `keyboardFillUnselected`, `keyboardOutlineAllUnselected`, `keyboardDrawSpecialOutlineWhenUnselected`, `keyboardSecondaryLabelRightPadding`, `keyboardSecondaryLabelTopPadding`, `keyboardMinArrowHeadSize`
- Popups: `popupTopOffsetRatio`, `popupMarginX`, `popupMarginY`, `popupFrameThickness`, `popupCornerRadius`, `popupTextBold`, `popupTextInverted`, `popupTextBaselineOffsetY`, `popupProgressBarHeight`, `popupProgressDrawOutline`, `popupProgressClampPercent`, `popupProgressFillInverted`, `popupProgressOutlineInverted`
- Text fields: `textFieldHorizontalPadding`, `textFieldNormalThickness`, `textFieldCursorThickness`, `textFieldLineEndOffset`

## Screen Layouts

Themes can define `screens.<screen>.layout` to place UI regions with the SDK row/column layout system. This is the preferred path for new SD themes.

Each layout node can contain:

- `id`: slot name used by widgets or firmware renderers.
- `axis`: `column` stacks children top-to-bottom; `row` lays children left-to-right.
- `gap`: pixels inserted between this node's direct children.
- `slots`: child layout nodes.
- `fixed`: exact pixel size along the parent axis.
- `flex`: proportional size after fixed children and gaps are subtracted.
- `token`: named size from `metrics`, such as `menuRow`, `recents`, `buttons`, `header`, `row`, `subtitleRow`, or `gap`.

Example:

```json
"screens": {
  "home": {
    "navigation": "linear",
    "layout": {
      "axis": "column",
      "gap": 0,
      "slots": [
        {
          "id": "header",
          "fixed": 40,
          "axis": "row",
          "gap": 4,
          "slots": [
            { "id": "homeClock", "fixed": 52 },
            { "id": "homeTitle", "flex": 1 },
            { "id": "homeBattery", "fixed": 66 }
          ]
        },
        { "id": "recents", "fixed": 340 },
        { "id": "carouselMenuGap", "fixed": 36 },
        { "id": "launchers", "fixed": 192 },
        { "id": "homeSpacer", "flex": 1 },
        { "id": "buttons", "fixed": 40 }
      ]
    }
  }
}
```

Important layout rules:

- A parent layout's `gap` only affects its direct child slots.
- `fixed` and `flex` decide how much space a slot receives. They do not decide how a widget draws inside that slot.
- Widget-specific `gap` fields control spacing inside that widget.
- Named spacer slots such as `carouselMenuGap` and `homeSpacer` do not draw anything unless a widget targets them. They are useful for placing visible regions without manual `x`/`y` coordinates.
- If a screen layout is invalid or missing required slots, CrossPoint falls back to the built-in Lyra-safe layout for that screen.

Home `navigation` modes:

- `linear`: default. Front/side navigation buttons all move through the visible home actions as one ordered list.
- `splitAxis`: front left/right move through launcher actions; side up/down move through recent-book actions. Bottom button hints show Left/Right.
- `carousel`: front left/right move through recent-book actions; side up/down move through launcher actions. Use this when left/right should stay inside a cover carousel and up/down should enter or leave the launcher menu.

Home `initialAction` can optionally choose the default selected action when entering home normally:

```json
"initialAction": "reader:recent"
```

Supported values match launcher `action` values. Explicit firmware navigation, such as returning to Settings from a settings submenu, still overrides this default.

### Layouts vs widgets

Layouts only create named rectangles. They do not choose whether a screen is a list, cover grid, carousel, or any other presentation.

Widgets choose what renders inside those rectangles. This keeps themes explicit and prevents firmware from guessing a grid just because a screen has a `list` slot.

For `screens.recentBooks`, use:

- No `recentBooks` screen: use the built-in Lyra recent-books screen.
- `layout` only, or a `list` widget: use the normal themed recent-books list in the `list` slot.
- A `coverGrid` widget: use FreeInkUI's cover-grid component in the target slot.

Minimal themed list example:

```json
"recentBooks": {
  "layout": {
    "axis": "column",
    "gap": 8,
    "slots": [
      { "id": "header", "fixed": 48 },
      { "id": "list", "flex": 1 },
      { "id": "buttons", "fixed": 40 }
    ]
  },
  "widgets": [
    { "slot": "list", "type": "list" }
  ]
}
```

Cover-grid screen example:

```json
"recentBooks": {
  "layout": {
    "axis": "column",
    "gap": 16,
    "slots": [
      { "id": "header", "fixed": 48 },
      { "id": "list", "flex": 1 },
      { "id": "buttons", "fixed": 40 }
    ]
  },
  "widgets": [
    {
      "slot": "list",
      "type": "coverGrid",
      "columns": 3,
      "rowGap": 36,
      "coverWidth": 92,
      "coverHeight": 132,
      "rowHeight": 172,
      "labelLines": 2,
      "selectionStyle": "coverFrame"
    }
  ]
}
```

Do not use screen-level `coverGrid`. Cover-grid settings belong on a widget with `type: "coverGrid"`.

### Home widgets

Home layouts use `screens.home.widgets` to map slot rectangles to visible content.

Supported widget types:

- `clock`: draws the clock when the device has RTC support. On devices without clock support, the slot stays empty.
- `headerTitle`: draws the normal home/header title.
- `battery`: draws the battery indicator.
- `recents`: draws the configured home recents component.
- `recentCoverGrid`: draws recent books with FreeInkUI's `coverGrid` component.
- `launcherList`: draws actions as one vertical menu inside its slot.
- `launcherGrid`: draws actions in a row/column grid inside its slot.
- `buttonHints`: draws bottom button hints.

`launcherGrid` fields:

- `slot`: slot id to render into.
- `presentation`: optional presentation style. Use `iconTabs` for icon-only launcher tabs with outlined unselected cells and filled selected cells.
- `columns`: number of grid columns.
- `rows`: optional fixed row count. If omitted, rows are derived from visible launcher count and columns.
- `gap`: pixels between grid cells, both horizontally and vertically.
- `items`: launcher actions. Each item accepts `text`, `icon`, and `action`.

All home widgets also support visual placement fields:

- `layer`: draw order. Lower layers draw first; higher layers paint on top. Widgets with the same layer keep JSON order.
- `offsetX`: moves the widget right after layout. Negative values move left.
- `offsetY`: moves the widget down after layout. Negative values move up.
- `bleed`: expands the widget draw rectangle outside its slot without changing layout. Use either a single number or `{ "top": 0, "right": 0, "bottom": 0, "left": 0 }`.
- `inset`: shrinks the widget draw rectangle inside its slot without changing layout. Use either a single number or `{ "top": 0, "right": 0, "bottom": 0, "left": 0 }`.

Example overlap:

```json
{
  "slot": "recents",
  "type": "recents",
  "layer": 0,
  "bleed": { "bottom": 24 }
},
{
  "slot": "launchers",
  "type": "launcherGrid",
  "layer": 10,
  "offsetY": -12,
  "columns": 2,
  "gap": 24
}
```

That keeps the structural row/column layout intact, but lets the launcher grid visually overlap the recents area by 12 pixels.

`buttonHints` widget fields:

- `labels.confirm`
- `labels.previous`
- `labels.next`
- `labels.back`

Button-hint labels are localized semantic tokens, not literal UI strings. Supported tokens are `default`, `empty`, `back`, `home`, `select`, `confirm`, `open`, `toggle`, `up`, `down`, `left`, and `right`. `default` uses the firmware fallback for that navigation mode; `empty` renders no label for that button.

Example carousel hints:

```json
{
  "slot": "buttons",
  "type": "buttonHints",
  "labels": {
    "confirm": "select",
    "previous": "left",
    "next": "right"
  }
}
```

When `components.buttonHints.layout` is `shapes` or `icons`, these same localized tokens render as button shapes/icons where supported.

Example icon tabs:

```json
{
  "slot": "tabs",
  "type": "launcherGrid",
  "presentation": "iconTabs",
  "columns": 5,
  "rows": 1,
  "gap": 6,
  "iconSize": 32,
  "selectedRadius": 5,
  "items": [
    { "icon": "folder", "action": "activity:fileBrowser" },
    { "icon": "recent", "action": "activity:recentBooks" },
    { "icon": "library", "action": "activity:opds" }
  ]
}
```

Launcher actions:

- `activity:fileBrowser`
- `activity:recentBooks`
- `activity:opds`
- `activity:fileTransfer`
- `activity:settings`
- `activity:reader`

For `launcherGrid`, the final cell height is:

```text
(slot height - gap * (rows - 1)) / rows
```

Then each cell calls the themed home menu renderer with one item. That means:

- Increase the widget `gap` to create more visible space between grid items.
- Increase the launcher slot `fixed` height if larger gaps need more total room.
- Use `menuRowHeight` to tune the selectable tile/text/icon band inside each cell.
- Do not expect `menuSpacing` to change `launcherGrid` spacing.

For a 3-row launcher grid with `menuRowHeight: 48` and `gap: 24`, use a launcher slot near:

```text
3 * 48 + 2 * 24 = 192
```

`recentCoverGrid` / recent-books `coverGrid` widget fields:

- `slot`: slot id to render into.
- `columns`: grid columns.
- `rows`: grid rows.
- `gap`: horizontal pixels between cells. Also used vertically when `rowGap` is omitted.
- `rowGap`: vertical pixels between cover-grid rows.
- `cellInset`: optional padding inside each cover-grid cell, before the cover and label are drawn.
- `labelInset`: optional padding inside the title label area. Use `{ "left": 5, "right": 5 }` to keep two-line titles away from cell edges.
- `coverWidth`: rendered cover width.
- `coverHeight`: rendered cover height and thumbnail size to generate.
- `placeholderIconSize`: maximum icon size for the missing-cover placeholder.
- `rowHeight`: height of each cell row, including label space.
- `labelHeight`: title label area below each cover. Use `0` to hide titles.
- `labelGap`: vertical pixels between the cover and title label block.
- `labelLines`: maximum title lines to render. Increase `rowHeight` when this is greater than `1`.
- `selectionStyle`: `fill`, `outline`, `coverFrame`, or `none`. Prefer `coverFrame` for cover grids because it frames only the thumbnail and does not depend on title wrapping.
- `startIndex`: first recent-book index to show. Use `2` when a featured area already uses the first two books.

These cover-grid widgets use FreeInkUI's `coverGrid` for layout, labels, cell styling, and selected state. CrossPoint supplies a cover painter callback so SD-card thumbnails render from the existing recent-book cache.

Cover widgets can use different visual `coverWidth` and `coverHeight` values on different screens. CrossPoint still generates and reads one largest-needed thumbnail height for the active theme, then scales/crops it into each widget. That keeps the same book cover available on home and recent-books instead of requiring separate BMPs per widget.

`featuredBookCard` fields:

- `coverWidth`, `coverHeight`: rendered cover size and thumbnail height to generate.
- `placeholderIconSize`: maximum icon size for the missing-cover placeholder.
- `coverGap`: horizontal gap between the cover and title/author text.
- `titleGap`: vertical gap below the Continue Reading label before the book card starts.
- `startIndex`: recent-book index to show.

## Components

### Fonts

Most components accept:

```json
"font": "large",
"style": "bold"
```

Supported `font` values are `small`, `medium`, and `large`.

Semantic aliases are also accepted:

- `chrome`, `caption`: same as `small`.
- `body`, `label`: same as `medium`.
- `title`, `display`: same as `large`.

Supported `style` values are `regular` and `bold`.

### Home recents

`components.homeRecents` controls the home cover area.

Supported types:

- `default`: Lyra default.
- `none`: no cover area.
- `cover-strip`: one or more cover slots.

Example:

```json
"homeRecents": {
  "type": "cover-strip",
  "maxBooks": 3,
  "wrap": true,
  "selectionLineWidth": 3,
  "inactiveSelectionLineWidth": 1,
  "selectionCornerRadius": 6,
  "slots": [
    {
      "book": "previous",
      "x": "padding",
      "y": "center",
      "height": 210,
      "widthPercent": 62
    },
    {
      "book": "selected",
      "x": "center",
      "y": "top",
      "height": 280,
      "widthPercent": 62,
      "selected": true,
      "title": {
        "enabled": true,
        "font": "large",
        "style": "bold",
        "maxLines": 2,
        "offsetY": 12
      }
    },
    {
      "book": "next",
      "x": "right-padding",
      "y": "center",
      "height": 210,
      "widthPercent": 62
    }
  ]
}
```

Slot fields:

- `book`: `selected`, `previous`, `next`, or `index`.
- `bookIndex`: zero-based index when `book` is `index`.
- `x`: `padding`, `center`, or `right-padding`.
- `y`: `top` or `center`.
- `height`: requested thumbnail height. CrossPoint generates/cache-misses thumbnails at requested sizes.
- `widthPercent`: cover width as a percent of the slot height.
- `xOffset`, `yOffset`: positional adjustments.
- `selected`: whether this slot receives the active selection outline.
- `title`: optional book title under the cover.

CrossPoint currently reads up to five cover slots.

Cover slots with `selected: true` draw after unselected slots, so selected covers appear in front. Within each group, slots draw in the same order they appear in JSON. For a carousel where the side covers sit behind the middle cover, mark the middle slot as `selected: true`.

Use `xOffset` and `yOffset` for small relative adjustments after `x`/`y` placement has been resolved:

- Positive `xOffset` moves a cover right.
- Negative `xOffset` moves a cover left.
- Positive `yOffset` moves a cover down.
- Negative `yOffset` moves a cover up.

Example carousel layering:

```json
"slots": [
  {
    "book": "previous",
    "x": "padding",
    "y": "center",
    "height": 225,
    "widthPercent": 62,
    "xOffset": 32
  },
  {
    "book": "next",
    "x": "right-padding",
    "y": "center",
    "height": 225,
    "widthPercent": 62,
    "xOffset": -32
  },
  {
    "book": "selected",
    "x": "center",
    "y": "top",
    "height": 300,
    "widthPercent": 62,
    "selected": true
  }
]
```

In that example, the side covers are pushed toward the center, and the selected cover is drawn in the foreground.

### Home menu

`components.homeMenu` styles the home menu options.

Supported fields:

- `font`, `style`, `bold`
- `centeredText`
- `centerVertically`
- `showIcons`
- `panelWidth`
- `drawPanel`
- `panelCornerRadius`
- `selectionStyle`: `fill`, `outline`, `triangle`, `underline`, or `pill`
- `selectionCornerRadius`
- `selectionInset`
- `selectedTextInverted`
- `selectionFillBlack`
- `rowPaddingX`
- `textInsetX`

### Lists

`components.list` styles Settings, Browse, Recent Books, and similar list rows.

Supported fields:

- `font`, `style`, `bold`
- `subtitleFontId`
- `valueFontId`
- `showIcons`
- `iconSize`
- `textGap`
- `selectionStyle`: `fill`, `outline`, or `underline`
- `selectionCornerRadius`
- `selectionFill`
- `selectionOutline`
- `selectedTextInverted`
- `rowBackgrounds`
- `centerSingleLineRows`
- `subtitleRowAutoHeight`
- `centerValueVertically`
- `rowSidePadding`
- `rowGap`
- `textInsetX`
- `selectionInsetX`
- `selectionInsetY`
- `titleOffsetY`
- `subtitleOffsetY`
- `subtitleTopPadding`
- `subtitleBottomPadding`
- `subtitleInterLineGap`
- `valueOffsetY`
- `subtitleValueOffsetY`
- `iconOffsetY`

### Header

`components.header` styles page headers.

Supported fields:

- `font`, `style`, `bold`
- `centeredTitle`
- `showDivider`
- `titleOffsetY`
- `batteryOffsetY`

### Tab bar

`components.tabBar` styles tabs.

Supported fields:

- `font`, `style`, `bold`
- `equalWidth`
- `selectionStyle`: `fill` or `underline`
- `selectedCornerRadius`
- `selectedTextInverted`
- `drawDivider`
- `horizontalInset`

### Button hints

`components.buttonHints` styles bottom and side button hints.

Supported fields:

- `font`, `style`, `bold`
- `layout`: `buttons`, `groups`, `shapes`, or `icons`
- `buttonWidth`
- `smallButtonHeight`
- `cornerRadius`
- `fill`
- `outline`
- `drawEmpty`
- `shapes`
- `sidePadding`
- `groupGap`
- `bottomMargin`
- `innerPadding`
- `shapeSize`
- `textOffsetY`

Use `layout: "shapes"` or `layout: "icons"` for icon-only arrows/circle/square hints.

### Reader chrome

`screens.reader.chrome` styles the reader status lane. Reader chrome still uses `screens.reader.layout` slots for placement; the chrome object controls how those slots draw.

Battery fields:

- `style`: `icon` or `bar`.
- `width`: battery glyph width in pixels.
- `height`: battery glyph height in pixels.
- `offsetY`: vertical adjustment applied after the battery is positioned in its slot. Positive values move it down; negative values move it up.
- `track`: background/track style for bar batteries: `none`, `hairline`, `outline`, or `dither`.
- `fill`: fill style for bar batteries: `solid`, `dither`, or `segments`.
- `direction`: fill direction: `left-to-right`, `right-to-left`, `center-out`, `bottom-to-top`, or `top-to-bottom`.
- `orientation`: `horizontal` or `vertical`. Vertical is also implied by `bottom-to-top` and `top-to-bottom`.
- `caps`: `square` or `pixel`. `pixel` trims the four filled corners for a softer e-ink cap.
- `segments`: number of filled blocks when `fill` is `segments`.
- `segmentGap`: pixels between segments.
- `radius`: rounded-rect radius for bar track/fill/segments. Keep this small for thin e-ink bars; `0` is square.
- `showPercentage`: whether reader chrome may draw the battery percentage when the global setting allows it.

Example:

```json
"screens": {
  "reader": {
    "layout": {
      "axis": "row",
      "gap": 8,
      "slots": [
        { "id": "bookmark", "fixed": 18 },
        { "id": "battery", "fixed": 38 },
        { "id": "title", "flex": 1 },
        { "id": "clock", "fixed": 42 },
        { "id": "progress", "fixed": 82 }
      ]
    },
    "chrome": {
      "battery": {
        "style": "bar",
        "width": 38,
        "height": 3,
        "offsetY": 1,
        "track": "none",
        "fill": "solid",
        "direction": "left-to-right",
        "radius": 0,
        "showPercentage": false
      }
    }
  }
}
```

## Icons

Icons are optional. If both `homeMenu.showIcons` and `list.showIcons` are false, omit `assets.icons` and the icon files to reduce download size and heap use.

Supported icon keys:

- `folder`, `folder24`
- `text`, `text24`
- `image`, `image24`
- `book`, `book24`
- `file`, `file24`
- `recent`
- `settings`, `settings2`
- `transfer`
- `library`
- `wifi`
- `hotspot`
- `bookmark`

Generate firmware-matching 1-bit BMP icons:

```bash
python3 scripts/generate-theme-icons.py \
  --icons src/components/icons \
  --themes ../crosspoint-tools/public/themes
```

The script writes rotated BMP files into each `../crosspoint-tools/public/themes/<theme-id>/icons/` folder.

Reference them from `theme.json`:

```json
"assets": {
  "icons": {
    "folder": "icons/folder.bmp",
    "book": "icons/book.bmp",
    "settings": "icons/settings2.bmp"
  }
}
```

## CrossInk and extension fields

CrossPoint only consumes the fields documented above. Unknown fields are ignored, so theme authors can include extra data for compatible apps and firmware.

Put app-specific fields under `extensions.<namespace>`:

```json
{
  "schema": 1,
  "id": "crossink-stats",
  "name": "CrossInk Stats",
  "inherits": "lyra",
  "components": {
    "homeRecents": {
      "type": "cover-strip",
      "maxBooks": 1
    }
  },
  "extensions": {
    "crossink": {
      "schema": 1,
      "readingStats": {
        "enabled": true,
        "placement": "home-footer",
        "font": "small",
        "style": "regular",
        "show": [
          "currentStreak",
          "readingTime",
          "pagesRead",
          "percentComplete"
        ],
        "labels": {
          "currentStreak": "streak",
          "readingTime": "reading",
          "pagesRead": "pages"
        }
      }
    }
  }
}
```

Recommended extension rules:

- Keep CrossPoint layout fields in `metrics`, `components`, `assets`, and `devices`.
- Keep CrossInk-only fields under `extensions.crossink`.
- Add an extension-local `schema` when the app-specific format may evolve.
- Prefer declarative fields such as `placement`, `font`, `show`, and `labels` over code-like strings.
- Keep extension data compact. CrossPoint ignores it, but it is still parsed transiently when discovering themes.
- Do not put required CrossPoint behavior only in an extension field. CrossPoint will not read it.

CrossInk can also use `requires` for compatibility metadata:

```json
"requires": {
  "crosspoint": {
    "schema": 1,
    "modules": ["cover-strip"]
  },
  "crossink": {
    "schema": 1,
    "modules": ["reading-stats"]
  }
}
```

CrossPoint currently treats `requires` as metadata.

## Package manifest

After adding or changing hosted themes, regenerate `themes.json` in `crosspoint-tools`:

```bash
python3 scripts/generate-theme-manifest.py \
  --root ../crosspoint-tools/public/themes \
  --base-url http://crosspointreader.com/themes \
  --output ../crosspoint-tools/public/themes/themes.json
```

The manifest generator:

- scans every `../crosspoint-tools/public/themes/<theme-id>/theme.json`
- includes every file in each theme folder
- writes per-file `size` and `crc32`
- writes the theme `id`, `name`, `description`, and `totalSize`

Commit the theme files and the regenerated manifest together in `crosspoint-tools`.

## Validation checklist

Before publishing:

```bash
for f in ../crosspoint-tools/public/themes/themes.json ../crosspoint-tools/public/themes/*/theme.json; do
  python3 -m json.tool "$f" >/dev/null
done

python3 scripts/generate-theme-manifest.py \
  --root ../crosspoint-tools/public/themes \
  --base-url http://crosspointreader.com/themes \
  --output ../crosspoint-tools/public/themes/themes.json

pio run -e gh_release
```

On device:

1. Download the theme from Settings -> UI Theme -> Download Themes.
2. Exit the downloader and let the device silently restart to clear WiFi/TLS heap.
3. Return to Settings -> UI Theme and select the downloaded theme.
4. Check Home, Settings, Browse, Recent Books, button hints, tabs, popups, keyboard, and reader menus.
