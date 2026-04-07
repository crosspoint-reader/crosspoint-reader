# EPUBキャッシュ事前生成 実装プラン

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** キャッシュがないEPUBを開いた時に確認ダイアログを表示し、承認後に全セクション＋画像キャッシュを一括生成する

**Architecture:** `EpubReaderActivity::onEnter()` でキャッシュ有無を判定し、ConfirmationActivity で確認後、全spine itemsをループして Section::createSectionFile + JPEG→BMP変換を実行。既存の `GUI.drawPopup` / `GUI.fillPopupProgress` でプログレス表示。

**Tech Stack:** C++20, ESP-IDF, PlatformIO, GfxRenderer, JpegToBmpConverter

**Spec:** `docs/superpowers/specs/2026-04-07-epub-cache-pregeneration-design.md`

---

## ファイル構成

| 操作 | パス | 責務 |
|------|------|------|
| 変更 | `src/activities/reader/EpubReaderActivity.h` | `pregenerateCache()` メソッド宣言追加 |
| 変更 | `src/activities/reader/EpubReaderActivity.cpp` | キャッシュ有無判定、確認ダイアログ、事前生成ループ実装 |
| 変更 | `lib/I18n/translations/japanese.yaml` | i18n文字列追加 |
| 変更 | `lib/I18n/translations/english.yaml` | i18n文字列追加 |

---

### Task 1: i18n文字列を追加

**Files:**
- Modify: `lib/I18n/translations/japanese.yaml`
- Modify: `lib/I18n/translations/english.yaml`

- [ ] **Step 1: japanese.yamlに文字列追加**

`lib/I18n/translations/japanese.yaml` の `STR_MEMORY_ERROR` の後に以下を追加:

```yaml
STR_GENERATE_CACHE: "キャッシュを生成しますか？"
STR_GENERATING_CACHE: "キャッシュを生成中..."
```

- [ ] **Step 2: english.yamlに文字列追加**

`lib/I18n/translations/english.yaml` の対応箇所に以下を追加:

```yaml
STR_GENERATE_CACHE: "Generate cache?"
STR_GENERATING_CACHE: "Generating cache..."
```

- [ ] **Step 3: i18nヘッダーを再生成**

```bash
python scripts/gen_i18n.py lib/I18n/translations lib/I18n/
```

- [ ] **Step 4: ビルドして文字列が認識されることを確認**

```bash
pio run
```

Expected: SUCCESS

- [ ] **Step 5: コミット**

```bash
git add lib/I18n/translations/japanese.yaml lib/I18n/translations/english.yaml
git commit -m "✨ キャッシュ事前生成のi18n文字列を追加（Issue #8）"
```

---

### Task 2: pregenerateCache() メソッドを実装

**Files:**
- Modify: `src/activities/reader/EpubReaderActivity.h`
- Modify: `src/activities/reader/EpubReaderActivity.cpp`

- [ ] **Step 1: ヘッダーにメソッド宣言を追加**

`src/activities/reader/EpubReaderActivity.h` の `private:` セクション、`void pageTurn(bool isForwardTurn);` の後に追加:

```cpp
  void pregenerateCache();
```

- [ ] **Step 2: pregenerateCache() を実装**

`src/activities/reader/EpubReaderActivity.cpp` に以下のメソッドを追加（`onEnter()` の前に配置）:

```cpp
void EpubReaderActivity::pregenerateCache() {
  if (!epub) return;

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount <= 0) return;

  // Calculate viewport dimensions (same logic as render())
  const auto orientedViewable = renderer.getOrientedViewableTRBL();
  int orientedMarginTop = orientedViewable.top;
  int orientedMarginRight = orientedViewable.right;
  int orientedMarginBottom = orientedViewable.bottom;
  int orientedMarginLeft = orientedViewable.left;
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  orientedMarginBottom += std::max(SETTINGS.screenMargin, statusBarHeight);

  const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;

  // Resolve writing mode
  bool isVertical = false;
  if (SETTINGS.writingMode == CrossPointSettings::WM_VERTICAL) {
    isVertical = true;
  } else if (SETTINGS.writingMode == CrossPointSettings::WM_HORIZONTAL) {
    isVertical = false;
  } else {
    isVertical = epub && epub->isPageProgressionRtl() &&
                 (epub->getLanguage() == "ja" || epub->getLanguage() == "jpn" ||
                  epub->getLanguage() == "zh" || epub->getLanguage() == "zho");
  }

  const float lineCompression = SETTINGS.getReaderLineCompression(isVertical);
  renderer.setVerticalCharSpacing(SETTINGS.getVerticalCharSpacingPercent());

  // Free font cache to maximize heap for section building
  auto* fcm = renderer.getFontCacheManager();
  if (fcm) {
    fcm->clearCache();
    fcm->freeKernLigatureData();
  }

  const int headingFontIds[6] = {SETTINGS.getHeadingFontId(1), SETTINGS.getHeadingFontId(2), 0, 0, 0, 0};

  // Show initial popup
  Rect popupRect = GUI.drawPopup(renderer, tr(STR_GENERATING_CACHE));
  GUI.fillPopupProgress(renderer, popupRect, 0);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);

  for (int i = 0; i < spineCount; i++) {
    // Update progress
    const int progress = (i * 100) / spineCount;
    GUI.fillPopupProgress(renderer, popupRect, progress);
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);

    // Create section cache
    Section sec(epub, i, renderer);
    if (sec.loadSectionFile(SETTINGS.getReaderFontId(), lineCompression, SETTINGS.extraParagraphSpacing,
                            SETTINGS.paragraphAlignment, viewportWidth, viewportHeight,
                            SETTINGS.hyphenationEnabled, SETTINGS.firstLineIndent, SETTINGS.embeddedStyle,
                            SETTINGS.imageRendering, isVertical)) {
      // Already cached, skip
      continue;
    }

    if (!sec.createSectionFile(SETTINGS.getReaderFontId(), lineCompression, SETTINGS.extraParagraphSpacing,
                               SETTINGS.paragraphAlignment, viewportWidth, viewportHeight,
                               SETTINGS.hyphenationEnabled, SETTINGS.firstLineIndent, SETTINGS.embeddedStyle,
                               SETTINGS.imageRendering, isVertical, nullptr, headingFontIds,
                               SETTINGS.getTableFontId())) {
      LOG_ERR("ERS", "Pregenerate: failed to create section %d (heap: %d)", i, ESP.getFreeHeap());
      continue;  // Skip failed sections, don't abort entire generation
    }

    // Generate image BMP caches for this section
    const std::string imgPrefix = epub->getCachePath() + "/img_" + std::to_string(i) + "_";
    for (int j = 0; ; j++) {
      // Check for .jpg and .jpeg
      std::string jpgPath = imgPrefix + std::to_string(j) + ".jpg";
      if (!Storage.exists(jpgPath.c_str())) {
        jpgPath = imgPrefix + std::to_string(j) + ".jpeg";
        if (!Storage.exists(jpgPath.c_str())) break;
      }

      std::string bmpCachePath;
      {
        const size_t dotPos = jpgPath.rfind('.');
        bmpCachePath = jpgPath.substr(0, dotPos) + ".pxc.bmp";
      }
      if (Storage.exists(bmpCachePath.c_str())) continue;

      FsFile jpegFile, bmpFile;
      if (Storage.openFileForRead("PRE", jpgPath, jpegFile) &&
          Storage.openFileForWrite("PRE", bmpCachePath, bmpFile)) {
        JpegToBmpConverter::jpegFileToBmpStreamWithSize(jpegFile, bmpFile, viewportWidth, viewportHeight);
        jpegFile.close();
        bmpFile.close();
      }
    }
  }

  // Final progress
  GUI.fillPopupProgress(renderer, popupRect, 100);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  delay(500);
}
```

- [ ] **Step 3: 必要なインクルードを追加**

`EpubReaderActivity.cpp` の先頭に以下が含まれていることを確認（なければ追加）:

```cpp
#include <JpegToBmpConverter.h>
```

- [ ] **Step 4: ビルド確認**

```bash
pio run
```

Expected: SUCCESS

- [ ] **Step 5: コミット**

```bash
git add src/activities/reader/EpubReaderActivity.h src/activities/reader/EpubReaderActivity.cpp
git commit -m "✨ pregenerateCache()メソッドを実装（Issue #8）"
```

---

### Task 3: onEnter() にキャッシュ判定と確認ダイアログを追加

**Files:**
- Modify: `src/activities/reader/EpubReaderActivity.cpp`

- [ ] **Step 1: onEnter() でキャッシュ有無を判定し確認ダイアログを表示**

`EpubReaderActivity::onEnter()` の `requestUpdate();` (94行目) の直前に以下を挿入:

```cpp
  // Check if section cache exists; offer to pregenerate if missing
  {
    const std::string sectionsDir = epub->getCachePath() + "/sections";
    bool hasCachedSections = false;
    FsFile dir;
    if (Storage.openDir(sectionsDir, dir)) {
      FsFile entry = dir.openNextFile();
      if (entry) {
        hasCachedSections = true;
        entry.close();
      }
      dir.close();
    }

    if (!hasCachedSections) {
      auto handler = [this](const ActivityResult& res) {
        if (!res.isCancelled) {
          pregenerateCache();
        }
        requestUpdate();
      };
      startActivityForResult(
          std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_GENERATE_CACHE), epub->getTitle()),
          handler);
      return;
    }
  }
```

- [ ] **Step 2: ConfirmationActivity のインクルードを確認**

`EpubReaderActivity.cpp` に以下が含まれていることを確認:

```cpp
#include "activities/util/ConfirmationActivity.h"
```

- [ ] **Step 3: Storage.openDir の存在を確認**

`HalStorage` に `openDir` メソッドがあるか確認。なければ代替方法（`Storage.exists(sectionsDir + "/0.bin")` でspine 0のキャッシュ有無をチェック）に変更:

```cpp
    if (!hasCachedSections) {
```

を以下に置き換え:

```cpp
    // Check if at least the first section is cached
    const std::string firstSectionPath = epub->getCachePath() + "/sections/0.bin";
    if (!Storage.exists(firstSectionPath.c_str())) {
```

（`hasCachedSections` 変数と `FsFile dir` 関連のコードは削除）

- [ ] **Step 4: ビルド確認**

```bash
pio run
```

Expected: SUCCESS

- [ ] **Step 5: フラッシュして動作確認**

```bash
pio run -t upload --upload-port /dev/tty.usbmodem101
```

キャッシュクリア後に本を開き:
1. 確認ダイアログが表示されること
2. 承認 → プログレスバー付きでキャッシュ生成が行われること
3. 完了後、通常のリーダー画面が表示されること
4. ページ送りが高速であること
5. キャンセル → 通常のオンデマンド生成で動作すること

- [ ] **Step 6: コミット**

```bash
git add src/activities/reader/EpubReaderActivity.cpp
git commit -m "✨ 初回オープン時のキャッシュ事前生成ダイアログを追加（Issue #8）"
```
