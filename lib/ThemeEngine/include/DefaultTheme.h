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

[BatteryContainer]
Parent = StatusBar
Type = Container
X = 400
Y = 3
Width = 28
Height = 18

[BatteryIcon]
Parent = BatteryContainer
Type = Icon
Src = battery
X = 0
Y = 0
Width = 28
Height = 18
Color = black

; Fill bar inside the battery (positioned inside the battery outline)
[BatteryFill]
Parent = BatteryContainer
Type = ProgressBar
X = 2
Y = 4
Width = 20
Height = 10
Value = {BatteryPercent}
Max = 100
FgColor = black
BgColor = white
ShowBorder = false

[BatteryLabel]
Parent = StatusBar
Type = Label
Font = UI_10
Text = {BatteryPercent}%
X = 432
Y = 3
Width = 48
Height = 18

; --- Recent Books Section ---
[RecentBooksSection]
Parent = Home
Type = Container
X = 0
Y = 30
Width = 100%
Height = 280
Visible = {HasRecentBooks}

[RecentBooksList]
Parent = RecentBooksSection
Type = List
Source = RecentBooks
ItemTemplate = RecentBookItem
X = 10
Y = 0
Width = 460
Height = 280
Direction = Horizontal
ItemWidth = 149
ItemHeight = 270
Spacing = 8

; --- Recent Book Item Template ---
; Based on mockup: 74.5px at 240px scale = 149px at 480px
[RecentBookItem]
Type = Container
Width = 149
Height = 270
Padding = 8
BgColor = {Item.Selected ? "0xD9" : "white"}
BorderRadius = 12

[BookCoverImage]
Parent = RecentBookItem
Type = Bitmap
X = 0
Y = 0
Width = 133
Height = 190
Src = {Item.Image}

[BookProgressBadge]
Parent = RecentBookItem
Type = Badge
X = 4
Y = 224
Text = {Item.Progress}%
Font = UI_10
BgColor = black
FgColor = white
PaddingH = 6
PaddingV = 3

[BookTitleLabel]
Parent = RecentBookItem
Type = Label
Font = UI_10
Text = {Item.Title}
X = 0
Y = 196
Width = 133
Height = 22
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
Padding = 16
BgColor = {Item.Selected ? "0xD9" : "white"}
BorderRadius = 12

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

; --- Bottom Navigation Bar ---
; Positioned at the very bottom of screen, buttons align with physical buttons
[NavBar]
Parent = Home
Type = Container
X = 0
Y = 776
Width = 100%
Height = 24

; Left button group (OK and Back) - aligned with left physical buttons
[NavLeftGroup]
Parent = NavBar
Type = HStack
X = 120
Y = 0
Width = 168
Height = 24
Spacing = 8

[NavBtnBack]
Parent = NavLeftGroup
Type = Container
Width = 80
Height = 8
Border = true
BorderRadius = 8

[NavBtnOK]
Parent = NavLeftGroup
Type = Container
Width = 80
Height = 24
Border = true
BorderRadius = 8

[NavBtnOKIcon]
Parent = NavBtnOK
Type = Icon
Src = check
X = 30
Y = 6
Width = 20
Height = 12

; Right button group (Up and Down) - aligned with right physical buttons
[NavRightGroup]
Parent = NavBar
Type = HStack
X = 292
Y = 0
Width = 168
Height = 24
Spacing = 8

[NavBtnUp]
Parent = NavRightGroup
Type = Container
Width = 80
Height = 24
Border = true
BorderRadius = 8

[NavBtnUpIcon]
Parent = NavBtnUp
Type = Icon
Src = up
X = 30
Y = 6
Width = 20
Height = 12

[NavBtnDown]
Parent = NavRightGroup
Type = Container
Width = 80
Height = 24
Border = true
BorderRadius = 8

[NavBtnDownIcon]
Parent = NavBtnDown
Type = Icon
Src = down
X = 30
Y = 6
Width = 20
Height = 12

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
