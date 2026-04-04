# SDカードフォント単一バリアント+スケーリング 実装計画

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** SDカードフォントを1ファイル(14pt Regular)のみロードし、描画時にnearest-neighborスケーリング+Bold合成を行うことで、メモリ使用量を削減する

**Architecture:** GfxRendererにスケールファクター管理を追加。SdCardFontManagerは1ファイルをロードし4つの仮想fontIdで登録。renderChar()でビットマップをオンザフライスケーリング。

**Tech Stack:** C++20, ESP32-C3, PlatformIO, 8.8固定小数点演算

**Spec:** `docs/superpowers/specs/2026-04-05-sdcard-font-single-variant-scaling-design.md`

---

## Done 判定基準

- [ ] `pio run` ビルド成功（エラー・警告なし）
- [ ] フォントサイズ小/中/大/特大で文字サイズが変わる
- [ ] 見出し(h1/h2)が本文より大きく太字で表示される
- [ ] 10ページ以上読み進めてもメモリエラーが発生しない
- [ ] `git diff` で意図しない変更がない
※ 必須ゲート（動作検証・既存機能・差分確認・シークレット）は常に適用

---

## ファイル構成

| ファイル | 変更種別 | 責務 |
|---------|---------|------|
| `lib/GfxRenderer/GfxRenderer.h` | 修正 | スケールファクター管理メソッド・メンバ追加 |
| `lib/GfxRenderer/GfxRenderer.cpp` | 修正 | renderChar()スケーリング、getTextAdvanceX/getSpaceAdvance等スケール適用 |
| `lib/EpdFont/SdCardFontManager.h` | 修正 | virtualFontIds_メンバ追加 |
| `lib/EpdFont/SdCardFontManager.cpp` | 修正 | loadFamily()単一ファイルロード+4仮想fontId登録、unloadAll()修正 |
| `src/SdCardFontSystem.cpp` | 修正 | resolveFontId()簡略化（全サイズ登録済みのため） |

---

### Task 1: GfxRenderer — スケールファクター管理の追加

**Files:**
- Modify: `lib/GfxRenderer/GfxRenderer.h:55,112-115`
- Modify: `lib/GfxRenderer/GfxRenderer.cpp` (新メソッド追加は不要、インライン実装)

- [ ] **Step 1: GfxRenderer.hにスケールマップとメソッドを追加**

`sdCardFonts_`宣言(行55付近)の直後に追加:
```cpp
mutable std::map<int, uint16_t> sdCardFontScales_;  // fontId → 8.8固定小数点スケール (256=1.0x)
```

`clearSdCardFonts()`(行114)の直後に追加:
```cpp
void registerSdCardFontScale(int fontId, uint16_t scale) { sdCardFontScales_[fontId] = scale; }
void clearSdCardFontScales() { sdCardFontScales_.clear(); }
uint16_t getSdCardFontScale(int fontId) const {
  auto it = sdCardFontScales_.find(fontId);
  return (it != sdCardFontScales_.end()) ? it->second : 256;
}
```

- [ ] **Step 2: ビルド確認**

Run: `pio run 2>&1 | tail -5`
Expected: SUCCESS

- [ ] **Step 3: コミット**

```bash
git add lib/GfxRenderer/GfxRenderer.h
git commit -m "👍 GfxRendererにSDカードフォントスケールファクター管理を追加"
```

---

### Task 2: SdCardFontManager — 単一ファイルロード+4仮想fontId登録

**Files:**
- Modify: `lib/EpdFont/SdCardFontManager.h:32-40`
- Modify: `lib/EpdFont/SdCardFontManager.cpp:31-111`

- [ ] **Step 1: SdCardFontManager.hにvirtualFontIds_を追加**

LoadedFont構造体の後、メンバ変数セクションに追加:
```cpp
std::vector<int> virtualFontIds_;  // 仮想fontId一覧（unload時に全削除用）
```

- [ ] **Step 2: loadFamily()を単一ファイルロード方式に書き換え**

主な変更点:
1. ベースサイズ(14pt、なければ最も近い)を1つだけロード
2. FONT_SIZE_TO_PT[] = {12,14,16,18}の各サイズに対して仮想fontIdを計算・登録
3. スケールファクターを計算してGfxRendererに登録
4. loaded_には1エントリのみ格納

```cpp
bool SdCardFontManager::loadFamily(const SdCardFontFamilyInfo& family, GfxRenderer& renderer) {
  if (!loadedFamilyName_.empty()) {
    unloadAll(renderer);
  }

  // ベースサイズ選択: 14ptを優先、なければ最も近いサイズ
  static constexpr uint8_t PREFERRED_BASE_PT = 14;
  static constexpr uint8_t ALL_SIZES[] = {12, 14, 16, 18};

  const SdCardFontFileInfo* bestFile = nullptr;
  int bestDiff = INT_MAX;
  for (const auto& fileInfo : family.files) {
    int diff = abs(static_cast<int>(fileInfo.pointSize) - PREFERRED_BASE_PT);
    if (diff < bestDiff) {
      bestDiff = diff;
      bestFile = &fileInfo;
    }
  }
  if (!bestFile) return false;

  auto* font = new (std::nothrow) SdCardFont();
  if (!font) {
    LOG_ERR("SDMGR", "Failed to allocate SdCardFont");
    return false;
  }
  if (!font->load(bestFile->path.c_str())) {
    LOG_ERR("SDMGR", "Failed to load %s", bestFile->path.c_str());
    delete font;
    return false;
  }

  const uint8_t basePt = bestFile->pointSize;
  loaded_.push_back({font, 0, basePt});  // fontId=0はプレースホルダ、後で上書き

  // 各サイズの仮想fontIdを登録
  EpdFontFamily fontFamily(font->getEpdFont(0), font->getEpdFont(1),
                           font->getEpdFont(2), font->getEpdFont(3));
  virtualFontIds_.clear();

  for (uint8_t targetPt : ALL_SIZES) {
    int fontId = computeFontId(font->contentHash(), family.name.c_str(), targetPt);
    if (renderer.getFontMap().count(fontId) != 0) {
      LOG_ERR("SDMGR", "Font ID %d collides, skipping size %u", fontId, targetPt);
      continue;
    }
    renderer.registerSdCardFont(fontId, font);
    renderer.insertFont(fontId, fontFamily);

    // スケールファクター: 8.8固定小数点 (256 = 1.0x)
    uint16_t scale = static_cast<uint16_t>(static_cast<uint32_t>(targetPt) * 256 / basePt);
    renderer.registerSdCardFontScale(fontId, scale);
    virtualFontIds_.push_back(fontId);

    if (targetPt == basePt) {
      loaded_[0].fontId = fontId;  // ベースサイズのfontIdを記録
    }

    LOG_DBG("SDMGR", "Registered size=%u id=%d scale=%u/256 (base=%u)", targetPt, fontId, scale, basePt);
  }

  if (virtualFontIds_.empty()) {
    delete font;
    loaded_.clear();
    return false;
  }

  // loaded_のfontIdがまだ0ならベースサイズのIDが衝突で登録されなかった
  if (loaded_[0].fontId == 0 && !virtualFontIds_.empty()) {
    loaded_[0].fontId = virtualFontIds_[0];
  }

  loadedFamilyName_ = family.name;
  LOG_DBG("SDMGR", "Loaded %s (base=%upt, %zu virtual IDs)", family.name.c_str(), basePt, virtualFontIds_.size());
  return true;
}
```

- [ ] **Step 3: unloadAll()を仮想fontId対応に修正**

```cpp
void SdCardFontManager::unloadAll(GfxRenderer& renderer) {
  // 仮想fontIdを全て削除（sdCardFonts_, fontMap, scaleMap）
  for (int id : virtualFontIds_) {
    renderer.unregisterSdCardFont(id);
    renderer.removeFont(id);
  }
  renderer.clearSdCardFontScales();
  virtualFontIds_.clear();

  // SdCardFontオブジェクトは1つだけdelete
  for (auto& lf : loaded_) {
    delete lf.font;
  }
  loaded_.clear();
  loadedFamilyName_.clear();
}
```

- [ ] **Step 4: getFontId()を修正（全サイズ対応）**

```cpp
int SdCardFontManager::getFontId(const std::string& familyName, uint8_t size, uint8_t /*style*/) const {
  if (familyName != loadedFamilyName_) return 0;
  if (loaded_.empty()) return 0;
  // 仮想fontIdを検索: computeFontIdで算出
  return computeFontId(loaded_[0].font->contentHash(), familyName.c_str(), size);
}
```

注意: このfontIdがvirtualFontIds_に含まれているかの検証は呼び出し側（resolveFontId）で行う。

- [ ] **Step 5: ビルド確認**

Run: `pio run 2>&1 | tail -5`
Expected: SUCCESS

- [ ] **Step 6: コミット**

```bash
git add lib/EpdFont/SdCardFontManager.h lib/EpdFont/SdCardFontManager.cpp
git commit -m "👍 SdCardFontManager: 単一ファイルロード+4仮想fontId登録方式に変更"
```

---

### Task 3: GfxRenderer::getTextAdvanceX/getSpaceAdvance — スケール適用

**Files:**
- Modify: `lib/GfxRenderer/GfxRenderer.cpp:1450-1458,1480-1492`

- [ ] **Step 1: getTextAdvanceX()にスケール適用**

行1484-1492のsdCardFonts_ fast-path:
```cpp
auto sdIt = sdCardFonts_.find(fontId);
if (sdIt != sdCardFonts_.end() && sdIt->second->hasAdvanceTable()) {
  int32_t widthFP = 0;
  const uint8_t styleIdx = static_cast<uint8_t>(style);
  while (uint32_t cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text))) {
    widthFP += sdIt->second->getAdvance(cp, styleIdx);
  }
  const uint16_t scale = getSdCardFontScale(fontId);
  return fp4::toPixel(static_cast<int32_t>(static_cast<int64_t>(widthFP) * scale / 256));
}
```

- [ ] **Step 2: getSpaceAdvance()にスケール適用**

行1455-1458のsdCardFonts_ fast-path:
```cpp
auto sdIt = sdCardFonts_.find(fontId);
if (sdIt != sdCardFonts_.end() && sdIt->second->hasAdvanceTable()) {
  const int32_t advFP = sdIt->second->getAdvance(' ', static_cast<uint8_t>(style));
  const uint16_t scale = getSdCardFontScale(fontId);
  return fp4::toPixel(static_cast<int32_t>(static_cast<int64_t>(advFP) * scale / 256));
}
```

- [ ] **Step 3: ビルド確認**

Run: `pio run 2>&1 | tail -5`
Expected: SUCCESS

- [ ] **Step 4: コミット**

```bash
git add lib/GfxRenderer/GfxRenderer.cpp
git commit -m "👍 getTextAdvanceX/getSpaceAdvanceにSDカードフォントスケール適用"
```

---

### Task 4: GfxRenderer::renderChar() — ビットマップスケーリング+Bold合成

**Files:**
- Modify: `lib/GfxRenderer/GfxRenderer.cpp:2123-2195`

- [ ] **Step 1: renderChar()のEpdFont描画パスにスケーリングを追加**

行2139(`const EpdFontData* fontData = ...`)以降を、SDカードフォント時のスケーリング描画に対応させる。

既存のglyph取得(行2123-2137)は変更なし。行2139以降を以下に置き換え:

```cpp
  const EpdFontData* fontData = fontFamily.getData(style);
  const bool is2Bit = fontData->is2Bit;
  const uint8_t baseW = glyph->width;
  const uint8_t baseH = glyph->height;
  const int baseLeft = glyph->left;

  const uint8_t* bitmap = getGlyphBitmap(fontData, glyph);

  // SDカードフォントスケーリング
  const uint16_t scale = getSdCardFontScale(fontId);
  const bool needsScale = (scale != 256);
  const uint16_t drawW = needsScale ? static_cast<uint16_t>((baseW * scale + 128) >> 8) : baseW;
  const uint16_t drawH = needsScale ? static_cast<uint16_t>((baseH * scale + 128) >> 8) : baseH;
  const int drawLeft = needsScale ? ((baseLeft * static_cast<int>(scale) + 128) >> 8) : baseLeft;
  const int drawTop = needsScale ? ((glyph->top * static_cast<int>(scale) + 128) >> 8) : glyph->top;

  // Bold合成判定: Boldスタイル要求だがフォントにBoldがない場合
  const bool synthBold = (style == EpdFontFamily::BOLD || style == EpdFontFamily::BOLD_ITALIC) &&
                         !fontFamily.hasStyle(style) && needsScale;
  const int boldPasses = synthBold ? 2 : 1;

  if (!bitmap) {
    *x += fp4::toPixel(glyph->advanceX);
    return;
  }

  for (int pass = 0; pass < boldPasses; pass++) {
    const int xOffset = pass;  // Bold: 2回目は1px右シフト
    for (uint16_t glyphY = 0; glyphY < drawH; glyphY++) {
      // ソース座標計算
      const uint16_t srcY = needsScale ? static_cast<uint16_t>(glyphY * 256 / scale) : glyphY;
      if (srcY >= baseH) continue;
      for (uint16_t glyphX = 0; glyphX < drawW; glyphX++) {
        const uint16_t srcX = needsScale ? static_cast<uint16_t>(glyphX * 256 / scale) : glyphX;
        if (srcX >= baseW) continue;

        const uint32_t pixelPosition = srcY * baseW + srcX;
        const int screenX = *x + drawLeft + glyphX + xOffset;
        const int screenY = *y - drawTop + glyphY;

        if (is2Bit) {
          const uint8_t byte = bitmap[pixelPosition / 4];
          const uint8_t bit_index = (3 - pixelPosition % 4) * 2;
          const uint8_t bmpVal = 3 - ((byte >> bit_index) & 0x3);

          if (renderMode_ == BW) {
            if (bmpVal >= 2) {
              if (darkMode_) {
                drawPixel(screenX, screenY, !pixelState);
              } else {
                drawPixel(screenX, screenY, pixelState);
              }
            }
          } else if (renderMode_ == GRAYSCALE_MSB) {
            if (bmpVal & 0x2) {
              drawPixel(screenX, screenY, pixelState);
            }
          } else {  // GRAYSCALE_LSB
            if (bmpVal & 0x1) {
              drawPixel(screenX, screenY, pixelState);
            }
          }
        } else {
          const uint8_t byte = bitmap[pixelPosition / 8];
          const uint8_t bit_index = 7 - (pixelPosition % 8);
          if ((byte >> bit_index) & 1) {
            drawPixel(screenX, screenY, pixelState);
          }
        }
      }
    }
  }

  int advPx = fp4::toPixel(glyph->advanceX);
  if (needsScale) advPx = (advPx * scale + 128) >> 8;
  if (synthBold) advPx += 1;
  *x += advPx;
```

- [ ] **Step 2: ビルド確認**

Run: `pio run 2>&1 | tail -5`
Expected: SUCCESS

- [ ] **Step 3: コミット**

```bash
git add lib/GfxRenderer/GfxRenderer.cpp
git commit -m "👍 renderChar: SDカードフォントのビットマップスケーリング+Bold合成を実装"
```

---

### Task 5: その他のメトリクススケーリング

**Files:**
- Modify: `lib/GfxRenderer/GfxRenderer.cpp` (getBasicSpaceAdvance, getKerning, getData参照箇所)

- [ ] **Step 1: getBasicSpaceAdvance()にスケール適用**

行1424-1448の関数内、SDカードフォントパス:
```cpp
const EpdGlyph* spaceGlyph = fontMap.at(effectiveFontId).getGlyph(' ', style);
if (!spaceGlyph) return 0;
int adv = fp4::toPixel(spaceGlyph->advanceX);
const uint16_t scale = getSdCardFontScale(effectiveFontId);
if (scale != 256) adv = (adv * scale + 128) >> 8;
return adv;
```

- [ ] **Step 2: getKerning()にスケール適用**

行1472-1478:
```cpp
int GfxRenderer::getKerning(const int fontId, ...) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) return 0;
  const int kernFP = fontIt->second.getKerning(leftCp, rightCp, style);
  int kern = fp4::toPixel(kernFP);
  const uint16_t scale = getSdCardFontScale(fontId);
  if (scale != 256) kern = (kern * static_cast<int>(scale) + 128) >> 8;
  return kern;
}
```

- [ ] **Step 3: renderChar末尾のadvanceX(行2194)は既にTask 4で対応済みを確認**

- [ ] **Step 4: ビルド確認**

Run: `pio run 2>&1 | tail -5`
Expected: SUCCESS

- [ ] **Step 5: コミット**

```bash
git add lib/GfxRenderer/GfxRenderer.cpp
git commit -m "👍 getBasicSpaceAdvance/getKerningにSDカードフォントスケール適用"
```

---

### Task 6: 最終ビルド検証+クリーンアップ

**Files:**
- All modified files

- [ ] **Step 1: クリーンビルド**

Run: `pio run -t clean && pio run 2>&1 | tail -10`
Expected: SUCCESS, RAM/Flash使用量が大きく変わらないこと

- [ ] **Step 2: git diffで差分確認**

Run: `git diff HEAD~5 --stat`
確認: 意図しないファイルが含まれていないこと

- [ ] **Step 3: デバイスにフラッシュ**

Run: `pio run -t upload 2>&1 | tail -5`
Expected: SUCCESS
