# Optimize EPUB機能のX3対応 設計書

## 背景

`src/network/html/FilesPage.html` の「Optimize EPUB」機能（EPUB内画像をデバイス画面に合わせてリサイズ／分割／回転する処理）が X4 専用にハードコードされており、X3 で正しく動作しない。

| 機種 | 縦向き解像度 |
|------|-------------|
| X4   | 480 × 800    |
| X3   | 528 × 792    |

X3 でも適切に画像最適化が行われるよう、デバイス解像度を動的に取得して処理に反映させる。

## 既存の状態

- `/api/status` は `renderer.getScreenWidth()` / `getScreenHeight()` を orientation-aware で返す（実装済み）
- `FilesPage.html` の `fetchVersion()` は `deviceScreenWidth` / `deviceScreenHeight` をAPIから取得済み
- ZIP→XTC コンバーター (`convertZipToXtc`) は `deviceScreenWidth/Height` を直接参照している（X3対応済み）
- XTCリーダー (`XtcReaderActivity`) はページメタデータの `pageWidth/pageHeight` を使用するため解像度非依存

## 問題点

`FilesPage.html` の Optimize EPUB ロジックでは以下が X4 用にハードコードされている。

1. **MAX_WIDTH / MAX_HEIGHT 定数** (line 3281-3289)
   ```js
   const DEFAULT_MAX_WIDTH = 480;
   const DEFAULT_MAX_HEIGHT = 800;
   let MAX_WIDTH = DEFAULT_MAX_WIDTH;
   let MAX_HEIGHT = DEFAULT_MAX_HEIGHT;
   ```
   API取得後に更新されない。

2. **インラインの `480` / `800` リテラル**
   - `extractEpubImages` (line 2391, 2396-2397): 画面フィット判定／Split可否判定
   - `renderImageGrid` (line 2635-2644): Split プレビュー計算

3. **静的UIラベル** (line 1543)
   ```html
   <span>📏 Max 480×800px</span>
   ```

## 設計方針

### 1. 縦向き正規化方式の採用

Optimize EPUB の内部ロジック（H-Split, V-Split, Rotate）は「MAX_HEIGHT が長辺、MAX_WIDTH が短辺」を前提に組まれている。デバイスがランドスケープ向きで Web ページが開かれた場合に API が短辺・長辺を逆転して返してくるため、必ず以下で正規化する:

```js
MAX_WIDTH  = Math.min(deviceScreenWidth, deviceScreenHeight);
MAX_HEIGHT = Math.max(deviceScreenWidth, deviceScreenHeight);
```

これにより Optimize EPUB は常に「縦向き読書を想定した最適化」として機能する。これはデバイス向きにかかわらず一貫した挙動となる。

### 2. デバイス解像度の取得と反映

`fetchVersion()` で API レスポンスから `deviceScreenWidth/Height` を取得した直後に `updateOptimizerDimensions()` を呼び出し、`MAX_WIDTH/MAX_HEIGHT` および UI ラベルを更新する。

### 3. ハードコードされたリテラルの置換

すべての `480` / `800` リテラルを `MAX_WIDTH` / `MAX_HEIGHT` に置換する。コメント内の数値は説明的な表現に変える（例: `// 800` → `// MAX_HEIGHT`）。

## 変更内容

### スコープ
- 修正対象ファイル: `src/network/html/FilesPage.html` のみ
- バックエンド (`CrossPointWebServer.cpp`)・XTCリーダー・コンバーターには変更なし

### 詳細変更

#### A. JS関数の追加（line 3320 周辺）

```js
function updateOptimizerDimensions() {
  // Optimize EPUBは常に縦向き想定: 短辺=MAX_WIDTH, 長辺=MAX_HEIGHT
  MAX_WIDTH  = Math.min(deviceScreenWidth, deviceScreenHeight);
  MAX_HEIGHT = Math.max(deviceScreenWidth, deviceScreenHeight);
  updateOptimizerLabel();
}

function updateOptimizerLabel() {
  const el = document.getElementById('optimizerResolutionInfo');
  if (el) el.textContent = `📏 Max ${MAX_WIDTH}×${MAX_HEIGHT}px`;
}
```

`fetchVersion()` 内で `deviceScreenWidth/Height` 更新後に `updateOptimizerDimensions()` を呼ぶ。

#### B. 静的ラベルへの ID 付与（line 1543）

```html
<!-- Before -->
<span>📏 Max 480×800px</span>

<!-- After -->
<span id="optimizerResolutionInfo">📏 Max 480×800px</span>
```

初期値は X4 デフォルト。API応答後に動的更新される。

#### C. リテラル置換一覧

| 行 | Before | After |
|----|--------|-------|
| 2391 | `dims.width <= 480 && dims.height <= 800` | `dims.width <= MAX_WIDTH && dims.height <= MAX_HEIGHT` |
| 2396 | `dims.width >= 800` | `dims.width >= MAX_HEIGHT` |
| 2397 | `dims.height >= 800` | `dims.height >= MAX_HEIGHT` |
| 2636 | `Math.round(img.height * (800 / img.width))` | `Math.round(img.height * (MAX_HEIGHT / img.width))` |
| 2640 | `Math.round(img.width * (800 / img.height))` | `Math.round(img.width * (MAX_HEIGHT / img.height))` |
| 2642 | `if (finalWidth > 480)` | `if (finalWidth > MAX_WIDTH)` |
| 2643 | `Math.round(480 * (OVERLAP_PERCENT / 100))` | `Math.round(MAX_WIDTH * (OVERLAP_PERCENT / 100))` |
| 2644 | `480 - minOverlapPx` | `MAX_WIDTH - minOverlapPx` |

`processImage` (line 4150-4451) 内の `MAX_HEIGHT` を含むコメント（`// 800 / origW` など）は説明的な表現に修正する。実際のコードは既に `MAX_WIDTH/MAX_HEIGHT` を使用しているため、ロジック変更は不要。

#### D. ビルド成果物の再生成

HTML編集後、`pio run` を実行することで `scripts/build_html.py` が `src/network/html/FilesPageHtml.generated.h` を自動再生成する。`.generated.h` はコミット対象外（`.gitignore` 済み）。

## Done判定基準

- [ ] `MAX_WIDTH/MAX_HEIGHT` が API取得後にデバイス解像度から更新される
- [ ] ラベル `📏 Max ...×...px` がデバイス解像度に応じて動的更新される
- [ ] `extractEpubImages` および `renderImageGrid` 内のすべての `480`/`800` リテラルが `MAX_WIDTH`/`MAX_HEIGHT` に置換されている
- [ ] X4ビルド (`pio run`) が通り、`FilesPageHtml.generated.h` が再生成される
- [ ] X4 で従来挙動と一致（リグレッションなし）: `📏 Max 480×800px` 表示、画像処理結果が変わらない
- [ ] X3 を想定した API応答（screenWidth=528, screenHeight=792）で `📏 Max 528×792px` に更新され、画像処理が新解像度で実施される
- [ ] ランドスケープ orientation でも `Math.min/max` により縦向き相当の値に正規化される

※ 必須ゲート（動作検証・既存機能・差分確認・シークレット）は常に適用

## 動作検証

### X4 (実機)
1. ファイル管理画面を開く → ラベル `📏 Max 480×800px`
2. EPUB アップロード時 Optimize ON → 画像が 480×800 内にリサイズされる（従来と一致）

### X3 (実機)
1. ファイル管理画面を開く → ラベル `📏 Max 528×792px`
2. EPUB アップロード時 Optimize ON → 画像が 528×792 内にリサイズされる
3. 横向き表示の画像が H-Split で 792 幅にスケール後、528 幅に分割される

### 横向き orientation でのアクセス（任意）
- 横向きでも縦向き相当の値（X3: 528×792）でラベルが表示される
- Optimize 結果も縦向き想定の出力になる

## 影響範囲

- ✅ Optimize EPUB 機能のみ
- ✅ ZIP→XTC 変換は既に `deviceScreenWidth/Height` を直接使用しており追加変更不要
- ✅ XTCリーダー（端末側 C++）は無変更で X3 解像度の XTC を再生可能
- ✅ バックエンドAPI は無変更
