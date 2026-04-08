# スリープ画面カレンダー表示 実装プラン

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** スリープ画面に当月カレンダーをオーバーレイ描画するオプション機能を追加する（Issue #36）

**Architecture:** 既存の6種類のスリープ画面モード上にカレンダーをオーバーレイ描画する。SleepActivityの各renderメソッドから`displayBuffer()`呼び出しを分離し、カレンダー描画→一括表示の流れにリファクタする。設定はSettingsList経由のJSON自動処理パターンに従う。

**Tech Stack:** C++20, ESP-IDF/Arduino-ESP32, GfxRenderer API, PlatformIO

**Spec:** `docs/superpowers/specs/2026-04-08-sleep-calendar-design.md`

---

### Task 1: I18n翻訳文字列の追加

**Files:**
- Modify: `lib/I18n/translations/english.yaml`
- Modify: `lib/I18n/translations/japanese.yaml`

- [ ] **Step 1: english.yamlに文字列キーを追加**

ファイル末尾に以下を追加:

```yaml
STR_SLEEP_CALENDAR: "Calendar"
STR_SLEEP_CALENDAR_POSITION: "Calendar Position"
STR_CALENDAR_POS_TOP: "Top"
STR_CALENDAR_POS_CENTER: "Center"
STR_CALENDAR_POS_BOTTOM: "Bottom"
STR_SUN: "Sun"
STR_MON: "Mon"
STR_TUE: "Tue"
STR_WED: "Wed"
STR_THU: "Thu"
STR_FRI: "Fri"
STR_SAT: "Sat"
```

- [ ] **Step 2: japanese.yamlに文字列キーを追加**

ファイル末尾に以下を追加:

```yaml
STR_SLEEP_CALENDAR: "カレンダー"
STR_SLEEP_CALENDAR_POSITION: "カレンダー配置"
STR_CALENDAR_POS_TOP: "上部"
STR_CALENDAR_POS_CENTER: "中央"
STR_CALENDAR_POS_BOTTOM: "下部"
STR_SUN: "日"
STR_MON: "月"
STR_TUE: "火"
STR_WED: "水"
STR_THU: "木"
STR_FRI: "金"
STR_SAT: "土"
```

- [ ] **Step 3: I18nヘッダを再生成**

Run: `python3 scripts/gen_i18n.py lib/I18n/translations lib/I18n/`
Expected: 生成ファイル（`I18nKeys.h`, `I18nStrings.h`, `I18nStrings.cpp`）が更新される

- [ ] **Step 4: ビルドして文字列キーが認識されることを確認**

Run: `pio run 2>&1 | tail -5`
Expected: ビルド成功（0 errors）

- [ ] **Step 5: コミット**

```bash
git add lib/I18n/translations/english.yaml lib/I18n/translations/japanese.yaml
git commit -m "✨ カレンダー表示用のI18n文字列を追加（Issue #36）"
```

---

### Task 2: CrossPointSettingsに設定フィールドを追加

**Files:**
- Modify: `src/CrossPointSettings.h:159-164` (sleepScreen設定の近く)
- Modify: `src/SettingsList.h:99-107` (スリープ画面設定の直後)

- [ ] **Step 1: CrossPointSettings.hに設定フィールドとenumを追加**

`src/CrossPointSettings.h` の `SLEEP_SCREEN_COVER_FILTER` enumの後（行35付近）に追加:

```cpp
  enum SLEEP_CALENDAR_POSITION {
    CALENDAR_POS_TOP = 0,
    CALENDAR_POS_CENTER = 1,
    CALENDAR_POS_BOTTOM = 2,
    SLEEP_CALENDAR_POSITION_COUNT
  };
```

既存のスリープ画面設定フィールド（行160-164）の直後に追加:

```cpp
  // Sleep calendar overlay
  uint8_t sleepCalendar = 0;  // 0=OFF, 1=ON
  uint8_t sleepCalendarPosition = CALENDAR_POS_CENTER;
```

- [ ] **Step 2: SettingsList.hにカレンダー設定項目を追加**

`src/SettingsList.h` の `sleepScreenCoverFilter` 設定の直後（行107付近）に追加:

```cpp
      SettingInfo::Toggle(StrId::STR_SLEEP_CALENDAR, &CrossPointSettings::sleepCalendar, "sleepCalendar",
                          StrId::STR_CAT_DISPLAY),
      SettingInfo::Enum(StrId::STR_SLEEP_CALENDAR_POSITION, &CrossPointSettings::sleepCalendarPosition,
                        {StrId::STR_CALENDAR_POS_TOP, StrId::STR_CALENDAR_POS_CENTER, StrId::STR_CALENDAR_POS_BOTTOM},
                        "sleepCalendarPosition", StrId::STR_CAT_DISPLAY),
```

- [ ] **Step 3: ビルド確認**

Run: `pio run 2>&1 | tail -5`
Expected: ビルド成功

- [ ] **Step 4: コミット**

```bash
git add src/CrossPointSettings.h src/SettingsList.h
git commit -m "✨ カレンダー表示の設定フィールドを追加（Issue #36）"
```

---

### Task 3: 設定UIでカレンダーOFF時に配置設定をスキップ

**Files:**
- Modify: `src/activities/settings/SettingsActivity.cpp:208-212` (toggleCurrentSetting内)

- [ ] **Step 1: toggleCurrentSetting内でカレンダーOFF時に配置設定をスキップ**

`src/activities/settings/SettingsActivity.cpp` の `toggleCurrentSetting()` メソッド内、`SettingType::ENUM && valuePtr != nullptr` ブランチの先頭（行209付近、Font Sizeスキップの直前）に追加:

```cpp
    // Calendar Position: skip when calendar is disabled
    if (setting.nameId == StrId::STR_SLEEP_CALENDAR_POSITION && SETTINGS.sleepCalendar == 0) {
      return;
    }
```

- [ ] **Step 2: ビルド確認**

Run: `pio run 2>&1 | tail -5`
Expected: ビルド成功

- [ ] **Step 3: コミット**

```bash
git add src/activities/settings/SettingsActivity.cpp
git commit -m "👍 カレンダーOFF時にカレンダー配置設定を無効化（Issue #36）"
```

---

### Task 4: SleepActivityのリファクタ（描画とdisplayBufferの分離）

**Files:**
- Modify: `src/activities/boot_sleep/SleepActivity.h`
- Modify: `src/activities/boot_sleep/SleepActivity.cpp`

- [ ] **Step 1: SleepActivity.hのメソッドシグネチャを変更**

`renderBitmapSleepScreen` に `bool skipDisplay` パラメータを追加し、`displaySleepScreen` ヘルパーを追加:

```cpp
class SleepActivity final : public Activity {
 public:
  explicit SleepActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Sleep", renderer, mappedInput) {}
  void onEnter() override;

 private:
  void renderDefaultSleepScreen() const;
  void renderCustomSleepScreen() const;
  void renderCoverSleepScreen() const;
  void renderBitmapSleepScreen(const Bitmap& bitmap, bool skipDisplay = false) const;
  void renderBlankSleepScreen() const;
  void displaySleepScreen() const;
};
```

- [ ] **Step 2: displaySleepScreenメソッドを追加**

`src/activities/boot_sleep/SleepActivity.cpp` に `displaySleepScreen` を追加:

```cpp
void SleepActivity::displaySleepScreen() const {
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}
```

- [ ] **Step 3: 各renderメソッドからdisplayBuffer呼び出しを除去**

`renderDefaultSleepScreen()` の末尾（行149）:
- `renderer.displayBuffer(HalDisplay::HALF_REFRESH);` を `displaySleepScreen();` に変更

`renderBitmapSleepScreen()` の BW描画部分（行204）:
- `renderer.displayBuffer(HalDisplay::HALF_REFRESH);` を以下に変更:
```cpp
  if (!skipDisplay) {
    displaySleepScreen();
  }
```
- グレースケール部分（行219-220）の `renderer.displayGrayBuffer();` の直前に `displaySleepScreen();` を追加（BWパス表示後にグレースケール）→ ここは変更不要、グレースケールはdisplayGrayBufferで独立表示

`renderBlankSleepScreen()` の末尾（行307）:
- `renderer.displayBuffer(HalDisplay::HALF_REFRESH);` を `displaySleepScreen();` に変更

- [ ] **Step 4: onEnter()のフロー変更（カレンダー対応準備）**

`onEnter()` のswitch文直後、`renderer.setDarkMode(wasDarkMode);` の直前に、今はまだカレンダー描画は入れない（次のTaskで追加）。ただし、各renderメソッドがdisplaySleepScreen()を呼ぶようにリファクタが完了していることを確認する。

- [ ] **Step 5: ビルド確認**

Run: `pio run 2>&1 | tail -5`
Expected: ビルド成功（既存動作は変わらない）

- [ ] **Step 6: コミット**

```bash
git add src/activities/boot_sleep/SleepActivity.h src/activities/boot_sleep/SleepActivity.cpp
git commit -m "🎨 SleepActivityの描画とdisplayBufferを分離（Issue #36）"
```

---

### Task 5: カレンダー描画ロジックの実装

**Files:**
- Modify: `src/activities/boot_sleep/SleepActivity.h`
- Modify: `src/activities/boot_sleep/SleepActivity.cpp`

- [ ] **Step 1: SleepActivity.hにrenderCalendarOverlayを追加**

privateセクションに追加:

```cpp
  void renderCalendarOverlay() const;
  static bool isTimeValid();
```

- [ ] **Step 2: isTimeValid()を実装**

```cpp
bool SleepActivity::isTimeValid() {
  // 2024-01-01 00:00:00 UTC = 1704067200
  return time(nullptr) >= 1704067200;
}
```

- [ ] **Step 3: renderCalendarOverlay()を実装**

`src/activities/boot_sleep/SleepActivity.cpp` に `#include <ctime>` を追加し、以下を実装:

```cpp
void SleepActivity::renderCalendarOverlay() const {
  // 現在時刻を取得
  time_t now = time(nullptr);
  struct tm timeInfo;
  localtime_r(&now, &timeInfo);

  const int year = timeInfo.tm_year + 1900;
  const int month = timeInfo.tm_mon + 1;
  const int today = timeInfo.tm_mday;

  // 今月の日数を計算
  // 翌月1日の前日 = 今月末日
  struct tm nextMonth = {};
  nextMonth.tm_year = timeInfo.tm_year;
  nextMonth.tm_mon = timeInfo.tm_mon + 1;
  nextMonth.tm_mday = 1;
  mktime(&nextMonth);
  nextMonth.tm_mday -= 1;
  mktime(&nextMonth);
  const int daysInMonth = nextMonth.tm_mday;

  // 今月1日の曜日を計算（0=日曜）
  struct tm firstDay = {};
  firstDay.tm_year = timeInfo.tm_year;
  firstDay.tm_mon = timeInfo.tm_mon;
  firstDay.tm_mday = 1;
  mktime(&firstDay);
  const int startDow = firstDay.tm_wday;  // 0=Sunday

  // レイアウト定数
  static constexpr int COL_WIDTH = 48;
  static constexpr int ROW_HEIGHT = 40;
  static constexpr int HEADER_HEIGHT = 30;
  static constexpr int DOW_HEIGHT = 28;
  static constexpr int PADDING_X = 24;
  static constexpr int PADDING_TOP = 20;
  static constexpr int PADDING_BOTTOM = 24;
  static constexpr int GRID_WIDTH = COL_WIDTH * 7;

  // 行数を計算
  const int totalCells = startDow + daysInMonth;
  const int numRows = (totalCells + 6) / 7;

  const int calendarHeight = PADDING_TOP + HEADER_HEIGHT + DOW_HEIGHT + ROW_HEIGHT * numRows + PADDING_BOTTOM;
  const int calendarWidth = GRID_WIDTH + PADDING_X * 2;

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();

  // X位置: 画面中央
  const int calX = (screenWidth - calendarWidth) / 2;

  // Y位置: 設定に応じて上部/中央/下部
  int calY;
  int viewTop, viewRight, viewBottom, viewLeft;
  renderer.getOrientedViewableTRBL(&viewTop, &viewRight, &viewBottom, &viewLeft);

  switch (SETTINGS.sleepCalendarPosition) {
    case CrossPointSettings::CALENDAR_POS_TOP:
      calY = viewTop + 20;
      break;
    case CrossPointSettings::CALENDAR_POS_BOTTOM:
      calY = screenHeight - viewBottom - calendarHeight - 20;
      break;
    case CrossPointSettings::CALENDAR_POS_CENTER:
    default:
      calY = (screenHeight - calendarHeight) / 2;
      break;
  }

  // 白背景矩形を描画
  renderer.fillRect(calX, calY, calendarWidth, calendarHeight, true);  // true = white

  // 年月ヘッダー描画
  char headerBuf[16];
  snprintf(headerBuf, sizeof(headerBuf), "%d / %d", year, month);
  const int headerY = calY + PADDING_TOP;
  renderer.drawCenteredText(UI_10_FONT_ID, headerY, headerBuf, false, EpdFontFamily::BOLD);

  // 曜日ヘッダー描画
  const StrId dowIds[] = {StrId::STR_SUN, StrId::STR_MON, StrId::STR_TUE, StrId::STR_WED,
                          StrId::STR_THU, StrId::STR_FRI, StrId::STR_SAT};
  const int dowY = headerY + HEADER_HEIGHT;
  const int gridLeft = calX + PADDING_X;

  for (int col = 0; col < 7; col++) {
    const char* dowStr = I18N.get(dowIds[col]);
    const int textW = renderer.getTextWidth(SMALL_FONT_ID, dowStr, EpdFontFamily::BOLD);
    const int cellCenterX = gridLeft + col * COL_WIDTH + (COL_WIDTH - textW) / 2;
    renderer.drawText(SMALL_FONT_ID, cellCenterX, dowY, dowStr, false, EpdFontFamily::BOLD);
  }

  // 日付グリッド描画
  const int gridStartY = dowY + DOW_HEIGHT;
  const int circleRadius = 16;

  for (int day = 1; day <= daysInMonth; day++) {
    const int cellIndex = startDow + day - 1;
    const int row = cellIndex / 7;
    const int col = cellIndex % 7;
    const bool isSunday = (col == 0);
    const bool isToday = (day == today);

    char dayBuf[4];
    snprintf(dayBuf, sizeof(dayBuf), "%d", day);

    const auto style = (isSunday || isToday) ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const int textW = renderer.getTextWidth(UI_10_FONT_ID, dayBuf, style);
    const int cellCenterX = gridLeft + col * COL_WIDTH + COL_WIDTH / 2;
    const int cellCenterY = gridStartY + row * ROW_HEIGHT + ROW_HEIGHT / 2;

    if (isToday) {
      // 黒丸を描画（fillRoundedRectで円を近似）
      renderer.fillRoundedRect(cellCenterX - circleRadius, cellCenterY - circleRadius,
                               circleRadius * 2, circleRadius * 2, circleRadius,
                               GfxRenderer::Color::Black);
      // 白文字で日付を描画
      const int textX = cellCenterX - textW / 2;
      const int textH = renderer.getTextHeight(UI_10_FONT_ID);
      const int textY = cellCenterY - textH / 2;
      renderer.drawText(UI_10_FONT_ID, textX, textY, dayBuf, true, style);
    } else {
      // 通常の黒文字で日付を描画
      const int textX = cellCenterX - textW / 2;
      const int textH = renderer.getTextHeight(UI_10_FONT_ID);
      const int textY = cellCenterY - textH / 2;
      renderer.drawText(UI_10_FONT_ID, textX, textY, dayBuf, false, style);
    }
  }
}
```

- [ ] **Step 4: onEnter()にカレンダーオーバーレイ呼び出しを追加**

`onEnter()` の switch文の後、`renderer.setDarkMode(wasDarkMode);` の前に追加:

```cpp
  // カレンダーオーバーレイ（設定ON＋時刻有効時のみ）
  if (SETTINGS.sleepCalendar && isTimeValid()) {
    renderCalendarOverlay();
  }
```

- [ ] **Step 5: ビルド確認**

Run: `pio run 2>&1 | tail -5`
Expected: ビルド成功

- [ ] **Step 6: コミット**

```bash
git add src/activities/boot_sleep/SleepActivity.h src/activities/boot_sleep/SleepActivity.cpp
git commit -m "✨ スリープ画面カレンダー描画ロジックを実装（Issue #36）"
```

---

### Task 6: 最終ビルド検証とdiff確認

- [ ] **Step 1: クリーンビルド**

Run: `pio run -t clean && pio run 2>&1 | tail -10`
Expected: ビルド成功（0 errors, 0 warnings）

- [ ] **Step 2: 差分確認**

Run: `git diff HEAD~4 --stat`
Expected: 以下のファイルが変更されている
- `lib/I18n/translations/english.yaml`
- `lib/I18n/translations/japanese.yaml`
- `src/CrossPointSettings.h`
- `src/SettingsList.h`
- `src/activities/settings/SettingsActivity.cpp`
- `src/activities/boot_sleep/SleepActivity.h`
- `src/activities/boot_sleep/SleepActivity.cpp`

- [ ] **Step 3: 生成ファイルがコミットに含まれていないことを確認**

Run: `git diff HEAD~4 --name-only | grep -E '\.(generated\.h|I18nKeys|I18nStrings)'`
Expected: 出力なし（生成ファイルはgitignore対象）

## Done 判定基準

- [ ] `pio run` がエラー・警告なしでビルド成功
- [ ] 設定画面にカレンダー表示ON/OFFとカレンダー配置の設定項目が追加されている
- [ ] カレンダーOFF時にカレンダー配置設定が操作不可
- [ ] SleepActivityのonEnter()でカレンダーオーバーレイが描画される
- [ ] 時刻未設定時（NTP未同期）はカレンダーが表示されない
- [ ] 差分が意図通りで、生成ファイルやシークレットが含まれていない
※ 実機での表示確認は人間テスター範囲（デバイスフラッシュ後に確認）
