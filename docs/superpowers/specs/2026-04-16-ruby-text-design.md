# ルビ（振り仮名）表示対応

**Issue**: zrn-ns/crosspoint-jp#43
**日付**: 2026-04-16

## 背景

日本語の縦書き小説ではルビ（振り仮名）表示が重要だが、現在のEPUBレンダラーは `<rt>` タグを `SKIP_TAGS` に含めて完全に無視している。横書き・縦書きの両方でルビを表示できるようにする。

## 要件

- EPUB内の `<ruby><rt>` タグからルビテキストを正しくパース・表示
- 横書き: ルビを親文字の**上側**に小フォントで表示
- 縦書き: ルビを親文字の**右側**に小フォントで表示
- ルビ付きの行/列は、ルビ分のスペースを確保して行間/列間を拡大
- 改行時にルビが分断されない設計（パース時にCJK文字単位で分割）
- 380KB RAM制約を考慮した省メモリ設計

## 設計

### 1. パーサー層の変更

**ファイル**: `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp`

#### 1a. SKIP_TAGSの修正

`<rt>` と `<rp>` を `SKIP_TAGS` から削除する。

```cpp
// 変更前:
const char* SKIP_TAGS[] = {"head", "rt", "rp"};

// 変更後:
const char* SKIP_TAGS[] = {"head", "rp"};
```

`<rp>` は引き続きスキップ（ルビ非対応環境向けの括弧表示なので不要）。

#### 1b. ルビモードの管理

パーサーに以下の状態を追加:

```cpp
bool inRuby = false;           // <ruby> タグ内かどうか
int rubyStartWordIndex = -1;   // <ruby> 開始時の単語インデックス
bool collectingRubyText = false; // <rt> タグ内かどうか
std::string rubyTextBuffer;     // <rt> 内のテキストを蓄積
```

**タグ処理フロー**:
- `<ruby>` 開始 → `inRuby = true`、`rubyStartWordIndex` を現在の単語数に設定
- `<rt>` 開始 → `collectingRubyText = true`
- `<rt>` 内のテキスト → `rubyTextBuffer` に蓄積（通常の単語追加はしない）
- `</rt>` 終了 → `collectingRubyText = false`
- `</ruby>` 終了 → ルビの均等分割を実行し、各単語に紐付け

#### 1c. CJK文字単位でのルビ均等分割

`</ruby>` 終了時の処理:

```
baseWordCount = 現在の単語数 - rubyStartWordIndex
rubyText = rubyTextBuffer の内容
rubyLen = rubyText の文字数（UTF-8文字単位）

各 baseWord[i] (i = 0..baseWordCount-1) に対して:
  start = i * rubyLen / baseWordCount
  end = (i + 1) * rubyLen / baseWordCount
  baseWord[i].ruby = rubyText[start..end]
```

この均等分割により、1親文字 = 1ルビ単位となり、レイアウト層での改行処理が自然に動作する。

非CJK文字のルビ（稀なケース）は1単語にまとめてルビグループ全体を紐付ける。

### 2. データ構造の変更

**ファイル**: `lib/Epub/Epub/ParsedText.h`

#### 2a. Word構造にルビフィールドを追加

現在 `ParsedText` は `addWord()` で単語を追加している。ルビテキストを単語に直接紐付ける:

```cpp
// ParsedText の単語情報に追加
struct WordEntry {
  // 既存フィールド（words, wordStyles 等のベクタで管理）
  // ...
  std::string rubyText;  // 空文字列 = ルビなし
};
```

実装方法: 既存の並列ベクタ（`words`, `wordStyles` 等）に `rubyTexts` ベクタを追加:

```cpp
std::vector<std::string> rubyTexts;  // words と同サイズ、ルビなしは空文字列
```

メモリ効率: `std::string` の空文字列は SSO (Small String Optimization) により追加ヒープ割り当てなし（24バイト/エントリ on ESP32-C3）。大半のエントリが空のため、実質的なヒープ増加はルビ付きの単語数×ルビ文字列長のみ。

**ファイル**: `lib/Epub/Epub/blocks/TextBlock.h`

TextBlock にもルビ情報を引き継ぐ:

```cpp
std::vector<std::string> rubyTexts;  // words と並列
```

### 3. レイアウト計算の変更

**ファイル**: `lib/Epub/Epub/ParsedText.cpp`

#### 3a. 横書きレイアウト

`layoutAndExtractLines()` の行高さ計算を修正:

- 各行について、ルビ付き単語が1つ以上あるかチェック
- ルビ付きの行: `lineHeight = baseFontHeight + rubyFontHeight + rubyGap`
- ルビなしの行: `lineHeight = baseFontHeight`（従来通り）
- `rubyGap` はルビと親文字の間隔（2px程度）

#### 3b. 縦書きレイアウト

`layoutVerticalColumns()` の列幅計算を修正:

- 各列について、ルビ付き単語が1つ以上あるかチェック
- ルビ付きの列: `columnWidth = baseFontWidth + rubyFontWidth + rubyGap`
- ルビなしの列: `columnWidth = baseFontWidth`（従来通り）

### 4. レンダリングの変更

**ファイル**: `lib/Epub/Epub/blocks/TextBlock.cpp`

#### 4a. 横書きルビ描画

`render()` メソッド内、各単語の描画後にルビを描画:

```
if (word.hasRuby):
  rubyWidth = getTextWidth(rubyFontId, rubyText)
  baseWidth = getTextWidth(baseFontId, baseText)
  rubyX = wordX + (baseWidth - rubyWidth) / 2  // センタリング
  rubyY = wordY - rubyFontHeight - rubyGap      // 親文字の上
  drawText(rubyFontId, rubyX, rubyY, rubyText)
```

#### 4b. 縦書きルビ描画

```
if (word.hasRuby):
  rubyX = wordX + baseFontWidth + rubyGap  // 親文字の右側
  // ルビテキストを縦方向に描画
  drawTextVertical(rubyFontId, rubyX, rubyY, rubyText)
```

### 5. ルビフォントの決定

ルビフォントは親文字フォントの約半分のサイズを使用する。

**SDカードフォント使用時**:
- 親フォントが16pt → ルビは8pt（同じフォントファミリーで探す）
- 該当サイズがない場合は最小利用可能サイズにフォールバック

**ビルトインフォント使用時**:
- `SMALL_FONT_ID` または `UI_10_FONT_ID` をルビ用として使用

**決定ロジック**: `SETTINGS` のフォントサイズから算出し、レンダリング時に動的に選択。新しいフォントIDの追加は不要（既存のフォントを流用）。

### 6. セクションキャッシュの変更

**ファイル**: `lib/Epub/Epub/Section.cpp`

- `SECTION_FILE_VERSION` をインクリメント（既存キャッシュの自動再生成）
- TextBlock のシリアライズ/デシリアライズにルビ情報を追加
- ルビなしの単語は空文字列のシリアライズ（0バイト長）

## 影響範囲

| ファイル | 変更内容 |
|---------|---------|
| `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp` | ルビパース処理追加 |
| `lib/Epub/Epub/ParsedText.h` | `rubyTexts` ベクタ追加 |
| `lib/Epub/Epub/ParsedText.cpp` | レイアウト計算にルビ高さ/幅を反映 |
| `lib/Epub/Epub/blocks/TextBlock.h` | `rubyTexts` ベクタ追加 |
| `lib/Epub/Epub/blocks/TextBlock.cpp` | ルビ描画ロジック追加 |
| `lib/Epub/Epub/Section.cpp` | キャッシュバージョン+1、ルビシリアライズ |

## 検証方法

1. `pio run` でビルド成功
2. 実機テスト:
   - ルビ付きEPUB（青空文庫の小説等）を開き、振り仮名が表示されること
   - 横書き: ルビが親文字の上に表示
   - 縦書き: ルビが親文字の右側に表示
   - 改行位置でルビが正しく分割されること
   - ルビのない書籍のレイアウトが変わらないこと（回帰なし）
   - ルビなしの行/列の行間/列幅が従来と同じこと
3. 旧キャッシュが自動再生成されること
