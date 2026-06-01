# SD-card theme creation

CrossPoint ships one built-in base theme, Lyra. Additional themes live on the SD card and are selected from Settings. A downloaded theme is just a folder containing a `theme.json` and optional assets such as 1-bit BMP icons.

CrossPoint ignores unknown JSON fields. Other readers, such as CrossInk, can add their own fields under a namespaced object like `extensions.crossink` without breaking CrossPoint.

## Folder layout

Manual install paths:

```text
/.themes/<theme-id>/theme.json   # hidden folder used by the downloader
/themes/<theme-id>/theme.json    # visible folder for manual installs
```

Hosted theme packages in this repo live under:

```text
sd-themes/<theme-id>/theme.json
sd-themes/<theme-id>/icons/*.bmp
```

Theme ids must be path-safe: letters, numbers, spaces, `-`, and `_` only. Avoid spaces for hosted themes because ids are used in URLs and settings.

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
      "font": "ui10",
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

- `topPadding`
- `headerHeight`
- `verticalSpacing`
- `contentSidePadding`
- `listRowHeight`
- `listWithSubtitleRowHeight`
- `menuRowHeight`
- `menuSpacing`
- `tabSpacing`
- `tabBarHeight`
- `scrollBarWidth`
- `scrollBarRightOffset`
- `homeTopPadding`
- `homeCoverHeight`
- `homeCoverTileHeight`
- `homeRecentBooksCount`
- `homeContinueReadingInMenu`
- `homeShowContinueReadingHeader`
- `homeMenuTopOffset`
- `buttonHintsHeight`
- `sideButtonHintsWidth`

Other supported metric groups:

- Battery: `batteryWidth`, `batteryHeight`, `batteryBarHeight`
- Reader progress/status: `progressBarHeight`, `progressBarMarginTop`, `statusBarHorizontalMargin`, `statusBarVerticalMargin`
- Keyboard: `keyboardKeyWidth`, `keyboardKeyHeight`, `keyboardKeySpacing`, `keyboardBottomKeyHeight`, `keyboardBottomKeySpacing`, `keyboardBottomAligned`, `keyboardCenteredText`, `keyboardVerticalOffset`, `keyboardTextFieldWidthPercent`, `keyboardWidthPercent`, `keyboardKeyCornerRadius`, `keyboardFillUnselected`, `keyboardOutlineAllUnselected`, `keyboardDrawSpecialOutlineWhenUnselected`, `keyboardSecondaryLabelRightPadding`, `keyboardSecondaryLabelTopPadding`, `keyboardMinArrowHeadSize`
- Popups: `popupTopOffsetRatio`, `popupMarginX`, `popupMarginY`, `popupFrameThickness`, `popupCornerRadius`, `popupTextBold`, `popupTextInverted`, `popupTextBaselineOffsetY`, `popupProgressBarHeight`, `popupProgressDrawOutline`, `popupProgressClampPercent`, `popupProgressFillInverted`, `popupProgressOutlineInverted`
- Text fields: `textFieldHorizontalPadding`, `textFieldNormalThickness`, `textFieldCursorThickness`, `textFieldLineEndOffset`

## Components

### Fonts

Most components accept:

```json
"font": "ui12",
"style": "bold"
```

Supported `font` values are `ui12`, `ui10`, and `small`. You can also use `fontId`, but named fonts are preferred. Supported `style` values are `regular` and `bold`.

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
        "font": "ui12",
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

### Home menu

`components.homeMenu` styles the home menu options.

Supported fields:

- `font`, `fontId`, `style`, `bold`
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

- `font`, `fontId`, `style`, `bold`
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

- `font`, `fontId`, `style`, `bold`
- `centeredTitle`
- `showDivider`
- `titleOffsetY`
- `batteryOffsetY`

### Tab bar

`components.tabBar` styles tabs.

Supported fields:

- `font`, `fontId`, `style`, `bold`
- `equalWidth`
- `selectionStyle`: `fill` or `underline`
- `selectedCornerRadius`
- `selectedTextInverted`
- `drawDivider`
- `horizontalInset`

### Button hints

`components.buttonHints` styles bottom and side button hints.

Supported fields:

- `font`, `fontId`, `style`, `bold`
- `layout`: `buttons`, `groups`, or `shapes`
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

Use `layout: "shapes"` for icon-only arrows/circle/square hints.

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
  --themes sd-themes
```

The script writes rotated BMP files into each `sd-themes/<theme-id>/icons/` folder.

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

After adding or changing hosted themes, regenerate `sd-themes/themes.json`:

```bash
python3 scripts/generate-theme-manifest.py \
  --root sd-themes \
  --base-url https://raw.githubusercontent.com/crosspoint-reader/crosspoint-reader/feat-sd-theme-system/sd-themes \
  --output sd-themes/themes.json
```

The manifest generator:

- scans every `sd-themes/<theme-id>/theme.json`
- includes every file in each theme folder
- writes per-file `size` and `crc32`
- writes the theme `id`, `name`, `description`, and `totalSize`

Commit the theme files and the regenerated manifest together.

## Validation checklist

Before publishing:

```bash
for f in sd-themes/themes.json sd-themes/*/theme.json; do
  python3 -m json.tool "$f" >/dev/null
done

python3 scripts/generate-theme-manifest.py \
  --root sd-themes \
  --base-url https://raw.githubusercontent.com/crosspoint-reader/crosspoint-reader/feat-sd-theme-system/sd-themes \
  --output sd-themes/themes.json

pio run -e gh_release
```

On device:

1. Download the theme from Settings -> UI Theme -> Download Themes.
2. Exit the downloader and let the device silently restart to clear WiFi/TLS heap.
3. Return to Settings -> UI Theme and select the downloaded theme.
4. Check Home, Settings, Browse, Recent Books, button hints, tabs, popups, keyboard, and reader menus.
