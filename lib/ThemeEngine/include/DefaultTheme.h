#pragma once

constexpr const char *DEFAULT_THEME_INI = R"(
; ============================================
; Default Theme for CrossPoint Reader
; ============================================

[Global]
FontUI12 = UI_12
FontUI10 = UI_10

; ============================================
; HOME SCREEN
; ============================================

[Home]
Type = Container
X = 0
Y = 0
Width = 100%
Height = 100%
Color = white

; --- Status Bar ---
[StatusBar]
Parent = Home
Type = Container
X = 0
Y = 10
Width = 100%
Height = 24

[BatteryIcon]
Parent = StatusBar
Type = Icon
Src = battery
X = 400
Width = 28
Height = 18
Color = black

[BatteryLabel]
Parent = StatusBar
Type = Label
Font = UI_10
Text = {BatteryPercent}%
X = 432
Width = 48
Height = 20

; --- Recent Books Section ---
[RecentBooksSection]
Parent = Home
Type = Container
X = 0
Y = 45
Width = 100%
Height = 260
Visible = {HasRecentBooks}

[RecentBooksList]
Parent = RecentBooksSection
Type = List
Source = RecentBooks
ItemTemplate = RecentBookItem
X = 15
Y = 0
Width = 450
Height = 260
Direction = Horizontal
ItemWidth = 145
Spacing = 10

; --- Recent Book Item Template ---
[RecentBookItem]
Type = Container
Width = 145
Height = 250

[BookCoverContainer]
Parent = RecentBookItem
Type = Container
X = 0
Y = 0
Width = 145
Height = 195
Border = {Item.Selected}

[BookCoverImage]
Parent = BookCoverContainer
Type = Bitmap
X = 2
Y = 2
Width = 141
Height = 191
Src = {Item.Image}
Cacheable = true

[BookProgressBadge]
Parent = BookCoverContainer
Type = Badge
X = 5
Y = 168
Text = {Item.Progress}%
Font = UI_10
BgColor = black
FgColor = white
PaddingH = 6
PaddingV = 2

[BookTitleLabel]
Parent = RecentBookItem
Type = Label
Font = UI_10
Text = {Item.Title}
X = 0
Y = 200
Width = 145
Height = 40
Ellipsis = true

; --- No Recent Books State ---
[EmptyBooksMessage]
Parent = Home
Type = Label
Font = UI_12
Text = No recent books
Centered = true
X = 0
Y = 100
Width = 480
Height = 30
Visible = {!HasRecentBooks}

[EmptyBooksSub]
Parent = Home
Type = Label
Font = UI_10
Text = Open a book to start reading
Centered = true
X = 0
Y = 130
Width = 480
Height = 30
Visible = {!HasRecentBooks}

; --- Main Menu (2-column grid) ---
[MainMenuList]
Parent = Home
Type = List
Source = MainMenu
ItemTemplate = MainMenuItem
X = 15
Y = 330
Width = 450
Height = 350
Columns = 2
ItemHeight = 70
Spacing = 20

; --- Menu Item Template ---
[MainMenuItem]
Type = HStack
Width = 210
Height = 65
Spacing = 12
CenterVertical = true
Border = {Item.Selected}

[MenuItemIcon]
Parent = MainMenuItem
Type = Icon
Src = {Item.Icon}
Width = 36
Height = 36
Color = black

[MenuItemLabel]
Parent = MainMenuItem
Type = Label
Font = UI_12
Text = {Item.Title}
Width = 150
Height = 40
Color = black

; --- Bottom Hint Bar ---
[HintBar]
Parent = Home
Type = HStack
X = 60
Y = 760
Width = 360
Height = 30
Spacing = 80
CenterVertical = true

[HintSelect]
Parent = HintBar
Type = Icon
Src = check
Width = 24
Height = 24

[HintUp]
Parent = HintBar
Type = Icon
Src = up
Width = 24
Height = 24

[HintDown]
Parent = HintBar
Type = Icon
Src = down
Width = 24
Height = 24

; ============================================
; SETTINGS SCREEN
; ============================================

[Settings]
Type = Container
X = 0
Y = 0
Width = 100%
Height = 100%
Color = white

[SettingsTitle]
Parent = Settings
Type = Label
Font = UI_12
Text = Settings
X = 15
Y = 15
Width = 200
Height = 30

[SettingsTabBar]
Parent = Settings
Type = TabBar
X = 0
Y = 50
Width = 100%
Height = 40
Selected = {SelectedTab}
IndicatorHeight = 3
ShowIndicator = true

[TabReading]
Parent = SettingsTabBar
Type = Label
Font = UI_10
Text = Reading
Centered = true
Height = 35

[TabControls]
Parent = SettingsTabBar
Type = Label
Font = UI_10
Text = Controls
Centered = true
Height = 35

[TabDisplay]
Parent = SettingsTabBar
Type = Label
Font = UI_10
Text = Display
Centered = true
Height = 35

[TabSystem]
Parent = SettingsTabBar
Type = Label
Font = UI_10
Text = System
Centered = true
Height = 35

[SettingsList]
Parent = Settings
Type = List
Source = SettingsItems
ItemTemplate = SettingsItem
X = 0
Y = 95
Width = 450
Height = 650
ItemHeight = 50
Spacing = 0

[SettingsScrollIndicator]
Parent = Settings
Type = ScrollIndicator
X = 460
Y = 100
Width = 15
Height = 640
Position = {ScrollPosition}
Total = {TotalItems}
VisibleCount = {VisibleItems}
TrackWidth = 4

; --- Settings Item Template ---
[SettingsItem]
Type = Container
Width = 450
Height = 48
Border = false

[SettingsItemBg]
Parent = SettingsItem
Type = Rectangle
X = 0
Y = 0
Width = 450
Height = 45
Fill = {Item.Selected}
Color = black

[SettingsItemLabel]
Parent = SettingsItem
Type = Label
Font = UI_10
Text = {Item.Title}
X = 15
Y = 0
Width = 250
Height = 45
Color = {Item.Selected ? white : black}

[SettingsItemValue]
Parent = SettingsItem
Type = Label
Font = UI_10
Text = {Item.Value}
X = 270
Y = 0
Width = 120
Height = 45
Align = Right
Color = {Item.Selected ? white : black}

[SettingsItemToggle]
Parent = SettingsItem
Type = Toggle
X = 390
Y = 8
Width = 50
Height = 30
Value = {Item.ToggleValue}
Visible = {Item.HasToggle}

[SettingsItemDivider]
Parent = SettingsItem
Type = Divider
X = 15
Y = 46
Width = 420
Height = 1
Horizontal = true
Color = 0x80
)";
