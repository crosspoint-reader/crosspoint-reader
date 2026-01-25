#pragma once

// Default theme - matches the original CrossPoint Reader look
// This is embedded in the firmware as a fallback

namespace ThemeEngine {

// Use static function for C++14 ODR compatibility
static const char* getDefaultThemeIni() {
  static const char* theme = R"INI(
; ============================================
; DEFAULT THEME - Original CrossPoint Reader
; ============================================
; Screen: 480x800
; Layout: Centered book card + vertical menu list

[Global]
FontUI12 = UI_12
FontUI10 = UI_10
NavBookCount = 1

; ============================================
; HOME SCREEN
; ============================================

[Home]
Type = Container
X = 0
Y = 0
Width = 480
Height = 800
BgColor = white

; --- Battery (top right) ---
[BatteryWrapper]
Parent = Home
Type = Container
X = 400
Y = 10
Width = 80
Height = 20

[BatteryIcon]
Parent = BatteryWrapper
Type = BatteryIcon
X = 0
Y = 5
Width = 15
Height = 20
Value = {BatteryPercent}
Color = black

[BatteryText]
Parent = BatteryWrapper
Type = Label
Font = Small
Text = {BatteryPercent}%
X = 22
Y = 0
Width = 50
Height = 20
Align = Left
Visible = {ShowBatteryPercent}

; --- Book Card (centered) ---
; Original: 240x400 at (120, 30)
[BookCard]
Parent = Home
Type = Container
X = 120
Y = 30
Width = 240
Height = 400
Border = true
BgColor = {IsBookSelected ? "black" : "white"}
Visible = {HasBook}

; Bookmark ribbon decoration (when no cover)
[BookmarkRibbon]
Parent = BookCard
Type = Container
X = 200
Y = 5
Width = 30
Height = 60
BgColor = {IsBookSelected ? "white" : "black"}
Visible = {!HasCover}

[BookmarkNotch]
Parent = BookmarkRibbon
Type = Container
X = 10
Y = 45
Width = 10
Height = 15
BgColor = {IsBookSelected ? "black" : "white"}

; Title centered in card
[BookTitle]
Parent = BookCard
Type = Label
Font = UI_12
Text = {BookTitle}
X = 20
Y = 150
Width = 200
Height = 60
Color = {IsBookSelected ? "white" : "black"}
Align = center
Ellipsis = true

[BookAuthor]
Parent = BookCard
Type = Label
Font = UI_10
Text = {BookAuthor}
X = 20
Y = 210
Width = 200
Height = 25
Color = {IsBookSelected ? "white" : "black"}
Align = center
Ellipsis = true

; "Continue Reading" at bottom of card
[ContinueLabel]
Parent = BookCard
Type = Label
Font = UI_10
Text = Continue Reading
X = 20
Y = 365
Width = 200
Height = 25
Color = {IsBookSelected ? "white" : "black"}
Align = center

; --- No Book Message ---
[NoBookCard]
Parent = Home
Type = Container
X = 120
Y = 30
Width = 240
Height = 400
Border = true
Visible = {!HasBook}

[NoBookTitle]
Parent = NoBookCard
Type = Label
Font = UI_12
Text = No open book
X = 20
Y = 175
Width = 200
Height = 25
Align = center

[NoBookSubtitle]
Parent = NoBookCard
Type = Label
Font = UI_10
Text = Start reading below
X = 20
Y = 205
Width = 200
Height = 25
Align = center

; --- Menu List ---
; Original: margin=20, tileWidth=440, tileHeight=45, spacing=8
; menuStartY = 30 + 400 + 15 = 445
[MenuList]
Parent = Home
Type = List
Source = MainMenu
ItemTemplate = MenuItem
X = 20
Y = 445
Width = 440
Height = 280
Direction = Vertical
ItemHeight = 45
Spacing = 8

; --- Menu Item Template ---
[MenuItem]
Type = Container
Width = 440
Height = 45
BgColor = {Item.Selected ? "black" : "white"}
Border = true

[MenuItemLabel]
Parent = MenuItem
Type = Label
Font = UI_10
Text = {Item.Title}
X = 0
Y = 0
Width = 440
Height = 45
Color = {Item.Selected ? "white" : "black"}
Align = center

; --- Button Hints (bottom) ---
; Original: 4 buttons at [25, 130, 245, 350], width=106, height=40
; Y = pageHeight - 40 = 760

[HintBtn2]
Parent = Home
Type = Container
X = 130
Y = 760
Width = 106
Height = 40
BgColor = white
Border = true

[HintBtn2Label]
Parent = HintBtn2
Type = Label
Font = UI_10
Text = Confirm
X = 0
Y = 0
Width = 106
Height = 40
Align = center

[HintBtn3]
Parent = Home
Type = Container
X = 245
Y = 760
Width = 106
Height = 40
BgColor = white
Border = true

[HintBtn3Label]
Parent = HintBtn3
Type = Label
Font = UI_10
Text = Up
X = 0
Y = 0
Width = 106
Height = 40
Align = center

[HintBtn4]
Parent = Home
Type = Container
X = 350
Y = 760
Width = 106
Height = 40
BgColor = white
Border = true

[HintBtn4Label]
Parent = HintBtn4
Type = Label
Font = UI_10
Text = Down
X = 0
Y = 0
Width = 106
Height = 40
Align = center

)INI";
  return theme;
}

}  // namespace ThemeEngine
