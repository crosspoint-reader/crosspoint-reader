# 縦書き/横書き別設定 実装プラン

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** リーダー設定を `DirectionSettings` 構造体に分離し、縦書き/横書きごとに独立した設定を持てるようにする。

**Architecture:** `DirectionSettings` 構造体を新設し `CrossPointSettings` に `horizontal`/`vertical` の2インスタンスを持たせる。JSON I/Oはネストオブジェクトで永続化し、旧フォーマットからの自動マイグレーションを行う。設定UIは `DirectionSettingsActivity` で方向別サブメニューを提供する。

**Tech Stack:** C++20, Arduino-ESP32, ArduinoJson, PlatformIO

**Spec:** `docs/superpowers/specs/2026-04-10-direction-settings-design.md`

---

## ファイル構成

| ファイル | 変更種別 | 責務 |
|---------|---------|------|
| `src/CrossPointSettings.h` | 変更 | `DirectionSettings` 構造体追加、旧フィールド削除、ヘルパー関数追加 |
| `src/CrossPointSettings.cpp` | 変更 | `getReaderFontId(bool)` 等のメソッド変更 |
| `src/JsonSettingsIO.cpp` | 変更 | ネストJSON読み書き、マイグレーション |
| `src/SettingsList.h` | 変更 | Reader カテゴリから分離項目を除去、サブメニューエントリ追加 |
| `src/activities/settings/DirectionSettingsActivity.h` | 新規 | 方向別設定サブメニューActivity（ヘッダ） |
| `src/activities/settings/DirectionSettingsActivity.cpp` | 新規 | 方向別設定サブメニューActivity（実装） |
| `src/activities/settings/SettingsActivity.cpp` | 変更 | サブメニュー遷移の追加 |
| `src/activities/reader/EpubReaderActivity.cpp` | 変更 | 設定参照を方向別に変更 |
| `src/activities/reader/EpubReaderMenuActivity.cpp` | 変更 | クイック設定の読み書きを方向別に変更 |
| `src/activities/reader/TxtReaderActivity.cpp` | 変更 | 設定参照を `horizontal` 経由に変更 |
| `src/activities/settings/GenerateAllCacheActivity.cpp` | 変更 | 設定参照を方向別に変更 |
| `src/SdCardFontSystem.cpp` | 変更 | sdFontFamilyNameの参照先変更 |
| `src/FontInstaller.cpp` | 変更 | sdFontFamilyNameの参照先変更 |
| `lib/Epub/Epub/Section.cpp` | 変更 | `SECTION_FILE_VERSION` インクリメント |
| `lib/I18n/translations/*.yaml` | 変更 | 新規文字列追加 |

---

## Done 判定基準

- [ ] `pio run` がエラー0・警告0でビルド成功
- [ ] 設定JSONが新フォーマット（`horizontal`/`vertical` ネスト）で保存される
- [ ] 旧設定JSONからの自動マイグレーションが動作する
- [ ] 設定UI「横書き設定」「縦書き設定」サブメニューが表示・操作できる
- [ ] 読書中クイックメニューが現在の方向に応じた設定を読み書きする
- [ ] `git diff` で意図しない変更が含まれていない
※ 必須ゲート（動作検証・既存機能・差分確認・シークレット）は常に適用

---

### Task 1: DirectionSettings 構造体の追加と CrossPointSettings の変更

**Files:**
- Modify: `src/CrossPointSettings.h:183-248` (フィールド削除・構造体追加)
- Modify: `src/CrossPointSettings.cpp:264-455` (メソッドの引数変更)

- [ ] **Step 1: `DirectionSettings` 構造体を `CrossPointSettings.h` に追加**

`CrossPointSettings` クラス定義の直前（行7の前）に構造体を追加し、クラス内に `horizontal`/`vertical` メンバを追加する。

```cpp
// CrossPointSettings.h の先頭付近（行6の後に追加）

struct DirectionSettings {
  uint8_t fontFamily = 0;       // CrossPointSettings::BOOKERLY
  char sdFontFamilyName[32] = "";
  uint8_t fontSize = 1;         // CrossPointSettings::MEDIUM
  uint8_t lineSpacing = 185;    // 80-250 (%)
  uint8_t charSpacing = 0;      // 0-50 (5刻み)
  uint8_t paragraphAlignment = 0; // CrossPointSettings::JUSTIFIED
  uint8_t extraParagraphSpacing = 0;
  uint8_t hyphenationEnabled = 0;
  uint8_t screenMargin = 10;    // 5-40
  uint8_t firstLineIndent = 1;
  uint8_t textAntiAliasing = 0;
};
```

- [ ] **Step 2: `CrossPointSettings` のフィールドを DirectionSettings に移動**

旧フィールド（行183-184, 200-206, 211, 214, 228-230, 241）を削除し、代わりに `horizontal`/`vertical` メンバを追加。
`verticalCharSpacing` のデフォルトだけ `vertical` 側で15に設定。

削除するフィールド:
- `extraParagraphSpacing` (行183)
- `textAntiAliasing` (行184)
- `fontFamily` (行200)
- `fontSize` (行201)
- `lineSpacingHorizontal` (行202)
- `lineSpacingVertical` (行203)
- `paragraphAlignment` (行204)
- `verticalCharSpacing` (行206)
- `hyphenationEnabled` (行211)
- `screenMargin` (行214)
- `sdFontFamilyName` (行230)
- `firstLineIndent` (行241)

追加するフィールド（旧フィールドがあった場所に配置）:
```cpp
  // Direction-specific reader settings
  DirectionSettings horizontal;
  DirectionSettings vertical = {0, "", 1, 185, 15, 0, 0, 0, 10, 1, 0}; // charSpacing=15
```

- [ ] **Step 3: ヘルパー関数を追加**

```cpp
  // Public section に追加
  const DirectionSettings& getDirectionSettings(bool isVertical) const {
    return isVertical ? vertical : horizontal;
  }
  DirectionSettings& getDirectionSettings(bool isVertical) {
    return isVertical ? vertical : horizontal;
  }
```

- [ ] **Step 4: `getReaderFontId()` に `bool isVertical` パラメータを追加**

`CrossPointSettings.h` のシグネチャ変更:
```cpp
  int getReaderFontId(bool isVertical) const;
  int getBuiltInReaderFontId(bool isVertical) const;
  int getHeadingFontId(int headingLevel, bool isVertical) const;
  int getTableFontId(bool isVertical) const;
```

`CrossPointSettings.cpp` の実装変更 — 全メソッドで `fontFamily` → `getDirectionSettings(isVertical).fontFamily`、`fontSize` → `getDirectionSettings(isVertical).fontSize`、`sdFontFamilyName` → `getDirectionSettings(isVertical).sdFontFamilyName` に置き換える。

`getReaderFontId(bool isVertical)`:
```cpp
int CrossPointSettings::getReaderFontId(bool isVertical) const {
  const auto& ds = getDirectionSettings(isVertical);
  if (ds.sdFontFamilyName[0] != '\0' && sdFontIdResolver) {
    int id = sdFontIdResolver(sdFontResolverCtx, ds.sdFontFamilyName, ds.fontSize);
    if (id != 0) return id;
  }
  const FontManager& fm = FontManager::getInstance();
  if (fm.isExternalFontEnabled()) {
    return -(fm.getSelectedIndex() + 1000);
  }
  return getBuiltInReaderFontId(isVertical);
}
```

`getBuiltInReaderFontId(bool isVertical)`: `fontFamily` → `getDirectionSettings(isVertical).fontFamily`、`fontSize` → `getDirectionSettings(isVertical).fontSize` に置き換え。

`getHeadingFontId(int, bool isVertical)`: 同様に `fontFamily`, `fontSize`, `sdFontFamilyName` を `getDirectionSettings(isVertical)` 経由に。

`getTableFontId(bool isVertical)`: 同様。

- [ ] **Step 5: `getReaderLineCompression()` と `getVerticalCharSpacingPercent()` を更新**

```cpp
float CrossPointSettings::getReaderLineCompression(const bool vertical) const {
  const uint8_t raw = getDirectionSettings(vertical).lineSpacing;
  const uint8_t clamped =
      (raw < LINE_SPACING_MIN) ? LINE_SPACING_MIN : ((raw > LINE_SPACING_MAX) ? LINE_SPACING_MAX : raw);
  return static_cast<float>(clamped) / 100.0f;
}
```

`getVerticalCharSpacingPercent()` は `vertical.charSpacing` 固定でOK（縦書き時のみ使用されるため）:
```cpp
uint8_t getVerticalCharSpacingPercent() const { return vertical.charSpacing; }
```

- [ ] **Step 6: `loadFromBinaryFile()` のマイグレーションを更新**

バイナリ読み込みで旧フィールドを読み取った後、`horizontal`/`vertical` 両方にコピーする。
`lineSpacingHorizontal` → `horizontal.lineSpacing`, `lineSpacingVertical` → `vertical.lineSpacing` として読み込む。
その他のフィールド（`fontFamily`, `fontSize`, `paragraphAlignment` 等）は両方にコピー。

- [ ] **Step 7: ビルド確認**

```bash
pio run 2>&1 | tail -20
```

この時点ではコンパイルエラーが多数出る（他ファイルが旧フィールドを参照しているため）。エラー内容を確認し、Task 2以降で修正する。

- [ ] **Step 8: コミット**

```bash
git add src/CrossPointSettings.h src/CrossPointSettings.cpp
git commit -m "👍 DirectionSettings構造体を追加し、リーダー設定を方向別に分離（Issue #39）"
```

---

### Task 2: JSON I/O のネスト対応とマイグレーション

**Files:**
- Modify: `src/JsonSettingsIO.cpp:100-233`
- Modify: `src/SettingsList.h:130-167`

- [ ] **Step 1: `SettingsList.h` から DirectionSettings 項目を除去**

Reader カテゴリから以下を削除:
- `buildFontFamilySetting(registry)` (行131)
- `fontSize` (行132-134)
- `lineSpacingHorizontal` (行135-136)
- `lineSpacingVertical` (行137-138)
- `screenMargin` (行139-140)
- `paragraphAlignment` (行141-144)
- `hyphenationEnabled` (行147-148)
- `extraParagraphSpacing` (行152-153)
- `textAntiAliasing` (行154-155)
- `firstLineIndent` (行159-160)
- `verticalCharSpacing` (行164-165)

Reader カテゴリに残す:
- `embeddedStyle`
- `orientation`
- `imageRendering`
- `writingMode`
- `invertImages`

Reader カテゴリに追加（writingMode の前）:
```cpp
SettingInfo::Action(StrId::STR_HORIZONTAL_SETTINGS, SettingAction::HorizontalSettings),
SettingInfo::Action(StrId::STR_VERTICAL_SETTINGS, SettingAction::VerticalSettings),
```

`SettingsActivity.h` の `SettingAction` enum に追加:
```cpp
enum class SettingAction {
  // ... 既存 ...
  HorizontalSettings,
  VerticalSettings,
};
```

- [ ] **Step 2: `JsonSettingsIO::saveSettings()` に DirectionSettings のネスト保存を追加**

`saveSettings()` の末尾（行131の前、`String json;` の前）に追加:

```cpp
  // Direction-specific settings
  auto saveDirection = [](JsonObject obj, const DirectionSettings& ds) {
    obj["fontFamily"] = ds.fontFamily;
    if (ds.sdFontFamilyName[0] != '\0') {
      obj["sdFontFamilyName"] = ds.sdFontFamilyName;
    }
    obj["fontSize"] = ds.fontSize;
    obj["lineSpacing"] = ds.lineSpacing;
    obj["charSpacing"] = ds.charSpacing;
    obj["paragraphAlignment"] = ds.paragraphAlignment;
    obj["extraParagraphSpacing"] = ds.extraParagraphSpacing;
    obj["hyphenationEnabled"] = ds.hyphenationEnabled;
    obj["screenMargin"] = ds.screenMargin;
    obj["firstLineIndent"] = ds.firstLineIndent;
    obj["textAntiAliasing"] = ds.textAntiAliasing;
  };
  saveDirection(doc["horizontal"].to<JsonObject>(), s.horizontal);
  saveDirection(doc["vertical"].to<JsonObject>(), s.vertical);
```

既存の手動保存（`fontFamily`, `sdFontFamilyName` — 行125-130）を削除。

- [ ] **Step 3: `JsonSettingsIO::loadSettings()` に DirectionSettings のネスト読み込みとマイグレーションを追加**

`loadSettings()` の末尾（行229の前、`LOG_DBG` の前）に追加:

```cpp
  auto loadDirection = [](JsonObject obj, DirectionSettings& ds) {
    if (obj.isNull()) return false;
    ds.fontFamily = obj["fontFamily"] | ds.fontFamily;
    if (ds.fontFamily >= CrossPointSettings::FONT_FAMILY_COUNT) ds.fontFamily = 0;
    const char* sfn = obj["sdFontFamilyName"] | "";
    strncpy(ds.sdFontFamilyName, sfn, sizeof(ds.sdFontFamilyName) - 1);
    ds.sdFontFamilyName[sizeof(ds.sdFontFamilyName) - 1] = '\0';
    ds.fontSize = obj["fontSize"] | ds.fontSize;
    if (ds.fontSize >= CrossPointSettings::FONT_SIZE_COUNT) ds.fontSize = 1;
    ds.lineSpacing = obj["lineSpacing"] | ds.lineSpacing;
    if (ds.lineSpacing < CrossPointSettings::LINE_SPACING_MIN) ds.lineSpacing = CrossPointSettings::LINE_SPACING_MIN;
    if (ds.lineSpacing > CrossPointSettings::LINE_SPACING_MAX) ds.lineSpacing = CrossPointSettings::LINE_SPACING_MAX;
    ds.charSpacing = obj["charSpacing"] | ds.charSpacing;
    if (ds.charSpacing > 50) ds.charSpacing = 0;
    ds.paragraphAlignment = obj["paragraphAlignment"] | ds.paragraphAlignment;
    if (ds.paragraphAlignment >= CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT) ds.paragraphAlignment = 0;
    ds.extraParagraphSpacing = obj["extraParagraphSpacing"] | ds.extraParagraphSpacing;
    ds.hyphenationEnabled = obj["hyphenationEnabled"] | ds.hyphenationEnabled;
    ds.screenMargin = obj["screenMargin"] | ds.screenMargin;
    if (ds.screenMargin < 5) ds.screenMargin = 5;
    if (ds.screenMargin > 40) ds.screenMargin = 40;
    ds.firstLineIndent = obj["firstLineIndent"] | ds.firstLineIndent;
    ds.textAntiAliasing = obj["textAntiAliasing"] | ds.textAntiAliasing;
    return true;
  };

  if (!loadDirection(doc["horizontal"].as<JsonObject>(), s.horizontal)) {
    // Migration from flat format: copy old values to both directions
    // fontFamily and sdFontFamilyName were loaded manually above
    s.horizontal.fontFamily = s.horizontal.fontFamily; // already set from manual load above
    // Copy line spacing from legacy split fields if present
    if (!doc["lineSpacingHorizontal"].isNull()) {
      s.horizontal.lineSpacing = doc["lineSpacingHorizontal"] | s.horizontal.lineSpacing;
    }
    if (!doc["lineSpacingVertical"].isNull()) {
      s.vertical.lineSpacing = doc["lineSpacingVertical"] | s.vertical.lineSpacing;
    }
    // Copy remaining flat fields to both
    auto migrateBoth = [&](const char* key, uint8_t DirectionSettings::* field) {
      uint8_t val = doc[key] | s.horizontal.*field;
      s.horizontal.*field = val;
      s.vertical.*field = val;
    };
    migrateBoth("extraParagraphSpacing", &DirectionSettings::extraParagraphSpacing);
    migrateBoth("textAntiAliasing", &DirectionSettings::textAntiAliasing);
    migrateBoth("paragraphAlignment", &DirectionSettings::paragraphAlignment);
    migrateBoth("hyphenationEnabled", &DirectionSettings::hyphenationEnabled);
    migrateBoth("screenMargin", &DirectionSettings::screenMargin);
    migrateBoth("firstLineIndent", &DirectionSettings::firstLineIndent);
    migrateBoth("fontSize", &DirectionSettings::fontSize);
    // fontFamily — already loaded manually, copy to both
    s.vertical.fontFamily = s.horizontal.fontFamily;
    strncpy(s.vertical.sdFontFamilyName, s.horizontal.sdFontFamilyName, sizeof(s.vertical.sdFontFamilyName));
    // verticalCharSpacing → vertical.charSpacing only
    s.vertical.charSpacing = doc["verticalCharSpacing"] | s.vertical.charSpacing;
    if (needsResave) *needsResave = true;
  } else {
    loadDirection(doc["vertical"].as<JsonObject>(), s.vertical);
  }
```

既存の手動ロード（`fontFamily`, `sdFontFamilyName` — 行223-228）を削除。
レガシー lineSpacing マイグレーション（行199-209）も上記マイグレーションに統合。

- [ ] **Step 4: ビルド確認**

```bash
pio run 2>&1 | tail -20
```

- [ ] **Step 5: コミット**

```bash
git add src/JsonSettingsIO.cpp src/SettingsList.h src/activities/settings/SettingsActivity.h
git commit -m "👍 JSON I/Oを方向別ネスト形式に変更し旧形式からの自動マイグレーションを追加（Issue #39）"
```

---

### Task 3: 全参照箇所の更新（EpubReaderActivity, TxtReaderActivity, GenerateAllCache, SdCardFontSystem 等）

**Files:**
- Modify: `src/activities/reader/EpubReaderActivity.cpp`
- Modify: `src/activities/reader/EpubReaderMenuActivity.cpp`
- Modify: `src/activities/reader/TxtReaderActivity.cpp`
- Modify: `src/activities/settings/GenerateAllCacheActivity.cpp`
- Modify: `src/SdCardFontSystem.cpp`
- Modify: `src/FontInstaller.cpp`
- Modify: `src/activities/settings/FontSelectActivity.cpp`
- Modify: `src/activities/settings/FontSelectionActivity.cpp`

- [ ] **Step 1: `EpubReaderActivity.cpp` の全参照を更新**

以下のパターンで全て置き換え:

| 旧 | 新 |
|----|-----|
| `SETTINGS.screenMargin` | `SETTINGS.getDirectionSettings(isVertical).screenMargin` (render内) / `SETTINGS.getDirectionSettings(verticalMode).screenMargin` (他) |
| `SETTINGS.getReaderFontId()` | `SETTINGS.getReaderFontId(verticalMode)` |
| `SETTINGS.extraParagraphSpacing` | `SETTINGS.getDirectionSettings(verticalMode).extraParagraphSpacing` |
| `SETTINGS.paragraphAlignment` | `SETTINGS.getDirectionSettings(verticalMode).paragraphAlignment` |
| `SETTINGS.hyphenationEnabled` | `SETTINGS.getDirectionSettings(verticalMode).hyphenationEnabled` |
| `SETTINGS.firstLineIndent` | `SETTINGS.getDirectionSettings(verticalMode).firstLineIndent` |
| `SETTINGS.embeddedStyle` | `SETTINGS.embeddedStyle` (変更なし) |
| `SETTINGS.imageRendering` | `SETTINGS.imageRendering` (変更なし) |
| `SETTINGS.textAntiAliasing` | `SETTINGS.getDirectionSettings(verticalMode).textAntiAliasing` |
| `SETTINGS.getHeadingFontId(N)` | `SETTINGS.getHeadingFontId(N, verticalMode)` |
| `SETTINGS.getTableFontId()` | `SETTINGS.getTableFontId(verticalMode)` |

`pregenerateCache()` 内（行55-144）: `isVertical` ローカル変数を使用。screenMargin は `SETTINGS.getDirectionSettings(isVertical).screenMargin`。

`render()` 内（行740-860）: `verticalMode` メンバ変数を使用。screenMargin は verticalMode 確定前に使用されているので注意 — 行745-758 の screenMargin 参照は、verticalMode が確定する行766の後に移動するか、一旦仮の値を使って後で再計算するか検討が必要。

**重要**: render() 内では screenMargin は verticalMode 判定前（行745-758）に使われ、verticalMode は行766-775で確定する。この順序問題を解決するため、render()の構造を以下のように変更:
1. まず writingMode → verticalMode を解決（既存の行766-775のロジック）
2. その後 screenMargin を `getDirectionSettings(verticalMode)` から取得
3. viewport計算を screenMargin 取得後に実行

`renderContents()` 内（行967-1059）: `SETTINGS.getReaderFontId()` → `SETTINGS.getReaderFontId(verticalMode)`, `SETTINGS.textAntiAliasing` → `SETTINGS.getDirectionSettings(verticalMode).textAntiAliasing`。

`silentIndexNextChapterIfNeeded()` 内（行920-948）: 同様のパターン。

クイックメニューアクション処理（行522-548）:
- `STYLE_FIRST_LINE_INDENT`: `SETTINGS.firstLineIndent` → `SETTINGS.getDirectionSettings(verticalMode).firstLineIndent`
- `STYLE_LINE_SPACING`: `SETTINGS.lineSpacingVertical`/`lineSpacingHorizontal` → `SETTINGS.getDirectionSettings(verticalMode).lineSpacing`

- [ ] **Step 2: `EpubReaderMenuActivity.cpp` の参照を更新**

`getMenuItemValue()` 内:
- `SETTINGS.firstLineIndent` → `SETTINGS.getDirectionSettings(verticalMode).firstLineIndent`
- `SETTINGS.lineSpacingVertical`/`lineSpacingHorizontal` → `SETTINGS.getDirectionSettings(verticalMode).lineSpacing`

- [ ] **Step 3: `TxtReaderActivity.cpp` の参照を更新**

`initializeReader()` 内（行92-118）:
- `SETTINGS.getReaderFontId()` → `SETTINGS.getReaderFontId(false)` (TXTは常に横書き)
- `SETTINGS.screenMargin` → `SETTINGS.horizontal.screenMargin`
- `SETTINGS.paragraphAlignment` → `SETTINGS.horizontal.paragraphAlignment`
- `SETTINGS.getReaderLineCompression(false)` → そのまま（既に false 渡し済み）

その他の参照:
- `SETTINGS.textAntiAliasing` → `SETTINGS.horizontal.textAntiAliasing`

- [ ] **Step 4: `GenerateAllCacheActivity.cpp` の参照を更新**

`pregenerateCache()` 相当の処理内: EpubReaderActivity と同パターン。
- `isVertical` ローカル変数がある（行174-183）のでそれに基づいて `getDirectionSettings(isVertical)` を使用。
- `SETTINGS.screenMargin` → `SETTINGS.getDirectionSettings(isVertical).screenMargin`
- 他も同様。

- [ ] **Step 5: `SdCardFontSystem.cpp` の参照を更新**

`begin()` と `ensureLoaded()` で `SETTINGS.sdFontFamilyName` を参照している箇所:
- SDカードフォントの「どちらの方向のフォントをロードするか」は、現在読んでいる本の方向に依存する
- `begin()` は起動時に1回呼ばれるだけなので、とりあえず `horizontal.sdFontFamilyName` をロードし、`ensureLoaded()` で方向切替時に再ロードする仕組みにする
- ただし実際には SdCardFontSystem は1つのフォントファミリーしかロードできない設計のため、横書きと縦書きで**異なるSDカードフォント**を使う場合はフォントの再ロードが必要

**現実的な対応**: `SdCardFontSystem::begin()` では横書きの `sdFontFamilyName` を初期ロードする。`ensureLoaded()` は引数 `bool isVertical` を追加し、適切な方向の `sdFontFamilyName` を参照する。`EpubReaderActivity` から `ensureLoaded(verticalMode)` を呼ぶ。

```cpp
// SdCardFontSystem.h に追加
void ensureLoaded(GfxRenderer& renderer, bool isVertical);
```

```cpp
// SdCardFontSystem.cpp
void SdCardFontSystem::begin(GfxRenderer& renderer) {
  // ... registry discover ...
  // Initial load: use horizontal settings
  const char* fontName = SETTINGS.horizontal.sdFontFamilyName;
  if (fontName[0] != '\0') {
    // ... load logic (既存と同じ、sdFontFamilyName → fontName) ...
  }
  FontManager::getInstance().setSdCardFontActive(fontName[0] != '\0');
}

void SdCardFontSystem::ensureLoaded(GfxRenderer& renderer, bool isVertical) {
  // ... 既存ロジック、wantedFamily = SETTINGS.getDirectionSettings(isVertical).sdFontFamilyName に変更 ...
}
```

- [ ] **Step 6: `FontInstaller.cpp` の参照を更新**

`removeFamily()` 内で `SETTINGS.sdFontFamilyName` を比較・クリアしている箇所 → `horizontal.sdFontFamilyName` と `vertical.sdFontFamilyName` の両方をチェック・クリア。

- [ ] **Step 7: `FontSelectActivity.cpp` の参照を更新**

`handleSelection()` 内で `SETTINGS.fontFamily` を設定している箇所: このActivityは設定画面から呼ばれるため、どちらの方向の設定を変更するかを知る必要がある。`FontSelectActivity` のコンストラクタに `bool isVertical` パラメータを追加し、`SETTINGS.getDirectionSettings(isVertical)` に書き込む。

- [ ] **Step 8: `FontSelectionActivity.cpp` の参照を更新**

同様に `bool isVertical` パラメータを追加。

- [ ] **Step 9: ビルド確認**

```bash
pio run 2>&1 | tail -20
```

エラー0を確認。

- [ ] **Step 10: コミット**

```bash
git add -A
git commit -m "👍 全参照箇所をDirectionSettings経由に変更（Issue #39）"
```

---

### Task 4: DirectionSettingsActivity の実装

**Files:**
- Create: `src/activities/settings/DirectionSettingsActivity.h`
- Create: `src/activities/settings/DirectionSettingsActivity.cpp`
- Modify: `src/activities/settings/SettingsActivity.cpp`

- [ ] **Step 1: `DirectionSettingsActivity.h` を作成**

```cpp
#pragma once
#include <I18n.h>
#include <SdCardFontRegistry.h>

#include <functional>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Setting info for DirectionSettings fields (uses member pointers into DirectionSettings)
struct DirectionSettingInfo {
  StrId nameId;
  enum class Type { TOGGLE, ENUM, VALUE, FONT_FAMILY } type;
  uint8_t DirectionSettings::* valuePtr = nullptr;
  std::vector<StrId> enumValues;
  std::vector<std::string> enumStringValues;
  struct ValueRange { uint8_t min; uint8_t max; uint8_t step; };
  ValueRange valueRange = {};
};

class DirectionSettingsActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  bool isVertical;
  int selectedIndex = 0;
  std::vector<DirectionSettingInfo> settingsList;

  void buildSettingsList();
  void toggleCurrentSetting();

 public:
  explicit DirectionSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool isVertical);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
```

- [ ] **Step 2: `DirectionSettingsActivity.cpp` を作成**

設定リストの構築:
```cpp
void DirectionSettingsActivity::buildSettingsList() {
  settingsList.clear();
  settingsList.reserve(12);

  // Font Family (special handling — opens FontSelectActivity)
  { DirectionSettingInfo s; s.nameId = StrId::STR_FONT_FAMILY; s.type = DirectionSettingInfo::Type::FONT_FAMILY; settingsList.push_back(std::move(s)); }

  // Font Size
  { DirectionSettingInfo s; s.nameId = StrId::STR_FONT_SIZE; s.type = DirectionSettingInfo::Type::ENUM;
    s.valuePtr = &DirectionSettings::fontSize;
    s.enumValues = {StrId::STR_SMALL, StrId::STR_MEDIUM, StrId::STR_LARGE, StrId::STR_X_LARGE};
    settingsList.push_back(std::move(s)); }

  // Line Spacing (opens LineSpacingSelectionActivity)
  { DirectionSettingInfo s; s.nameId = StrId::STR_LINE_SPACING; s.type = DirectionSettingInfo::Type::VALUE;
    s.valuePtr = &DirectionSettings::lineSpacing; s.valueRange = {80, 250, 1};
    settingsList.push_back(std::move(s)); }

  // Character Spacing
  { DirectionSettingInfo s; s.nameId = StrId::STR_CHAR_SPACING; s.type = DirectionSettingInfo::Type::VALUE;
    s.valuePtr = &DirectionSettings::charSpacing; s.valueRange = {0, 50, 5};
    settingsList.push_back(std::move(s)); }

  // Paragraph Alignment
  { DirectionSettingInfo s; s.nameId = StrId::STR_PARA_ALIGNMENT; s.type = DirectionSettingInfo::Type::ENUM;
    s.valuePtr = &DirectionSettings::paragraphAlignment;
    s.enumValues = {StrId::STR_JUSTIFY, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT, StrId::STR_BOOK_S_STYLE};
    settingsList.push_back(std::move(s)); }

  // Extra Paragraph Spacing
  { DirectionSettingInfo s; s.nameId = StrId::STR_EXTRA_SPACING; s.type = DirectionSettingInfo::Type::TOGGLE;
    s.valuePtr = &DirectionSettings::extraParagraphSpacing;
    settingsList.push_back(std::move(s)); }

  // Hyphenation
  { DirectionSettingInfo s; s.nameId = StrId::STR_HYPHENATION; s.type = DirectionSettingInfo::Type::TOGGLE;
    s.valuePtr = &DirectionSettings::hyphenationEnabled;
    settingsList.push_back(std::move(s)); }

  // Screen Margin
  { DirectionSettingInfo s; s.nameId = StrId::STR_SCREEN_MARGIN; s.type = DirectionSettingInfo::Type::VALUE;
    s.valuePtr = &DirectionSettings::screenMargin; s.valueRange = {5, 40, 5};
    settingsList.push_back(std::move(s)); }

  // First Line Indent
  { DirectionSettingInfo s; s.nameId = StrId::STR_FIRST_LINE_INDENT; s.type = DirectionSettingInfo::Type::TOGGLE;
    s.valuePtr = &DirectionSettings::firstLineIndent;
    settingsList.push_back(std::move(s)); }

  // Text Anti-Aliasing
  { DirectionSettingInfo s; s.nameId = StrId::STR_TEXT_AA; s.type = DirectionSettingInfo::Type::TOGGLE;
    s.valuePtr = &DirectionSettings::textAntiAliasing;
    settingsList.push_back(std::move(s)); }
}
```

`toggleCurrentSetting()`:
- TOGGLE: `ds.*ptr = !ds.*ptr`
- ENUM: `ds.*ptr = (ds.*ptr + 1) % count`
- VALUE (lineSpacing): `LineSpacingSelectionActivity` を起動
- VALUE (その他): `ds.*ptr += step; if (ds.*ptr > max) ds.*ptr = min;`
- FONT_FAMILY: `FontSelectActivity` を起動（`isVertical` を渡す）

`render()`:
- `SettingsActivity::render()` と同様のリスト表示UI
- ヘッダーに「横書き設定」または「縦書き設定」を表示
- 各設定の現在値を `ds.*ptr` から読み取って表示

`loop()`:
- Back: `SETTINGS.saveToFile()` して `finish()`
- Confirm: `toggleCurrentSetting()`
- Up/Down: リスト移動

- [ ] **Step 3: `SettingsActivity.cpp` にサブメニュー遷移を追加**

`toggleCurrentSetting()` の ACTION 型処理（行272-327）に追加:

```cpp
case SettingAction::HorizontalSettings: {
  startActivityForResult(
      std::make_unique<DirectionSettingsActivity>(renderer, mappedInput, false),
      [this](const ActivityResult&) {
        rebuildSettingsLists();
        requestUpdate();
      });
  break;
}
case SettingAction::VerticalSettings: {
  startActivityForResult(
      std::make_unique<DirectionSettingsActivity>(renderer, mappedInput, true),
      [this](const ActivityResult&) {
        rebuildSettingsLists();
        requestUpdate();
      });
  break;
}
```

ヘッダーインクルードに `DirectionSettingsActivity.h` を追加。

- [ ] **Step 4: ビルド確認**

```bash
pio run 2>&1 | tail -20
```

- [ ] **Step 5: コミット**

```bash
git add src/activities/settings/DirectionSettingsActivity.h src/activities/settings/DirectionSettingsActivity.cpp src/activities/settings/SettingsActivity.cpp
git commit -m "✨ 方向別設定サブメニュー（DirectionSettingsActivity）を追加（Issue #39）"
```

---

### Task 5: I18n 文字列の追加

**Files:**
- Modify: `lib/I18n/translations/english.yaml`
- Modify: `lib/I18n/translations/japanese.yaml`
- Modify: 他の翻訳ファイル（中国語簡体字、中国語繁体字）

- [ ] **Step 1: 翻訳ファイルに新規キーを追加**

`english.yaml`:
```yaml
STR_HORIZONTAL_SETTINGS: "Horizontal Settings"
STR_VERTICAL_SETTINGS: "Vertical Settings"
STR_CHAR_SPACING: "Character Spacing"
STR_LINE_SPACING: "Line Spacing"
```

`japanese.yaml`:
```yaml
STR_HORIZONTAL_SETTINGS: "横書き設定"
STR_VERTICAL_SETTINGS: "縦書き設定"
STR_CHAR_SPACING: "文字間隔"
STR_LINE_SPACING: "行間"
```

中国語ファイル（簡体字・繁体字）にも対応する翻訳を追加。

- [ ] **Step 2: I18n ヘッダーを再生成**

```bash
python3 scripts/gen_i18n.py lib/I18n/translations lib/I18n/
```

- [ ] **Step 3: ビルド確認**

```bash
pio run 2>&1 | tail -20
```

- [ ] **Step 4: コミット**

```bash
git add lib/I18n/translations/
git commit -m "✨ 方向別設定のI18n文字列を追加（Issue #39）"
```

---

### Task 6: SECTION_FILE_VERSION のインクリメント

**Files:**
- Modify: `lib/Epub/Epub/Section.cpp`

- [ ] **Step 1: バージョン番号を確認してインクリメント**

```bash
grep -n "SECTION_FILE_VERSION" lib/Epub/Epub/Section.cpp
```

現在の値（27）を28に変更。

- [ ] **Step 2: コミット**

```bash
git add lib/Epub/Epub/Section.cpp
git commit -m "👍 設定構造変更に伴いSECTION_FILE_VERSIONを28に更新（Issue #39）"
```

---

### Task 7: ビルド確認と最終調整

- [ ] **Step 1: クリーンビルド**

```bash
pio run -t clean && pio run 2>&1 | tail -30
```

エラー0を確認。

- [ ] **Step 2: 残存する旧フィールド参照のチェック**

```bash
grep -rn "SETTINGS\.fontFamily\|SETTINGS\.fontSize\|SETTINGS\.lineSpacingHorizontal\|SETTINGS\.lineSpacingVertical\|SETTINGS\.verticalCharSpacing\|SETTINGS\.paragraphAlignment\|SETTINGS\.extraParagraphSpacing\|SETTINGS\.hyphenationEnabled\|SETTINGS\.screenMargin\|SETTINGS\.firstLineIndent\|SETTINGS\.textAntiAliasing\|SETTINGS\.sdFontFamilyName" src/ lib/
```

ヒットがあれば修正。

- [ ] **Step 3: getReaderFontId() の引数なし呼び出しのチェック**

```bash
grep -rn "getReaderFontId()" src/ lib/
```

引数なしの呼び出しが残っていればコンパイルエラーになるが、念のため確認。

- [ ] **Step 4: git diff の確認**

```bash
git diff --stat
```

意図しない変更がないことを確認。

- [ ] **Step 5: フラッシュ**

```bash
pio run -t upload
```
