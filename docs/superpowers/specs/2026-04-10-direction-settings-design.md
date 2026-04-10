# 縦書き/横書き別設定の設計書

**Issue**: zrn-ns/crosspoint-jp#39
**日付**: 2026-04-10
**ステータス**: 承認済み

## 概要

縦書きと横書きでそれぞれ別のフォント・サイズ・スペーシング等のリーダー設定を持てるようにする。設定UIは「横書き設定」「縦書き設定」のサブメニューに分離する。

## 要件

1. リーダー設定のうち方向依存の項目を、横書き用・縦書き用の2セットに分離する
2. `writingMode` が `Auto` のとき、自動判定された方向に応じた設定セットを適用する
3. 既存ユーザーの設定は横書き・縦書き両方にコピーしてマイグレーションする
4. 設定UIに「横書き設定」「縦書き設定」のサブメニューを新設する
5. 読書中クイックメニューの設定項目も現在の方向に応じた値を読み書きする

## データモデル

### DirectionSettings 構造体

`CrossPointSettings.h` に新設する。方向依存の設定を束ねる構造体。

```cpp
struct DirectionSettings {
  uint8_t fontFamily = CrossPointSettings::BOOKERLY;
  char sdFontFamilyName[32] = "";
  uint8_t fontSize = CrossPointSettings::MEDIUM;
  uint8_t lineSpacing = 185;        // 80-250 (%)
  uint8_t charSpacing = 0;          // 0-50 (5刻み)
  uint8_t paragraphAlignment = CrossPointSettings::JUSTIFIED;
  uint8_t extraParagraphSpacing = 0;
  uint8_t hyphenationEnabled = 0;
  uint8_t screenMargin = 10;        // 5-40
  uint8_t firstLineIndent = 1;
  uint8_t textAntiAliasing = 0;
};
```

### CrossPointSettings への統合

```cpp
// 新規メンバ
DirectionSettings horizontal;
DirectionSettings vertical;

// 削除するメンバ（DirectionSettingsに移動）
// fontFamily, sdFontFamilyName, fontSize,
// lineSpacingHorizontal, lineSpacingVertical, verticalCharSpacing,
// paragraphAlignment, extraParagraphSpacing, hyphenationEnabled,
// screenMargin, firstLineIndent, textAntiAliasing

// ヘルパー関数
const DirectionSettings& getDirectionSettings(bool isVertical) const;
DirectionSettings& getDirectionSettings(bool isVertical);
int16_t getReaderFontId(bool isVertical) const;
float getReaderLineCompression(bool isVertical) const;
```

### 分離対象/非対象の分類

**分離する（DirectionSettingsに移動）**:

| フィールド | 旧名 | 説明 |
|-----------|------|------|
| `fontFamily` | `fontFamily` | ビルトインフォント種別 |
| `sdFontFamilyName` | `sdFontFamilyName` | SDカードフォント名 |
| `fontSize` | `fontSize` | フォントサイズ |
| `lineSpacing` | `lineSpacingHorizontal` / `lineSpacingVertical` | 行間 |
| `charSpacing` | `verticalCharSpacing` | 文字間隔 |
| `paragraphAlignment` | `paragraphAlignment` | 段落配置 |
| `extraParagraphSpacing` | `extraParagraphSpacing` | 段落間スペーシング |
| `hyphenationEnabled` | `hyphenationEnabled` | ハイフネーション |
| `screenMargin` | `screenMargin` | 画面マージン |
| `firstLineIndent` | `firstLineIndent` | 段落インデント |
| `textAntiAliasing` | `textAntiAliasing` | テキストAA |

**分離しない（グローバルのまま）**:

| フィールド | 理由 |
|-----------|------|
| `writingMode` | 方向選択そのものなので分離不要 |
| `orientation` | 画面向きは方向に依存しない |
| `embeddedStyle` | 本のCSS適用はグローバルな判断 |
| `imageRendering` | 画像表示モードは方向に依存しない |
| `invertImages` | 画像反転は方向に依存しない |

### RAM影響

`DirectionSettings` ≈ 48バイト × 2 = 96バイト。旧フィールド分（約50バイト）が削除されるため、実質 +46バイト程度。無視可能。

## JSON永続化

### 新フォーマット

```json
{
  "horizontal": {
    "fontFamily": 0,
    "sdFontFamilyName": "",
    "fontSize": 1,
    "lineSpacing": 185,
    "charSpacing": 0,
    "paragraphAlignment": 0,
    "extraParagraphSpacing": 0,
    "hyphenationEnabled": 0,
    "screenMargin": 10,
    "firstLineIndent": 1,
    "textAntiAliasing": 0
  },
  "vertical": {
    "fontFamily": 0,
    "sdFontFamilyName": "",
    "fontSize": 1,
    "lineSpacing": 185,
    "charSpacing": 15,
    "paragraphAlignment": 0,
    "extraParagraphSpacing": 0,
    "hyphenationEnabled": 0,
    "screenMargin": 10,
    "firstLineIndent": 1,
    "textAntiAliasing": 0
  },
  "writingMode": 0,
  "orientation": 0,
  ...
}
```

### マイグレーション（旧JSON → 新JSON）

`JsonSettingsIO::loadSettings()` で処理:

1. 新形式の `"horizontal"` キーが存在すれば → そのまま読み込み
2. 存在しなければ旧形式とみなす:
   - 旧フラットフィールドを読み取り、横書き・縦書き**両方**にコピー
   - 例外: `lineSpacingHorizontal` → `horizontal.lineSpacing`、`lineSpacingVertical` → `vertical.lineSpacing`
   - 例外: `verticalCharSpacing` → `vertical.charSpacing`（横書きは0）
3. 読み込み後に新形式で再保存（自動マイグレーション完了）

### JsonSettingsIOの変更

- `DirectionSettings` 用の設定項目リストを別途定義
- `saveSettings` / `loadSettings` で `"horizontal"` / `"vertical"` をネストオブジェクトとして読み書き
- ArduinoJsonの `JsonObject nested = doc["horizontal"].to<JsonObject>()` でネスト処理

## 設定UI

### メニュー構成

```
設定
├── 表示 (Display)
├── リーダー (Reader)
│   ├── 横書き設定 →  [DirectionSettingsActivity へ遷移]
│   │   ├── フォント
│   │   ├── フォントサイズ
│   │   ├── 行間
│   │   ├── 文字間隔
│   │   ├── 段落配置
│   │   ├── 段落間スペーシング
│   │   ├── ハイフネーション
│   │   ├── 画面マージン
│   │   ├── 段落インデント
│   │   └── テキストAA
│   ├── 縦書き設定 →  [DirectionSettingsActivity へ遷移]
│   │   └── (同上の項目)
│   ├── 記述方向 (Auto/横/縦)
│   ├── 画面向き
│   ├── 埋め込みCSS
│   ├── 画像表示
│   └── 画像反転
├── 操作 (Controls)
└── 本体 (System)
```

### 新規ファイル

- `DirectionSettingsActivity.h/.cpp` — `DirectionSettings&` を受け取り、方向別設定の編集UI
- `DirectionSettingsList.h` — `DirectionSettings` メンバポインタを使った `SettingInfo` リスト

### 実装方針

- `DirectionSettingsActivity` は既存の `SettingsActivity` と同様のリスト表示UIを持つ
- フォント選択は既存の `FontSelectActivity` を再利用
- 行間設定は既存の `LineSpacingSelectionActivity` を再利用
- `SettingsActivity` のリーダーカテゴリから分離した項目を除去し、「横書き設定」「縦書き設定」のエントリを追加

### I18n追加

新規文字列キー:
- `STR_HORIZONTAL_SETTINGS` — 「横書き設定」
- `STR_VERTICAL_SETTINGS` — 「縦書き設定」
- `STR_CHAR_SPACING` — 「文字間隔」（既存 `STR_VERT_CHAR_SPACING` を一般化）

## リーダー側の変更

### EpubReaderActivity

本を開く時の流れ:

1. `writingMode` の判定（Auto/横/縦）→ `isVertical` を決定（既存ロジックそのまま）
2. `SETTINGS.getDirectionSettings(isVertical)` で適切な設定セットを取得
3. 取得した `DirectionSettings` からフォントID、行間、マージン等をレイアウトエンジンに渡す

### EpubReaderMenuActivity（読書中クイックメニュー）

- `verticalMode` は既にコンストラクタで渡されている
- `getMenuItemValue()` で `SETTINGS.firstLineIndent` → `SETTINGS.getDirectionSettings(verticalMode).firstLineIndent` に変更
- 行間表示: 既に `verticalMode` で分岐済み → `getDirectionSettings(verticalMode).lineSpacing` に統一
- トグル操作時も同じ `DirectionSettings` の値を変更

### 影響箇所

| ファイル | 変更内容 |
|---------|---------|
| `EpubReaderActivity.cpp` | 設定参照を `getDirectionSettings(isVertical)` 経由に変更 |
| `EpubReaderMenuActivity.cpp` | クイック設定の読み書きを `getDirectionSettings(verticalMode)` に変更 |
| `ChapterHtmlSlimParser.cpp` | マージン・インデント等の設定参照変更 |
| `CrossPointSettings.cpp` | `getReaderFontId(bool)`, `getReaderLineCompression(bool)` の変更 |

### TxtReaderActivity

TXTリーダーは常に横書き（`isVertical = false`）なので `SETTINGS.getDirectionSettings(false)` を使用。既存動作に変更なし。

### キャッシュバージョン

`SECTION_FILE_VERSION` を 27 → 28 にインクリメント。設定構造の変更により旧キャッシュは自動再生成。

## 変更ファイル一覧

| ファイル | 変更種別 | 変更内容 |
|---------|---------|---------|
| `CrossPointSettings.h` | 変更 | `DirectionSettings` 構造体追加、旧フィールド削除、ヘルパー関数追加 |
| `CrossPointSettings.cpp` | 変更 | `getReaderFontId(bool)`, `getReaderLineCompression(bool)` の変更 |
| `JsonSettingsIO.cpp` | 変更 | ネストJSON読み書き、マイグレーションロジック |
| `SettingsList.h` | 変更 | 分離項目をリーダーカテゴリから除去、横書き/縦書き設定エントリ追加 |
| `DirectionSettingsList.h` | **新規** | `DirectionSettings` 用設定項目リスト |
| `DirectionSettingsActivity.h` | **新規** | 方向別設定サブメニューActivity（ヘッダ） |
| `DirectionSettingsActivity.cpp` | **新規** | 方向別設定サブメニューActivity（実装） |
| `SettingsActivity.cpp` | 変更 | サブメニュー遷移の追加 |
| `EpubReaderActivity.cpp` | 変更 | 設定参照を `getDirectionSettings(isVertical)` 経由に変更 |
| `EpubReaderMenuActivity.cpp` | 変更 | クイック設定の読み書きを方向別に変更 |
| `ChapterHtmlSlimParser.cpp` | 変更 | マージン・インデント等の設定参照変更 |
| `Section.cpp` | 変更 | `SECTION_FILE_VERSION` 27→28 |
| `lib/I18n/translations/*.yaml` | 変更 | 新規文字列追加 |

## 非変更対象

| ファイル/モジュール | 理由 |
|-------------------|------|
| `GfxRenderer` | フォントID・座標を受け取るだけで設定を直接参照しない |
| `ParsedText` | パラメータは呼び出し元から渡される |
| `SdCardFont` / `FontManager` | フォントシステム自体は変更なし |
| `HalStorage` / `HalDisplay` | HAL層は無関係 |
| ホーム画面・ファイルブラウザ | リーダー設定のみの変更 |
