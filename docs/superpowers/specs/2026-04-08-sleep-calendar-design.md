# スリープ画面カレンダー表示 設計書

**Issue**: zrn-ns/crosspoint-jp#36
**日付**: 2026-04-08

## 概要

既存のスリープ画面モード（DARK/LIGHT/BLANK/CUSTOM/COVER/COVER_CUSTOM）の上にカレンダーをオーバーレイ描画するオプション機能を追加する。

## 機能仕様

### カレンダー描画

- 当月のみ表示、日曜始まり
- 縦向き（480x800）固定で描画
- ヘッダー: 「2026 / 4」形式の年月表示
- 曜日行: 日/月/火/水/木/金/土
- 今日の日付: 黒丸＋白文字（反転表示）。DARKモードでは`invertScreen()`により自動的に白丸+黒文字に反転
- 日曜列: 太字で描画
- カレンダー領域には白背景の矩形を敷いてから描画（DARKモード時は`invertScreen()`で黒背景になる）

### 表示条件

- カレンダー表示設定がONの場合のみ描画
- システム時刻が有効な場合のみ描画（NTP未同期＝エポック付近の場合はスキップし、カレンダーなしのスリープ画面を表示）
  - 判定: `time(nullptr)` が 2024年1月1日（1704067200）以降であれば有効とみなす
- 全スリープ画面モードとの組み合わせが可能

### 配置

- 上部 / 中央 / 下部 の3択を設定で選択可能
- カレンダーOFF時は配置設定をUIで非表示（無効化）

## 設定項目

| 設定キー | 型 | 値 | デフォルト |
|---------|-----|-----|---------|
| `sleepCalendar` | `uint8_t` | 0=OFF, 1=ON | 0 (OFF) |
| `sleepCalendarPosition` | `uint8_t` | 0=上部, 1=中央, 2=下部 | 1 (中央) |

## 描画仕様

### レイアウト（縦向き 480x800）

- カレンダー全体幅: 約336px（7列 x 48px）、画面中央に水平配置
- 背景矩形: カレンダー全体を囲む白矩形（パディング含む）
- 年月ヘッダー: UIフォント（UI_10_FONT_ID）、太字、中央揃え
- 曜日ヘッダー: UIフォント（SMALL_FONT_ID）、太字、各列中央揃え
- 日付: UIフォント（UI_10_FONT_ID）、各セル中央揃え
  - 日曜列: 太字（EpdFontFamily::BOLD）
  - 今日: `fillCircle` + 白文字で反転描画
- 配置オフセット（Y軸）:
  - 上部: ベゼルマージン + 固定パディング
  - 中央: 画面高さの中心にカレンダー中心を合わせる
  - 下部: 画面下端 - ベゼルマージン - 固定パディング

### フォント使用

- 年月ヘッダー: `UI_10_FONT_ID`, `EpdFontFamily::BOLD`
- 曜日ヘッダー: `SMALL_FONT_ID`, `EpdFontFamily::BOLD`
- 日付（通常）: `UI_10_FONT_ID`, `EpdFontFamily::REGULAR`
- 日付（日曜）: `UI_10_FONT_ID`, `EpdFontFamily::BOLD`
- 日付（今日）: `UI_10_FONT_ID`, `EpdFontFamily::BOLD`, 反転描画

## 実装箇所

### CrossPointSettings（設定追加）

- `src/CrossPointSettings.h`: `sleepCalendar`, `sleepCalendarPosition` フィールド追加、enum追加
- `src/CrossPointSettings.cpp`: シリアライズ/デシリアライズに追加

### SleepActivity（描画ロジック）

- `src/activities/boot_sleep/SleepActivity.h`: `renderCalendarOverlay()` メソッド追加
- `src/activities/boot_sleep/SleepActivity.cpp`:
  - `renderCalendarOverlay()`: カレンダー描画ロジック（日付計算、グリッド描画、今日の反転）
  - `onEnter()`: 各renderメソッド呼び出し後、`displayBuffer()`呼び出し前にカレンダーオーバーレイを挿入
  - 時刻有効性チェック: `time(nullptr)` >= 1704067200 (2024-01-01)

### 設定UI

- 既存のスリープ画面設定セクションに以下を追加:
  - 「カレンダー表示」ON/OFFトグル
  - 「カレンダー配置」選択（上部/中央/下部）— カレンダーOFF時は非表示

### I18n（翻訳）

- 必要な文字列キー:
  - `STR_SLEEP_CALENDAR`: 「カレンダー表示」
  - `STR_SLEEP_CALENDAR_POSITION`: 「カレンダー配置」
  - `STR_CALENDAR_POS_TOP`: 「上部」
  - `STR_CALENDAR_POS_CENTER`: 「中央」
  - `STR_CALENDAR_POS_BOTTOM`: 「下部」
  - 曜日: `STR_SUN`, `STR_MON`, `STR_TUE`, `STR_WED`, `STR_THU`, `STR_FRI`, `STR_SAT`

## 描画フロー

既存の各renderメソッドは内部で`displayBuffer()`を呼んでいる。カレンダーオーバーレイのために二重リフレッシュを避けるため、描画と表示を分離するリファクタが必要。

```
SleepActivity::onEnter()
  ├── switch(SETTINGS.sleepScreen)
  │   ├── renderBlankSleepScreen()     ← clearScreenのみ（displayBufferなし）
  │   ├── renderDefaultSleepScreen()   ← ロゴ描画のみ（displayBufferなし）
  │   ├── renderCustomSleepScreen()    ← BMP描画のみ（displayBufferなし）
  │   └── renderCoverSleepScreen()     ← 表紙描画のみ（displayBufferなし）
  │
  ├── if (SETTINGS.sleepCalendar == ON && timeIsValid())
  │   └── renderCalendarOverlay()      ← バッファにカレンダーを上書き描画
  │
  └── displayBuffer(HALF_REFRESH)      ← 一括表示
```

注意: DARKモードの`invertScreen()`はカレンダー描画前に呼ばれるため、カレンダーも自動的に反転される（黒背景+白文字）。グレースケール対応のBMP（CUSTOM/COVER）はBWパス後にカレンダーを描画し、その後グレースケールパスを実行。カレンダー部分はBWのまま（テキストはクリスプな方が良い）。

## スコープ外

- 祝日表示（将来の別Issueとして分離）
- 横向き対応
- 前月・来月の表示
- カレンダーフォントのカスタマイズ
