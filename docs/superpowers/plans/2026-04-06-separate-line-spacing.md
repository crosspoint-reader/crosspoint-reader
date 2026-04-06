# 行間隔の縦書き/横書き別設定 実装プラン

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `lineSpacing` を `lineSpacingHorizontal` / `lineSpacingVertical` に分離し、縦書き/横書きでそれぞれ独立した行間隔を設定できるようにする。

**Architecture:** CrossPointSettings のメンバ変数を2つに分離し、`getReaderLineCompression(bool vertical)` で方向に応じた値を返す。UI は Settings 画面に2項目、リーダーメニューでは現在の書字方向に応じた1項目のみ表示。キャッシュ機構は既存の仕組みをそのまま活用。

**Tech Stack:** C++20 (PlatformIO / ESP32-C3), ArduinoJson, YAML i18n

**Spec:** `docs/superpowers/specs/2026-04-06-separate-line-spacing-design.md`

---

## File Map

| ファイル | 操作 | 責務 |
|---------|------|------|
| `lib/I18n/translations/*.yaml` (全22言語) | Modify | i18n文字列追加 |
| `src/CrossPointSettings.h` | Modify | メンバ変数分離、メソッドシグネチャ変更 |
| `src/CrossPointSettings.cpp` | Modify | `getReaderLineCompression(bool)` 実装、レガシー移行 |
| `src/SettingsList.h` | Modify | 2エントリに分離 |
| `src/JsonSettingsIO.cpp` | Modify | 旧キー移行ロジック追加 |
| `src/activities/reader/EpubReaderMenuActivity.h` | Modify | コンストラクタに `verticalMode` 追加 |
| `src/activities/reader/EpubReaderMenuActivity.cpp` | Modify | 行間隔の表示・書き込みを `verticalMode` で分岐 |
| `src/activities/reader/EpubReaderActivity.cpp` | Modify | `getReaderLineCompression(verticalMode)` 呼び出し、メニュー生成に `verticalMode` 追加 |
| `src/activities/settings/SettingsActivity.cpp` | Modify | `STR_LINE_SPACING` の条件分岐を2つの新キーに更新 |

---

### Task 1: i18n文字列の追加

**Files:**
- Modify: `lib/I18n/translations/english.yaml`
- Modify: `lib/I18n/translations/japanese.yaml`
- Modify: 残り20言語の `*.yaml`

- [ ] **Step 1: english.yaml に新キーを追加**

`STR_LINE_SPACING` の直後に以下を追加:

```yaml
STR_LINE_SPACING_HORIZONTAL: "Line Spacing (Horizontal)"
STR_LINE_SPACING_VERTICAL: "Line Spacing (Vertical)"
```

既存の `STR_LINE_SPACING` は削除しない（リーダーメニューで引き続き使用）。

- [ ] **Step 2: japanese.yaml に新キーを追加**

`STR_LINE_SPACING` の直後に以下を追加:

```yaml
STR_LINE_SPACING_HORIZONTAL: "行間隔（横書き）"
STR_LINE_SPACING_VERTICAL: "行間隔（縦書き）"
```

- [ ] **Step 3: 残り20言語に英語フォールバックとして追加**

各言語ファイルの `STR_LINE_SPACING` の直後に以下を追加（英語のまま。翻訳は後日対応可）:

```yaml
STR_LINE_SPACING_HORIZONTAL: "Line Spacing (Horizontal)"
STR_LINE_SPACING_VERTICAL: "Line Spacing (Vertical)"
```

対象: belarusian, catalan, czech, danish, dutch, finnish, french, german, hungarian, italian, kazakh, lithuanian, polish, portuguese, romanian, russian, spanish, swedish, turkish, ukrainian

- [ ] **Step 4: i18n ヘッダを再生成**

Run: `python3 scripts/gen_i18n.py lib/I18n/translations lib/I18n/`

Expected: 正常終了。`lib/I18n/I18nKeys.h` に `STR_LINE_SPACING_HORIZONTAL` と `STR_LINE_SPACING_VERTICAL` が追加される。

- [ ] **Step 5: コミット**

```bash
git add lib/I18n/translations/*.yaml
git commit -m "✨ 行間隔の縦書き/横書き別設定用のi18n文字列を追加"
```

注意: `I18nKeys.h`, `I18nStrings.h`, `I18nStrings.cpp` は `.gitignore` 済みなので `git add` しない。

---

### Task 2: CrossPointSettings のデータモデル変更

**Files:**
- Modify: `src/CrossPointSettings.h:193,273`
- Modify: `src/CrossPointSettings.cpp:171-184,262-269`

- [ ] **Step 1: メンバ変数を分離（CrossPointSettings.h）**

`src/CrossPointSettings.h` の行193を変更:

```cpp
// 変更前
uint8_t lineSpacing = LINE_SPACING_DEFAULT;

// 変更後
uint8_t lineSpacingHorizontal = LINE_SPACING_DEFAULT;
uint8_t lineSpacingVertical = LINE_SPACING_DEFAULT;
```

- [ ] **Step 2: メソッドシグネチャを変更（CrossPointSettings.h）**

`src/CrossPointSettings.h` の行273を変更:

```cpp
// 変更前
float getReaderLineCompression() const;

// 変更後
float getReaderLineCompression(bool vertical) const;
```

- [ ] **Step 3: getReaderLineCompression 実装を更新（CrossPointSettings.cpp）**

`src/CrossPointSettings.cpp` の行262-269を変更:

```cpp
float CrossPointSettings::getReaderLineCompression(const bool vertical) const {
  const uint8_t raw = vertical ? lineSpacingVertical : lineSpacingHorizontal;
  const uint8_t clamped =
      (raw < LINE_SPACING_MIN) ? LINE_SPACING_MIN : ((raw > LINE_SPACING_MAX) ? LINE_SPACING_MAX : raw);
  return static_cast<float>(clamped) / 100.0f;
}
```

- [ ] **Step 4: バイナリ移行のレガシー読み込みを更新（CrossPointSettings.cpp）**

`src/CrossPointSettings.cpp` の行171-184（`loadFromBinaryFile` 内の lineSpacing 読み込みブロック）を変更:

```cpp
    {
      uint8_t rawLineSpacing = LINE_SPACING_DEFAULT;
      serialization::readPod(inputFile, rawLineSpacing);
      uint8_t migratedValue;
      if (rawLineSpacing < LINE_COMPRESSION_COUNT) {
        migratedValue = migrateLegacyLineSpacing(rawLineSpacing);
      } else if (rawLineSpacing >= LINE_SPACING_MIN && rawLineSpacing <= LINE_SPACING_MAX) {
        migratedValue = rawLineSpacing;
      } else if (rawLineSpacing >= 20 && rawLineSpacing <= 60) {
        migratedValue = LINE_SPACING_DEFAULT;
      } else {
        migratedValue = LINE_SPACING_DEFAULT;
      }
      lineSpacingHorizontal = migratedValue;
      lineSpacingVertical = migratedValue;
    }
```

- [ ] **Step 5: ビルド確認**

Run: `pio run 2>&1 | tail -20`

Expected: `lineSpacing` への参照がまだ残っているのでコンパイルエラーが出る。これは想定通り — 残りのタスクで修正する。

- [ ] **Step 6: コミット**

```bash
git add src/CrossPointSettings.h src/CrossPointSettings.cpp
git commit -m "👍 lineSpacingをlineSpacingHorizontal/lineSpacingVerticalに分離"
```

---

### Task 3: SettingsList と JsonSettingsIO の更新

**Files:**
- Modify: `src/SettingsList.h:130-131`
- Modify: `src/JsonSettingsIO.cpp:135-219`
- Modify: `src/activities/settings/SettingsActivity.cpp:243-254`

- [ ] **Step 1: SettingsList のエントリを2つに分離（SettingsList.h）**

`src/SettingsList.h` の行130-131を変更:

```cpp
// 変更前
      SettingInfo::Value(StrId::STR_LINE_SPACING, &CrossPointSettings::lineSpacing, {80, 250, 1}, "lineSpacing",
                         StrId::STR_CAT_READER),

// 変更後
      SettingInfo::Value(StrId::STR_LINE_SPACING_HORIZONTAL, &CrossPointSettings::lineSpacingHorizontal, {80, 250, 1},
                         "lineSpacingHorizontal", StrId::STR_CAT_READER),
      SettingInfo::Value(StrId::STR_LINE_SPACING_VERTICAL, &CrossPointSettings::lineSpacingVertical, {80, 250, 1},
                         "lineSpacingVertical", StrId::STR_CAT_READER),
```

- [ ] **Step 2: JsonSettingsIO に旧キー移行ロジックを追加（JsonSettingsIO.cpp）**

`src/JsonSettingsIO.cpp` の `loadSettings()` 内、SettingsList ループの直後（行195の後、frontButtonBack の手動読み込みの前）に以下を挿入:

```cpp
  // Legacy migration: single "lineSpacing" key → split into horizontal/vertical
  if (!doc["lineSpacing"].isNull() && doc["lineSpacingHorizontal"].isNull() &&
      doc["lineSpacingVertical"].isNull()) {
    const uint8_t legacy = doc["lineSpacing"] | CrossPointSettings::LINE_SPACING_DEFAULT;
    const uint8_t clamped = (legacy < CrossPointSettings::LINE_SPACING_MIN)
                                ? CrossPointSettings::LINE_SPACING_MIN
                                : ((legacy > CrossPointSettings::LINE_SPACING_MAX) ? CrossPointSettings::LINE_SPACING_MAX
                                                                                   : legacy);
    s.lineSpacingHorizontal = clamped;
    s.lineSpacingVertical = clamped;
    if (needsResave) *needsResave = true;
  }
```

`needsResave = true` により、次回保存時に新キーで書き出される。

- [ ] **Step 3: SettingsActivity の LineSpacing 条件分岐を更新（SettingsActivity.cpp）**

`src/activities/settings/SettingsActivity.cpp` の行243の条件を変更:

```cpp
// 変更前
    if (setting.nameId == StrId::STR_LINE_SPACING) {
      startActivityForResult(
          std::make_unique<LineSpacingSelectionActivity>(
              renderer, mappedInput, static_cast<int>(SETTINGS.lineSpacing),
              [this](const int selectedValue) {
                SETTINGS.lineSpacing = static_cast<uint8_t>(selectedValue);
                SETTINGS.saveToFile();
                finish();
              },
              [this] { finish(); }),
          [this](const ActivityResult&) { requestUpdate(); });
      return;
    }

// 変更後
    if (setting.nameId == StrId::STR_LINE_SPACING_HORIZONTAL || setting.nameId == StrId::STR_LINE_SPACING_VERTICAL) {
      const bool isVertical = (setting.nameId == StrId::STR_LINE_SPACING_VERTICAL);
      uint8_t& target = isVertical ? SETTINGS.lineSpacingVertical : SETTINGS.lineSpacingHorizontal;
      startActivityForResult(
          std::make_unique<LineSpacingSelectionActivity>(
              renderer, mappedInput, static_cast<int>(target),
              [this, &target](const int selectedValue) {
                target = static_cast<uint8_t>(selectedValue);
                SETTINGS.saveToFile();
                finish();
              },
              [this] { finish(); }),
          [this](const ActivityResult&) { requestUpdate(); });
      return;
    }
```

注意: `target` はシングルトン `SETTINGS` のメンバへの参照なのでラムダでキャプチャしても安全（Activity のライフタイム内で使用）。

- [ ] **Step 4: コミット**

```bash
git add src/SettingsList.h src/JsonSettingsIO.cpp src/activities/settings/SettingsActivity.cpp
git commit -m "👍 Settings画面とJSON I/Oを行間隔2項目に対応"
```

---

### Task 4: EpubReaderMenuActivity の更新（verticalMode 対応）

**Files:**
- Modify: `src/activities/reader/EpubReaderMenuActivity.h:33-35`
- Modify: `src/activities/reader/EpubReaderMenuActivity.cpp:21-31,203-217`

- [ ] **Step 1: コンストラクタに verticalMode パラメータを追加（EpubReaderMenuActivity.h）**

`src/activities/reader/EpubReaderMenuActivity.h` の行33-35を変更:

```cpp
// 変更前
  explicit EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                                  const int currentPage, const int totalPages, const int bookProgressPercent,
                                  const uint8_t currentOrientation, const bool hasFootnotes);

// 変更後
  explicit EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                                  const int currentPage, const int totalPages, const int bookProgressPercent,
                                  const uint8_t currentOrientation, const bool hasFootnotes, const bool verticalMode);
```

メンバ変数を追加（`bookProgressPercent` の行64の後に）:

```cpp
  bool verticalMode = false;
```

- [ ] **Step 2: コンストラクタ実装を更新（EpubReaderMenuActivity.cpp）**

`src/activities/reader/EpubReaderMenuActivity.cpp` の行21-31を変更:

```cpp
EpubReaderMenuActivity::EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                               const std::string& title, const int currentPage, const int totalPages,
                                               const int bookProgressPercent, const uint8_t currentOrientation,
                                               const bool hasFootnotes, const bool verticalMode)
    : Activity("EpubReaderMenu", renderer, mappedInput),
      menuItems(buildMenuItems(hasFootnotes)),
      title(title),
      pendingOrientation(currentOrientation),
      currentPage(currentPage),
      totalPages(totalPages),
      bookProgressPercent(bookProgressPercent),
      verticalMode(verticalMode) {}
```

- [ ] **Step 3: getMenuItemValue を更新して verticalMode で分岐（EpubReaderMenuActivity.cpp）**

`src/activities/reader/EpubReaderMenuActivity.cpp` の行213-217（`STYLE_LINE_SPACING` case）を変更:

```cpp
    case MenuAction::STYLE_LINE_SPACING: {
      char spacingBuf[16];
      const uint8_t spacing = verticalMode ? SETTINGS.lineSpacingVertical : SETTINGS.lineSpacingHorizontal;
      snprintf(spacingBuf, sizeof(spacingBuf), "%.2fx", static_cast<float>(spacing) / 100.0f);
      return spacingBuf;
    }
```

- [ ] **Step 4: コミット**

```bash
git add src/activities/reader/EpubReaderMenuActivity.h src/activities/reader/EpubReaderMenuActivity.cpp
git commit -m "👍 EpubReaderMenuActivityにverticalModeを追加し行間隔表示を分岐"
```

---

### Task 5: EpubReaderActivity の更新（全参照の修正）

**Files:**
- Modify: `src/activities/reader/EpubReaderActivity.cpp:165-167,390-404,637-639,787-800`

- [ ] **Step 1: メニュー生成に verticalMode を渡す（EpubReaderActivity.cpp）**

`src/activities/reader/EpubReaderActivity.cpp` の行165-167を変更:

```cpp
// 変更前
    startActivityForResult(std::make_unique<EpubReaderMenuActivity>(
                               renderer, mappedInput, epub->getTitle(), menuCurrentPage, menuTotalPages,
                               bookProgressPercent, SETTINGS.orientation, !currentPageFootnotes.empty()),

// 変更後
    startActivityForResult(std::make_unique<EpubReaderMenuActivity>(
                               renderer, mappedInput, epub->getTitle(), menuCurrentPage, menuTotalPages,
                               bookProgressPercent, SETTINGS.orientation, !currentPageFootnotes.empty(), verticalMode),
```

- [ ] **Step 2: STYLE_LINE_SPACING ハンドラを verticalMode で分岐（EpubReaderActivity.cpp）**

`src/activities/reader/EpubReaderActivity.cpp` の行390-404を変更:

```cpp
    case EpubReaderMenuActivity::MenuAction::STYLE_LINE_SPACING: {
      uint8_t& target = verticalMode ? SETTINGS.lineSpacingVertical : SETTINGS.lineSpacingHorizontal;
      startActivityForResult(
          std::make_unique<LineSpacingSelectionActivity>(
              renderer, mappedInput, static_cast<int>(target),
              [this, &target](const int selectedValue) {
                target = static_cast<uint8_t>(selectedValue);
                SETTINGS.saveToFile();
                finish();
              },
              [this] { finish(); }),
          [this](const ActivityResult&) {
            invalidateSectionPreservingPosition();
            requestUpdate();
          });
      break;
    }
```

- [ ] **Step 3: lineCompression 取得を verticalMode 対応に変更（EpubReaderActivity.cpp）**

行637を変更:

```cpp
// 変更前
    const float lineCompression = SETTINGS.getReaderLineCompression();

// 変更後
    const float lineCompression = SETTINGS.getReaderLineCompression(verticalMode);
```

行639のログも更新:

```cpp
// 変更前
    LOG_DBG("ERS", "Reflow params: lineSpacing=%u, compression=%.2f, viewport=%ux%u", SETTINGS.lineSpacing,
            lineCompression, viewportWidth, viewportHeight);

// 変更後
    LOG_DBG("ERS", "Reflow params: lineSpacing=%u, compression=%.2f, viewport=%ux%u, vertical=%d",
            verticalMode ? SETTINGS.lineSpacingVertical : SETTINGS.lineSpacingHorizontal, lineCompression,
            viewportWidth, viewportHeight, verticalMode);
```

- [ ] **Step 4: プリフェッチ（silentIndexNextChapterIfNeeded）を更新（EpubReaderActivity.cpp）**

行787と行796の `SETTINGS.getReaderLineCompression()` を `SETTINGS.getReaderLineCompression(verticalMode)` に変更:

```cpp
// 行787 変更前
  if (nextSection.loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),

// 行787 変更後
  if (nextSection.loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(verticalMode),

// 行796 変更前
  if (!nextSection.createSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),

// 行796 変更後
  if (!nextSection.createSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(verticalMode),
```

- [ ] **Step 5: コミット**

```bash
git add src/activities/reader/EpubReaderActivity.cpp
git commit -m "👍 EpubReaderActivityの行間隔参照をverticalMode対応に更新"
```

---

### Task 6: ビルド検証と差分確認

- [ ] **Step 1: フルビルド**

Run: `pio run -t clean && pio run 2>&1 | tail -30`

Expected: `SUCCESS` でエラー・警告なし。

- [ ] **Step 2: 他の getReaderLineCompression() 呼び出しが残っていないか確認**

Run: `grep -rn "getReaderLineCompression" src/ lib/`

Expected: すべての呼び出しが `getReaderLineCompression(verticalMode)` になっていること。引数なしの呼び出しが残っていればコンパイルエラーになるはず。

- [ ] **Step 3: lineSpacing（旧変数名）への直接参照が残っていないか確認**

Run: `grep -rn "SETTINGS\.lineSpacing[^HV]" src/ lib/ --include="*.cpp" --include="*.h"`

Expected: マッチなし（`lineSpacingHorizontal` や `lineSpacingVertical` はマッチするが `lineSpacing` 単体はマッチしない）。

注意: `SETTINGS.lineSpacing` という部分文字列は `lineSpacingHorizontal` 等にもマッチするので、正規表現で `SETTINGS\.lineSpacing[^HV]` を使って区別する。

- [ ] **Step 4: git diff で意図しない変更がないことを確認**

Run: `git diff HEAD~5 --stat`

Expected: 変更ファイルがスペックの影響ファイル一覧と一致すること。

- [ ] **Step 5: (完了確認のみ — コミット不要)**

すべてのDone判定基準を確認:
- [x] `lineSpacingHorizontal` と `lineSpacingVertical` がそれぞれ独立して保存・読み込みされる
- [x] 旧設定（`lineSpacing`キー）から新設定への移行が正しく動作する
- [x] Settings画面に2項目が表示される
- [x] リーダーメニューで現在の書字方向に応じた行間隔のみ表示される
- [x] 縦書き/横書きでそれぞれ異なる行間隔が適用される
- [x] `pio run` がエラー・警告なしで成功する
- [x] `git diff` で意図しない変更がないことを確認
