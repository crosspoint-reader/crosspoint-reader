#include "ChapterHtmlSlimParser.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Utf8.h>
#include <expat.h>

#include <algorithm>
#include <cctype>

#include "../../Epub.h"
#include "../Page.h"
#include "../converters/ImageDecoderFactory.h"
#include "../converters/ImageToFramebufferDecoder.h"
#include "../htmlEntities.h"

const char* HEADER_TAGS[] = {"h1", "h2", "h3", "h4", "h5", "h6"};
constexpr int NUM_HEADER_TAGS = sizeof(HEADER_TAGS) / sizeof(HEADER_TAGS[0]);

// Size thresholds (bytes of XHTML) controlling indexing popup behavior.
// Each progress callback costs ~640ms of e-ink refresh, so we trade granularity off
// against indexing time based on expected duration.
//   < 15KB:  no popup at all - indexing finishes faster than the popup would draw
//   < 30KB:  popup only (one refresh up-front, no mid-parse updates)
//   < 80KB:  popup + one heartbeat at 50%
//   >= 80KB: popup + ticks at 25/50/75%
constexpr size_t MIN_SIZE_FOR_POPUP = 15 * 1024;
constexpr size_t SIZE_FOR_PROGRESS_HEARTBEAT = 30 * 1024;
constexpr size_t SIZE_FOR_PROGRESS_FINE = 80 * 1024;
constexpr size_t PARSE_BUFFER_SIZE = 1024;

const char* BLOCK_TAGS[] = {"p", "li", "div", "br", "blockquote", "pre"};
constexpr int NUM_BLOCK_TAGS = sizeof(BLOCK_TAGS) / sizeof(BLOCK_TAGS[0]);

const char* BOLD_TAGS[] = {"b", "strong"};
constexpr int NUM_BOLD_TAGS = sizeof(BOLD_TAGS) / sizeof(BOLD_TAGS[0]);

const char* ITALIC_TAGS[] = {"i", "em"};
constexpr int NUM_ITALIC_TAGS = sizeof(ITALIC_TAGS) / sizeof(ITALIC_TAGS[0]);

const char* UNDERLINE_TAGS[] = {"u", "ins"};
constexpr int NUM_UNDERLINE_TAGS = sizeof(UNDERLINE_TAGS) / sizeof(UNDERLINE_TAGS[0]);

const char* IMAGE_TAGS[] = {"img"};
constexpr int NUM_IMAGE_TAGS = sizeof(IMAGE_TAGS) / sizeof(IMAGE_TAGS[0]);

const char* SKIP_TAGS[] = {"head"};
constexpr int NUM_SKIP_TAGS = sizeof(SKIP_TAGS) / sizeof(SKIP_TAGS[0]);

bool isWhitespace(const char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; }

// given the start and end of a tag, check to see if it matches a known tag
bool matches(const char* tag_name, const char* possible_tags[], const int possible_tag_count) {
  for (int i = 0; i < possible_tag_count; i++) {
    if (strcmp(tag_name, possible_tags[i]) == 0) {
      return true;
    }
  }
  return false;
}

const char* getAttribute(const XML_Char** atts, const char* attrName) {
  if (!atts) return nullptr;
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(atts[i], attrName) == 0) return atts[i + 1];
  }
  return nullptr;
}

bool isInternalEpubLink(const char* href) {
  if (!href || href[0] == '\0') return false;
  if (strncmp(href, "http://", 7) == 0 || strncmp(href, "https://", 8) == 0) return false;
  if (strncmp(href, "mailto:", 7) == 0) return false;
  if (strncmp(href, "ftp://", 6) == 0) return false;
  if (strncmp(href, "tel:", 4) == 0) return false;
  if (strncmp(href, "javascript:", 11) == 0) return false;
  return true;
}

bool isHeaderOrBlock(const char* name) {
  return matches(name, HEADER_TAGS, NUM_HEADER_TAGS) || matches(name, BLOCK_TAGS, NUM_BLOCK_TAGS);
}

bool isTableStructuralTag(const char* name) {
  return strcmp(name, "table") == 0 || strcmp(name, "tr") == 0 || strcmp(name, "td") == 0 || strcmp(name, "th") == 0;
}

std::string buildTextBlockPreview(const std::shared_ptr<TextBlock>& line, const size_t maxLen = 120) {
  if (!line) {
    return {};
  }

  std::string preview;
  const auto& words = line->getWords();
  for (size_t i = 0; i < words.size(); ++i) {
    if (i > 0) {
      preview.push_back(' ');
    }
    preview += words[i];
    if (preview.size() >= maxLen) {
      preview.resize(maxLen);
      preview += "...";
      break;
    }
  }
  return preview;
}

// Calibre sometimes injects empty <p style="margin:0; border:0; height:0">...</p>
// spacers inside running prose. Keep them as paragraph boundaries, but ignore
// their inner text payload (usually NBSP) to avoid no-break-space glue artifacts.
bool isZeroHeightSpacerParagraph(const char* name, const std::string& styleAttr) {
  if (strcmp(name, "p") != 0 || styleAttr.empty()) {
    return false;
  }

  std::string normalized;
  normalized.reserve(styleAttr.size());
  for (const char ch : styleAttr) {
    if (!isWhitespace(ch)) {
      normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
  }

  const bool hasZeroHeight = normalized.find("height:0") != std::string::npos;
  const bool hasZeroMargin = normalized.find("margin:0") != std::string::npos;
  const bool hasZeroBorder = normalized.find("border:0") != std::string::npos;
  return hasZeroHeight && hasZeroMargin && hasZeroBorder;
}

// Update effective bold/italic/underline based on block style and inline style stack
void ChapterHtmlSlimParser::updateEffectiveInlineStyle() {
  // Start with block-level styles
  effectiveBold = currentCssStyle.hasFontWeight() && currentCssStyle.fontWeight == CssFontWeight::Bold;
  effectiveItalic = currentCssStyle.hasFontStyle() && currentCssStyle.fontStyle == CssFontStyle::Italic;
  effectiveUnderline =
      currentCssStyle.hasTextDecoration() && currentCssStyle.textDecoration == CssTextDecoration::Underline;

  // Apply inline style stack in order
  for (const auto& entry : inlineStyleStack) {
    if (entry.hasBold) {
      effectiveBold = entry.bold;
    }
    if (entry.hasItalic) {
      effectiveItalic = entry.italic;
    }
    if (entry.hasUnderline) {
      effectiveUnderline = entry.underline;
    }
  }
}

// flush the contents of partWordBuffer to currentTextBlock
void ChapterHtmlSlimParser::flushPartWordBuffer() {
  // Determine font style from depth-based tracking and CSS effective style
  const bool isBold = boldUntilDepth < depth || effectiveBold;
  const bool isItalic = italicUntilDepth < depth || effectiveItalic;
  const bool isUnderline = underlineUntilDepth < depth || effectiveUnderline;

  // Combine style flags using bitwise OR
  EpdFontFamily::Style fontStyle = EpdFontFamily::REGULAR;
  if (isBold) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::BOLD);
  }
  if (isItalic) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::ITALIC);
  }
  if (isUnderline) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::UNDERLINE);
  }

  // flush the buffer
  partWordBuffer[partWordBufferIndex] = '\0';
  currentTextBlock->addWord(partWordBuffer, fontStyle, false, nextWordContinues);
  partWordBufferIndex = 0;
  nextWordContinues = false;
}

// Emit the current page, keeping paragraphLutPerPage and completedPageCount in lockstep.
// Callers must ensure currentPage is non-null and carries content; the helper resets
// currentPage to a fresh Page and zeroes currentPageNextY so the caller can keep building.
void ChapterHtmlSlimParser::emitPage(uint32_t xhtmlByteOffset) {
  paragraphLutPerPage.push_back({xhtmlByteOffset, xpathParagraphIndex});
  completePageFn(std::move(currentPage));
  completedPageCount++;
  currentPage.reset(new Page());
  currentPageNextY = 0;
}

// start a new text block if needed
void ChapterHtmlSlimParser::startNewTextBlock(const BlockStyle& blockStyle) {
  nextWordContinues = false;  // New block = new paragraph, no continuation
  if (currentTextBlock) {
    // already have a text block running and it is empty - just reuse it
    if (currentTextBlock->isEmpty()) {
      // Merge with existing block style to accumulate CSS styling from parent block elements.
      // This handles cases like <div style="margin-bottom:2em"><h1>text</h1></div> where the
      // div's margin should be preserved, even though it has no direct text content.
      BlockStyle incoming = blockStyle;
      const bool brGapPending = currentTextBlock->getBlockStyle().fromBrElement;
      if (brGapPending) {
        // The empty block was created by a <br> section separator. Inject a full line of
        // blank space before the following paragraph so the scene/section break is visible.
        // This only fires when the <br> block stayed empty (i.e. no inline text was added).
        const int16_t lineHeight = static_cast<int16_t>(renderer.getLineHeight(fontId) * lineCompression + 0.5f);
        incoming.marginTop = static_cast<int16_t>(incoming.marginTop + lineHeight);
      }

      BlockStyle merged = currentTextBlock->getBlockStyle().getCombinedBlockStyle(incoming);
      // Preserve only whether the current empty block still represents <br> separators.
      // This lets consecutive <br> accumulate one line each without leaking the flag to real content blocks.
      merged.fromBrElement = blockStyle.fromBrElement;
      currentTextBlock->setBlockStyle(merged);

      if (!pendingAnchorId.empty()) {
        if (std::find(tocAnchors.begin(), tocAnchors.end(), pendingAnchorId) != tocAnchors.end()) {
          if (currentPage && !currentPage->elements.empty()) {
            emitPage(lastBodyChildByteOffset);
          }
        }
        anchorData.push_back({std::move(pendingAnchorId), static_cast<uint16_t>(completedPageCount)});
        pendingAnchorId.clear();
      }
      wordsExtractedInBlock = 0;
      return;
    }

    makePages();
  }
  // If the pending anchor is a TOC chapter boundary, force a page break after the previous
  // block is flushed so the chapter starts on a fresh page.
  if (!pendingAnchorId.empty() &&
      std::find(tocAnchors.begin(), tocAnchors.end(), pendingAnchorId) != tocAnchors.end()) {
    if (currentPage && !currentPage->elements.empty()) {
      emitPage(lastBodyChildByteOffset);
    }
  }
  // Record deferred anchor after previous block is flushed (and any TOC page break)
  if (!pendingAnchorId.empty()) {
    anchorData.push_back({std::move(pendingAnchorId), static_cast<uint16_t>(completedPageCount)});
    pendingAnchorId.clear();
  }
  currentTextBlock.reset(new ParsedText(extraParagraphSpacing, hyphenationEnabled, blockStyle));
  wordsExtractedInBlock = 0;
}

void XMLCALL ChapterHtmlSlimParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);

  // Middle of skip
  if (self->skipUntilDepth < self->depth) {
    self->depth += 1;
    return;
  }

  // Extract class, style, and id attributes
  std::string classAttr;
  std::string styleAttr;
  if (atts != nullptr) {
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "class") == 0) {
        classAttr = atts[i + 1];
      } else if (strcmp(atts[i], "style") == 0) {
        styleAttr = atts[i + 1];
      } else if (strcmp(atts[i], "id") == 0) {
        // Defer both anchor recording and TOC page breaks until startNewTextBlock,
        // after the previous block is flushed to pages via makePages().
        self->pendingAnchorId = atts[i + 1];
      }
    }
  }

  auto centeredBlockStyle = BlockStyle();
  centeredBlockStyle.textAlignDefined = true;
  centeredBlockStyle.alignment = CssTextAlign::Center;

  // Compute CSS style for this element early so display:none can short-circuit
  // before tag-specific branches emit any content or metadata.
  CssStyle cssStyle;
  if (self->cssParser) {
    {
      std::string cacheKey(name);
      cacheKey += '|';
      cacheKey += classAttr;
      auto it = self->cssStyleCache_.find(cacheKey);
      if (it != self->cssStyleCache_.end()) {
        cssStyle = it->second;
      } else {
        CssStyle resolved = self->cssParser->resolveStyle(name, classAttr);
        if (resolved.defined.anySet())
          cssStyle = self->cssStyleCache_.emplace(cacheKey, resolved).first->second;
        else
          cssStyle = resolved;  // transient fallback: skip cache so future calls can re-resolve
      }
    }
    if (!styleAttr.empty()) {
      auto it = self->inlineStyleCache_.find(styleAttr);
      if (it == self->inlineStyleCache_.end())
        it = self->inlineStyleCache_.emplace(styleAttr, CssParser::parseInlineStyle(styleAttr)).first;
      cssStyle.applyOver(it->second);
    }
  }

  // Skip elements with display:none before all fast paths (tables, links, etc.).
  if (cssStyle.hasDisplay() && cssStyle.display == CssDisplay::None) {
    self->skipUntilDepth = self->depth;
    self->depth += 1;
    return;
  }

  // Special handling for tables/cells: flatten into per-cell paragraphs with a prefixed header.
  if (strcmp(name, "table") == 0) {
    // skip nested tables
    if (self->tableDepth > 0) {
      self->tableDepth += 1;
      return;
    }

    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
    }
    self->tableDepth += 1;
    self->tableRowIndex = 0;
    self->tableColIndex = 0;
    self->depth += 1;
    return;
  }

  if (self->tableDepth == 1 && strcmp(name, "tr") == 0) {
    self->tableRowIndex += 1;
    self->tableColIndex = 0;
    self->depth += 1;
    return;
  }

  if (self->tableDepth == 1 && (strcmp(name, "td") == 0 || strcmp(name, "th") == 0)) {
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
    }
    self->tableColIndex += 1;

    auto tableCellBlockStyle = BlockStyle();
    tableCellBlockStyle.textAlignDefined = true;
    const auto align = (self->paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None))
                           ? CssTextAlign::Justify
                           : static_cast<CssTextAlign>(self->paragraphAlignment);
    tableCellBlockStyle.alignment = align;
    self->startNewTextBlock(tableCellBlockStyle);

    const std::string headerText =
        "Tab Row " + std::to_string(self->tableRowIndex) + ", Cell " + std::to_string(self->tableColIndex) + ":";
    StyleStackEntry headerStyle;
    headerStyle.depth = self->depth;
    headerStyle.hasBold = true;
    headerStyle.bold = false;
    headerStyle.hasItalic = true;
    headerStyle.italic = true;
    headerStyle.hasUnderline = true;
    headerStyle.underline = false;
    self->inlineStyleStack.push_back(headerStyle);
    self->updateEffectiveInlineStyle();
    self->characterData(userData, headerText.c_str(), static_cast<int>(headerText.length()));
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
    }
    self->nextWordContinues = false;
    self->inlineStyleStack.pop_back();
    self->updateEffectiveInlineStyle();

    self->depth += 1;
    return;
  }

  if (matches(name, IMAGE_TAGS, NUM_IMAGE_TAGS)) {
    std::string src;
    std::string alt;
    if (atts != nullptr) {
      for (int i = 0; atts[i]; i += 2) {
        if (strcmp(atts[i], "src") == 0) {
          src = atts[i + 1];
        } else if (strcmp(atts[i], "alt") == 0) {
          alt = atts[i + 1];
        }
      }

      // imageRendering: 0=display, 1=placeholder (alt text only), 2=suppress entirely
      if (self->imageRendering == 2) {
        // Suppressing an image should not leak accumulated wrapper block spacing
        // (e.g. figure/h1 margins) into the next text paragraph.
        if (self->currentTextBlock && self->currentTextBlock->isEmpty()) {
          BlockStyle resetStyle;
          resetStyle.textAlignDefined = true;
          const auto align = (self->paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None))
                                 ? CssTextAlign::Justify
                                 : static_cast<CssTextAlign>(self->paragraphAlignment);
          resetStyle.alignment = align;
          self->currentTextBlock->setBlockStyle(resetStyle);
          LOG_DBG("EHP", "Image suppressed: pending empty block style reset");
        }
        self->skipUntilDepth = self->depth;
        self->depth += 1;
        return;
      }

      // Skip image if CSS display:none
      if (self->cssParser) {
        std::string imgCacheKey("img|");
        imgCacheKey += classAttr;
        auto imgIt = self->cssStyleCache_.find(imgCacheKey);
        if (imgIt == self->cssStyleCache_.end())
          imgIt = self->cssStyleCache_.emplace(imgCacheKey, self->cssParser->resolveStyle("img", classAttr)).first;
        CssStyle imgDisplayStyle = imgIt->second;
        if (!styleAttr.empty()) {
          auto it = self->inlineStyleCache_.find(styleAttr);
          if (it == self->inlineStyleCache_.end())
            it = self->inlineStyleCache_.emplace(styleAttr, CssParser::parseInlineStyle(styleAttr)).first;
          imgDisplayStyle.applyOver(it->second);
        }
        if (imgDisplayStyle.hasDisplay() && imgDisplayStyle.display == CssDisplay::None) {
          // CSS-hidden images should behave like suppressed images for spacing.
          if (self->currentTextBlock && self->currentTextBlock->isEmpty()) {
            BlockStyle resetStyle;
            resetStyle.textAlignDefined = true;
            const auto align = (self->paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None))
                                   ? CssTextAlign::Justify
                                   : static_cast<CssTextAlign>(self->paragraphAlignment);
            resetStyle.alignment = align;
            self->currentTextBlock->setBlockStyle(resetStyle);
            LOG_DBG("EHP", "Image hidden via CSS display:none: pending empty block style reset");
          }
          self->skipUntilDepth = self->depth;
          self->depth += 1;
          return;
        }
      }

      if (!src.empty() && self->imageRendering != 1) {
        LOG_DBG("EHP", "Found image: src=%s", src.c_str());

        {
          // Resolve the image path relative to the HTML file
          std::string resolvedPath = FsHelpers::normalisePath(self->contentBase + src);

          if (ImageDecoderFactory::isFormatSupported(resolvedPath)) {
            // Create a unique filename for the cached image
            std::string ext;
            size_t extPos = resolvedPath.rfind('.');
            if (extPos != std::string::npos) {
              ext = resolvedPath.substr(extPos);
            }
            std::string cachedImagePath = self->imageBasePath + std::to_string(self->imageCounter++) + ext;

            // Extract image to cache file
            FsFile cachedImageFile;
            bool extractSuccess = false;
            if (Storage.openFileForWrite("EHP", cachedImagePath, cachedImageFile)) {
              extractSuccess = self->epub->readItemContentsToStream(resolvedPath, cachedImageFile, 4096);
              cachedImageFile.flush();
              cachedImageFile.close();
              delay(50);  // Give SD card time to sync
            }

            if (extractSuccess) {
              // Get image dimensions
              ImageDimensions dims = {0, 0};
              ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(cachedImagePath);
              if (decoder && decoder->getDimensions(cachedImagePath, dims)) {
                LOG_DBG("EHP", "Image dimensions: %dx%d", dims.width, dims.height);

                int displayWidth = 0;
                int displayHeight = 0;
                const float emSize = static_cast<float>(self->renderer.getFontAscenderSize(self->fontId));
                std::string imgCacheKey("img|");
                imgCacheKey += classAttr;
                auto imgStyleIt = self->cssParser ? self->cssStyleCache_.find(imgCacheKey) : self->cssStyleCache_.end();
                if (self->cssParser && imgStyleIt == self->cssStyleCache_.end())
                  imgStyleIt =
                      self->cssStyleCache_.emplace(imgCacheKey, self->cssParser->resolveStyle("img", classAttr)).first;
                CssStyle imgStyle = self->cssParser ? imgStyleIt->second : CssStyle{};
                // Merge inline style (e.g. style="height: 2em") so it overrides stylesheet rules
                if (!styleAttr.empty()) {
                  auto it = self->inlineStyleCache_.find(styleAttr);
                  if (it == self->inlineStyleCache_.end())
                    it = self->inlineStyleCache_.emplace(styleAttr, CssParser::parseInlineStyle(styleAttr)).first;
                  imgStyle.applyOver(it->second);
                }
                const bool hasCssHeight = imgStyle.hasImageHeight();
                const bool hasCssWidth = imgStyle.hasImageWidth();
                int containerWidth = self->viewportWidth;
                if (self->currentTextBlock) {
                  const int inset = self->currentTextBlock->getBlockStyle().totalHorizontalInset();
                  if (inset > 0 && inset < self->viewportWidth) {
                    containerWidth = self->viewportWidth - inset;
                  }
                }

                if (hasCssHeight && hasCssWidth && dims.width > 0 && dims.height > 0) {
                  // Both CSS height and width set: resolve both, then clamp to
                  // current container preserving requested ratio.
                  displayHeight = static_cast<int>(
                      imgStyle.imageHeight.toPixels(emSize, static_cast<float>(self->viewportHeight)) + 0.5f);
                  displayWidth =
                      static_cast<int>(imgStyle.imageWidth.toPixels(emSize, static_cast<float>(containerWidth)) + 0.5f);
                  if (displayHeight < 1) displayHeight = 1;
                  if (displayWidth < 1) displayWidth = 1;
                  if (displayWidth > containerWidth || displayHeight > self->viewportHeight) {
                    float scaleX =
                        (displayWidth > containerWidth) ? static_cast<float>(containerWidth) / displayWidth : 1.0f;
                    float scaleY = (displayHeight > self->viewportHeight)
                                       ? static_cast<float>(self->viewportHeight) / displayHeight
                                       : 1.0f;
                    float scale = (scaleX < scaleY) ? scaleX : scaleY;
                    displayWidth = static_cast<int>(displayWidth * scale + 0.5f);
                    displayHeight = static_cast<int>(displayHeight * scale + 0.5f);
                    if (displayWidth < 1) displayWidth = 1;
                    if (displayHeight < 1) displayHeight = 1;
                  }
                  LOG_DBG("EHP", "Display size from CSS height+width: %dx%d", displayWidth, displayHeight);
                } else if (hasCssHeight && !hasCssWidth && dims.width > 0 && dims.height > 0) {
                  // Use CSS height (resolve % against viewport height) and derive width from aspect ratio
                  displayHeight = static_cast<int>(
                      imgStyle.imageHeight.toPixels(emSize, static_cast<float>(self->viewportHeight)) + 0.5f);
                  if (displayHeight < 1) displayHeight = 1;
                  displayWidth =
                      static_cast<int>(displayHeight * (static_cast<float>(dims.width) / dims.height) + 0.5f);
                  if (displayHeight > self->viewportHeight) {
                    displayHeight = self->viewportHeight;
                    // Rescale width to preserve aspect ratio when height is clamped
                    displayWidth =
                        static_cast<int>(displayHeight * (static_cast<float>(dims.width) / dims.height) + 0.5f);
                    if (displayWidth < 1) displayWidth = 1;
                  }
                  if (displayWidth > containerWidth) {
                    displayWidth = containerWidth;
                    // Rescale height to preserve aspect ratio when width is clamped
                    displayHeight =
                        static_cast<int>(displayWidth * (static_cast<float>(dims.height) / dims.width) + 0.5f);
                    if (displayHeight < 1) displayHeight = 1;
                  }
                  if (displayWidth < 1) displayWidth = 1;
                  LOG_DBG("EHP", "Display size from CSS height: %dx%d", displayWidth, displayHeight);
                } else if (hasCssWidth && !hasCssHeight && dims.width > 0 && dims.height > 0) {
                  // Use CSS width (resolve % against container width) and derive
                  // height from aspect ratio.
                  displayWidth =
                      static_cast<int>(imgStyle.imageWidth.toPixels(emSize, static_cast<float>(containerWidth)) + 0.5f);
                  if (displayWidth > containerWidth) displayWidth = containerWidth;
                  if (displayWidth < 1) displayWidth = 1;
                  displayHeight =
                      static_cast<int>(displayWidth * (static_cast<float>(dims.height) / dims.width) + 0.5f);
                  if (displayHeight > self->viewportHeight) {
                    displayHeight = self->viewportHeight;
                    // Rescale width to preserve aspect ratio when height is clamped
                    displayWidth =
                        static_cast<int>(displayHeight * (static_cast<float>(dims.width) / dims.height) + 0.5f);
                    if (displayWidth < 1) displayWidth = 1;
                  }
                  if (displayHeight < 1) displayHeight = 1;
                  LOG_DBG("EHP", "Display size from CSS width: %dx%d", displayWidth, displayHeight);
                } else {
                  // Scale to fit current container while maintaining aspect ratio.
                  int maxWidth = containerWidth;
                  int maxHeight = self->viewportHeight;
                  float scaleX = (dims.width > maxWidth) ? (float)maxWidth / dims.width : 1.0f;
                  float scaleY = (dims.height > maxHeight) ? (float)maxHeight / dims.height : 1.0f;
                  float scale = (scaleX < scaleY) ? scaleX : scaleY;
                  if (scale > 1.0f) scale = 1.0f;

                  displayWidth = (int)(dims.width * scale);
                  displayHeight = (int)(dims.height * scale);
                  LOG_DBG("EHP", "Display size: %dx%d (scale %.2f)", displayWidth, displayHeight, scale);
                }

                // Flush any pending text block so it appears before the image
                if (self->partWordBufferIndex > 0) {
                  self->flushPartWordBuffer();
                }
                if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
                  const BlockStyle parentBlockStyle = self->currentTextBlock->getBlockStyle();
                  self->startNewTextBlock(parentBlockStyle);
                }

                // If the current text block is still empty, it may carry accumulated parent
                // block spacing (e.g. div/figure/h1 wrappers). Apply that spacing around the
                // image itself so it doesn't leak into the next text paragraph.
                BlockStyle pendingImageBlockStyle;
                if (self->currentTextBlock && self->currentTextBlock->isEmpty()) {
                  pendingImageBlockStyle = self->currentTextBlock->getBlockStyle();
                }

                const int imageSpacingTop = std::max(0, static_cast<int>(pendingImageBlockStyle.marginTop)) +
                                            std::max(0, static_cast<int>(pendingImageBlockStyle.paddingTop));
                const int imageSpacingBottom = std::max(0, static_cast<int>(pendingImageBlockStyle.marginBottom)) +
                                               std::max(0, static_cast<int>(pendingImageBlockStyle.paddingBottom));
                const int totalImageHeightWithSpacing = imageSpacingTop + displayHeight + imageSpacingBottom;

                LOG_DBG("EHP",
                        "Image layout prep: src=%s dims=%dx%d display=%dx%d y=%d spacing(top=%d,bottom=%d,total=%d)",
                        src.c_str(), dims.width, dims.height, displayWidth, displayHeight, self->currentPageNextY,
                        imageSpacingTop, imageSpacingBottom, totalImageHeightWithSpacing);

                // Create page for image - only break if image won't fit remaining space
                if (self->currentPage && !self->currentPage->elements.empty() &&
                    (self->currentPageNextY + totalImageHeightWithSpacing > self->viewportHeight)) {
                  LOG_DBG("EHP", "Image page break: currentY=%d needed=%d viewportH=%d", self->currentPageNextY,
                          totalImageHeightWithSpacing, self->viewportHeight);
                  self->emitPage(self->lastBodyChildByteOffset);
                  if (!self->currentPage) {
                    LOG_ERR("EHP", "Failed to create new page");
                    return;
                  }
                } else if (!self->currentPage) {
                  self->currentPage.reset(new Page());
                  if (!self->currentPage) {
                    LOG_ERR("EHP", "Failed to create initial page");
                    return;
                  }
                  self->currentPageNextY = 0;
                }

                self->currentPageNextY += imageSpacingTop;

                // Create ImageBlock and add to page
                auto imageBlock = std::make_shared<ImageBlock>(cachedImagePath, displayWidth, displayHeight);
                if (!imageBlock) {
                  LOG_ERR("EHP", "Failed to create ImageBlock");
                  return;
                }
                int xPos = (self->viewportWidth - displayWidth) / 2;
                auto pageImage = std::make_shared<PageImage>(imageBlock, xPos, self->currentPageNextY);
                if (!pageImage) {
                  LOG_ERR("EHP", "Failed to create PageImage");
                  return;
                }
                self->currentPage->elements.push_back(pageImage);
                self->currentPageNextY += displayHeight;
                self->currentPageNextY += imageSpacingBottom;

                LOG_DBG("EHP", "Image placed: x=%d y=%d w=%d h=%d nextY=%d", xPos, pageImage->yPos, displayWidth,
                        displayHeight, self->currentPageNextY);

                // Reset empty pending block style after consuming spacing around the image.
                // This prevents figure/header wrapper margins from being applied again to the
                // next paragraph block.
                if (self->currentTextBlock && self->currentTextBlock->isEmpty()) {
                  BlockStyle resetStyle;
                  resetStyle.textAlignDefined = true;
                  const auto align = (self->paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None))
                                         ? CssTextAlign::Justify
                                         : static_cast<CssTextAlign>(self->paragraphAlignment);
                  resetStyle.alignment = align;
                  self->currentTextBlock->setBlockStyle(resetStyle);
                  LOG_DBG("EHP", "Image spacing consumed; pending empty block style reset for following text");
                }

                self->depth += 1;
                return;
              } else {
                LOG_ERR("EHP", "Failed to get image dimensions");
                Storage.remove(cachedImagePath.c_str());
              }
            } else {
              LOG_ERR("EHP", "Failed to extract image");
            }
          }  // isFormatSupported
        }
      }

      // Fallback to alt text if image processing fails
      if (!alt.empty()) {
        alt = "[Image: " + alt + "]";
        self->startNewTextBlock(centeredBlockStyle);
        self->italicUntilDepth = std::min(self->italicUntilDepth, self->depth);
        self->depth += 1;
        self->characterData(userData, alt.c_str(), alt.length());
        // Skip any child content (skip until parent as we pre-advanced depth above)
        self->skipUntilDepth = self->depth - 1;
        return;
      }

      // No alt text, skip
      self->skipUntilDepth = self->depth;
      self->depth += 1;
      return;
    }
  }

  // Track body element depth for paragraph index counting
  if (strcmp(name, "body") == 0 && self->xpathBodyDepth < 0) {
    self->xpathBodyDepth = self->depth;
  }

  // Count <p> sibling indices at body-child level. Must happen BEFORE the display:none
  // check so that hidden <p> elements are still counted, matching ChapterXPathIndexer's
  // counting (pure XML, no CSS). This ensures paragraph indices in the section cache LUT
  // align with KOReader's crengine XPath indices.
  // At the same time, record the byte offset of every direct-body-child element start:
  // the forward mapper's partial-parse heuristic requires the seek hint to land on a
  // body-child boundary, otherwise partialBaseDepth can misidentify wrapped paragraphs.
  if (self->xpathBodyDepth >= 0 && self->depth == self->xpathBodyDepth + 1) {
    if (self->activeParser) {
      self->lastBodyChildByteOffset = static_cast<uint32_t>(XML_GetCurrentByteIndex(self->activeParser));
    }
    if (strcmp(name, "p") == 0) {
      self->xpathParagraphIndex++;
    }
  }

  if (matches(name, SKIP_TAGS, NUM_SKIP_TAGS)) {
    // start skip
    self->skipUntilDepth = self->depth;
    self->depth += 1;
    return;
  }

  // Skip blocks with role="doc-pagebreak" and epub:type="pagebreak"
  if (atts != nullptr) {
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "role") == 0 && strcmp(atts[i + 1], "doc-pagebreak") == 0 ||
          strcmp(atts[i], "epub:type") == 0 && strcmp(atts[i + 1], "pagebreak") == 0) {
        self->skipUntilDepth = self->depth;
        self->depth += 1;
        return;
      }
    }
  }

  // Detect internal <a href="..."> links (footnotes, cross-references)
  // Note: <aside epub:type="footnote"> elements are rendered as normal content
  // without special handling. Links pointing to them are collected as footnotes.
  if (strcmp(name, "a") == 0) {
    const char* href = getAttribute(atts, "href");

    bool isInternalLink = isInternalEpubLink(href);

    // Special case: javascript:void(0) links with data attributes
    // Example: <a href="javascript:void(0)"
    // data-xyz="{&quot;name&quot;:&quot;OPS/ch2.xhtml&quot;,&quot;frag&quot;:&quot;id46&quot;}">
    if (href && strncmp(href, "javascript:", 11) == 0) {
      isInternalLink = false;
      // TODO: Parse data-* attributes to extract actual href
    }

    if (isInternalLink) {
      // Flush buffer before style change
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
        self->nextWordContinues = true;
      }
      self->insideFootnoteLink = true;
      self->footnoteLinkDepth = self->depth;
      strncpy(self->currentFootnote.href, href, sizeof(self->currentFootnote.href) - 1);
      self->currentFootnote.href[sizeof(self->currentFootnote.href) - 1] = '\0';
      self->currentFootnote.number[0] = '\0';
      self->currentFootnoteLinkTextLen = 0;

      // Apply underline style to visually indicate the link
      self->underlineUntilDepth = std::min(self->underlineUntilDepth, self->depth);
      StyleStackEntry entry;
      entry.depth = self->depth;
      entry.hasUnderline = true;
      entry.underline = true;
      self->inlineStyleStack.push_back(entry);
      self->updateEffectiveInlineStyle();

      // Skip CSS resolution — we already handled styling for this <a> tag
      self->depth += 1;
      return;
    }
  }

  if (strcmp(name, "ul") == 0 || strcmp(name, "ol") == 0) {
    self->listStack.push_back({self->depth, name[0] == 'o', 0});
  }

  const float emSize = static_cast<float>(self->renderer.getFontAscenderSize(self->fontId));
  const auto userAlignmentBlockStyle = BlockStyle::fromCssStyle(
      cssStyle, emSize, static_cast<CssTextAlign>(self->paragraphAlignment), self->viewportWidth);

  // Block/header boundaries must flush any buffered trailing word first.
  // Otherwise tags like ..."item?"<p ...> can carry the final word into the next paragraph.
  if (self->partWordBufferIndex > 0 && ((matches(name, HEADER_TAGS, NUM_HEADER_TAGS)) ||
                                        (matches(name, BLOCK_TAGS, NUM_BLOCK_TAGS) && strcmp(name, "br") != 0))) {
    self->flushPartWordBuffer();
  }

  if (matches(name, HEADER_TAGS, NUM_HEADER_TAGS)) {
    self->currentCssStyle = cssStyle;
    auto headerBlockStyle = BlockStyle::fromCssStyle(cssStyle, emSize, CssTextAlign::Center, self->viewportWidth);
    headerBlockStyle.textAlignDefined = true;
    if (self->embeddedStyle && cssStyle.hasTextAlign()) {
      headerBlockStyle.alignment = cssStyle.textAlign;
    }
    self->startNewTextBlock(headerBlockStyle);
    self->boldUntilDepth = std::min(self->boldUntilDepth, self->depth);
    self->updateEffectiveInlineStyle();
  } else if (matches(name, BLOCK_TAGS, NUM_BLOCK_TAGS)) {
    if (isZeroHeightSpacerParagraph(name, styleAttr)) {
      // Preserve paragraph break semantics for this <p>, but skip its inner text payload.
      self->currentCssStyle = cssStyle;
      auto blockStyle = userAlignmentBlockStyle;
      if (self->embeddedStyle && cssStyle.hasTextAlign()) {
        blockStyle.alignment = cssStyle.textAlign;
        blockStyle.textAlignDefined = true;
      }
      self->startNewTextBlock(blockStyle);
      self->updateEffectiveInlineStyle();

      self->skipTextUntilDepth = self->depth;
      self->depth += 1;
      return;
    }

    if (strcmp(name, "br") == 0) {
      if (self->partWordBufferIndex > 0) {
        // flush word preceding <br/> to currentTextBlock before calling startNewTextBlock
        self->flushPartWordBuffer();
      }
      // Tag the new block so startNewTextBlock can inject a full line-height gap if
      // the block remains empty (i.e. <br> is a section separator between paragraphs).
      // If the block gets text added before the next block opens it becomes non-empty,
      // goes through makePages() normally, and the flag has no effect (inline <br> case).
      // Build a neutral <br> style that keeps inline alignment/indent context but avoids
      // carrying cumulative margins from previous empty blocks (which can force spurious page breaks).
      const BlockStyle& currentStyle = self->currentTextBlock->getBlockStyle();
      BlockStyle brStyle;
      brStyle.alignment = currentStyle.alignment;
      brStyle.textAlignDefined = currentStyle.textAlignDefined;
      brStyle.textIndent = currentStyle.textIndent;
      brStyle.textIndentDefined = currentStyle.textIndentDefined;
      brStyle.fromBrElement = true;
      self->startNewTextBlock(brStyle);
    } else {
      self->currentCssStyle = cssStyle;
      auto blockStyle = userAlignmentBlockStyle;
      if (self->embeddedStyle && cssStyle.hasTextAlign()) {
        blockStyle.alignment = cssStyle.textAlign;
        blockStyle.textAlignDefined = true;
      }
      self->startNewTextBlock(blockStyle);
      self->updateEffectiveInlineStyle();

      if (strcmp(name, "li") == 0) {
        char marker[12];
        if (!self->listStack.empty() && self->listStack.back().isOrdered) {
          self->listStack.back().counter += 1;
          snprintf(marker, sizeof(marker), "%d.", self->listStack.back().counter);
        } else {
          strcpy(marker, "\xe2\x80\xa2");
        }
        self->currentTextBlock->addWord(marker, EpdFontFamily::REGULAR);
      } else if (strcmp(name, "pre") == 0) {
        // Record depth so characterData can treat \n as a hard line break inside <pre>.
        // depth has not been incremented yet here; it will be after startElement returns.
        self->preUntilDepth = std::min(self->preUntilDepth, self->depth);
      }
    }
  } else if (matches(name, UNDERLINE_TAGS, NUM_UNDERLINE_TAGS)) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->underlineUntilDepth = std::min(self->underlineUntilDepth, self->depth);
    // Push inline style entry for underline tag
    StyleStackEntry entry;
    entry.depth = self->depth;  // Track depth for matching pop
    entry.hasUnderline = true;
    entry.underline = true;
    if (cssStyle.hasFontWeight()) {
      entry.hasBold = true;
      entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
    }
    if (cssStyle.hasFontStyle()) {
      entry.hasItalic = true;
      entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
    }
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  } else if (matches(name, BOLD_TAGS, NUM_BOLD_TAGS)) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->boldUntilDepth = std::min(self->boldUntilDepth, self->depth);
    // Push inline style entry for bold tag
    StyleStackEntry entry;
    entry.depth = self->depth;  // Track depth for matching pop
    entry.hasBold = true;
    entry.bold = true;
    if (cssStyle.hasFontStyle()) {
      entry.hasItalic = true;
      entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
    }
    if (cssStyle.hasTextDecoration()) {
      entry.hasUnderline = true;
      entry.underline = cssStyle.textDecoration == CssTextDecoration::Underline;
    }
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  } else if (matches(name, ITALIC_TAGS, NUM_ITALIC_TAGS)) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->italicUntilDepth = std::min(self->italicUntilDepth, self->depth);
    // Push inline style entry for italic tag
    StyleStackEntry entry;
    entry.depth = self->depth;  // Track depth for matching pop
    entry.hasItalic = true;
    entry.italic = true;
    if (cssStyle.hasFontWeight()) {
      entry.hasBold = true;
      entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
    }
    if (cssStyle.hasTextDecoration()) {
      entry.hasUnderline = true;
      entry.underline = cssStyle.textDecoration == CssTextDecoration::Underline;
    }
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  } else if (strcmp(name, "span") == 0 || !isHeaderOrBlock(name)) {
    // Handle span and other inline elements for CSS styling
    if (cssStyle.hasFontWeight() || cssStyle.hasFontStyle() || cssStyle.hasTextDecoration()) {
      // Flush buffer before style change so preceding text gets current style
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
        self->nextWordContinues = true;
      }
      StyleStackEntry entry;
      entry.depth = self->depth;  // Track depth for matching pop
      if (cssStyle.hasFontWeight()) {
        entry.hasBold = true;
        entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
      }
      if (cssStyle.hasFontStyle()) {
        entry.hasItalic = true;
        entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
      }
      if (cssStyle.hasTextDecoration()) {
        entry.hasUnderline = true;
        entry.underline = cssStyle.textDecoration == CssTextDecoration::Underline;
      }
      self->inlineStyleStack.push_back(entry);
      self->updateEffectiveInlineStyle();
    }
  }

  // Unprocessed tag, just increasing depth and continue forward
  self->depth += 1;
}

void XMLCALL ChapterHtmlSlimParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);

  // Skip content of nested table
  if (self->tableDepth > 1) {
    return;
  }

  // Middle of skip
  if (self->skipUntilDepth < self->depth) {
    return;
  }

  // Ignore character data inside synthetic zero-height spacer <p> tags.
  if (self->skipTextUntilDepth < self->depth) {
    return;
  }

  // Collect footnote link display text (for the number label)
  // Remove leading/trailing whitespace and square brackets from the
  // footnote link text to normalize noterefs like "[1]" → "1"
  if (self->insideFootnoteLink) {
    int start = 0;
    int end = len - 1;

    // Example input and output texts:
    // "     [  12  ]   " => "12"
    // "   turn to 256  " => "turn to 256"

    // Ignore leading whitespaces and left square brackets
    while (start < len && (isWhitespace(s[start]) || (s[start] == '['))) {
      ++start;
    }

    // Ignore trailing whitespaces and right square brackets
    while (end >= start && (isWhitespace(s[end]) || (s[end] == ']'))) {
      --end;
    }

    // Extract footnote link text
    for (int i = start; (self->currentFootnoteLinkTextLen < sizeof(self->currentFootnote.number) - 1) && (i <= end);
         ++i) {
      self->currentFootnote.number[self->currentFootnoteLinkTextLen++] = s[i];
    }
    self->currentFootnote.number[self->currentFootnoteLinkTextLen] = '\0';
  }

  for (int i = 0; i < len; i++) {
    const unsigned char c = static_cast<unsigned char>(s[i]);

    // Fast path for plain ASCII word characters (> 0x20 and < 0x80).
    // This covers the vast majority of characters in Latin-script text.
    // All multi-byte UTF-8 sequences start with a byte >= 0x80, so this
    // path is safe to take without any further multi-byte checks.
    if (c > 0x20 && c < 0x80) {
      if (self->partWordBufferIndex >= MAX_WORD_SIZE) {
        // Buffer is full — flush before appending. Pure ASCII means no
        // partial multi-byte sequence can be at the boundary.
        self->flushPartWordBuffer();
      }
      self->partWordBuffer[self->partWordBufferIndex++] = s[i];
      continue;
    }

    if (isWhitespace(s[i])) {
      // Inside <pre>: treat \n as a hard line break.
      if (s[i] == '\n' && self->preUntilDepth < self->depth) {
        if (self->partWordBufferIndex > 0) {
          self->flushPartWordBuffer();
        }
        // Blank line: the current block is empty, but we still need to emit a visible
        // empty line.  Add a single space so the block is non-empty and makePages()
        // will produce a line of the correct height instead of reusing the empty block.
        if (self->currentTextBlock->isEmpty()) {
          self->currentTextBlock->addWord(" ", EpdFontFamily::REGULAR);
        }
        self->startNewTextBlock(self->currentTextBlock->getBlockStyle());
        self->nextWordContinues = false;
        continue;
      }
      // Currently looking at whitespace, if there's anything in the partWordBuffer, flush it
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }
      // Whitespace is a real word boundary — reset continuation state
      self->nextWordContinues = false;
      // Skip the whitespace char
      continue;
    }

    // Detect U+00A0 (non-breaking space, UTF-8: 0xC2 0xA0) or
    //        U+202F (narrow no-break space, UTF-8: 0xE2 0x80 0xAF).
    //
    // Both are rendered as a visible space but must never allow a line break around them.
    // We split the no-break space into its own word token and link the surrounding words
    // with continuation flags so the layout engine treats them as an indivisible group.
    //
    // Example: "200&#xA0;Quadratkilometer" or "200&#x202F;Quadratkilometer"
    //   Input bytes:  "200\xC2\xA0Quadratkilometer"  (or 0xE2 0x80 0xAF for U+202F)
    //   Tokens produced:
    //     [0] "200"               continues=false
    //     [1] " "                 continues=true   (attaches to "200", no gap)
    //     [2] "Quadratkilometer"  continues=true   (attaches to " ", no gap)
    //
    //   The continuation flags prevent the line-breaker from inserting a line break
    //   between "200" and "Quadratkilometer". However, "Quadratkilometer" is now a
    //   standalone word for hyphenation purposes, so Liang patterns can produce
    //   "200 Quadrat-" / "kilometer" instead of the unusable "200" / "Quadratkilometer".
    if (static_cast<uint8_t>(s[i]) == 0xC2 && i + 1 < len && static_cast<uint8_t>(s[i + 1]) == 0xA0) {
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }

      self->partWordBuffer[0] = ' ';
      self->partWordBuffer[1] = '\0';
      self->partWordBufferIndex = 1;
      self->nextWordContinues = true;  // Attach space to previous word (no break).
      self->flushPartWordBuffer();

      self->nextWordContinues = true;  // Next real word attaches to this space (no break).

      i++;  // Skip the second byte (0xA0)
      continue;
    }

    // U+202F (narrow no-break space) — identical logic to U+00A0 above.
    if (static_cast<uint8_t>(s[i]) == 0xE2 && i + 2 < len && static_cast<uint8_t>(s[i + 1]) == 0x80 &&
        static_cast<uint8_t>(s[i + 2]) == 0xAF) {
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }

      self->partWordBuffer[0] = ' ';
      self->partWordBuffer[1] = '\0';
      self->partWordBufferIndex = 1;
      self->nextWordContinues = true;
      self->flushPartWordBuffer();

      self->nextWordContinues = true;

      i += 2;  // Skip the remaining two bytes (0x80 0xAF)
      continue;
    }

    // Skip Zero Width No-Break Space / BOM (U+FEFF) = 0xEF 0xBB 0xBF
    const XML_Char FEFF_BYTE_1 = static_cast<XML_Char>(0xEF);
    const XML_Char FEFF_BYTE_2 = static_cast<XML_Char>(0xBB);
    const XML_Char FEFF_BYTE_3 = static_cast<XML_Char>(0xBF);

    if (s[i] == FEFF_BYTE_1) {
      // Check if the next two bytes complete the 3-byte sequence
      if ((i + 2 < len) && (s[i + 1] == FEFF_BYTE_2) && (s[i + 2] == FEFF_BYTE_3)) {
        // Sequence 0xEF 0xBB 0xBF found!
        i += 2;    // Skip the next two bytes
        continue;  // Move to the next iteration
      }
    }

    // If we're about to run out of space, then cut the word off and start a new one.
    // For CJK text (no spaces), this is the primary word-breaking mechanism.
    // We must avoid splitting multi-byte UTF-8 sequences across word boundaries,
    // otherwise the trailing bytes become orphaned continuation bytes that the
    // decoder can't interpret.
    if (self->partWordBufferIndex >= MAX_WORD_SIZE) {
      int safeLen = utf8SafeTruncateBuffer(self->partWordBuffer, self->partWordBufferIndex);

      if (safeLen < self->partWordBufferIndex && safeLen > 0) {
        // Incomplete UTF-8 sequence at the end — save it before flushing
        int overflow = self->partWordBufferIndex - safeLen;
        char saved[4];
        for (int j = 0; j < overflow; j++) {
          saved[j] = self->partWordBuffer[safeLen + j];
        }
        self->partWordBufferIndex = safeLen;
        self->flushPartWordBuffer();
        for (int j = 0; j < overflow; j++) {
          self->partWordBuffer[j] = saved[j];
        }
        self->partWordBufferIndex = overflow;
      } else {
        self->flushPartWordBuffer();
      }
    }

    self->partWordBuffer[self->partWordBufferIndex++] = s[i];
  }

  // If we have > 750 words buffered up, perform the layout and consume out all but the last line
  // There should be enough here to build out 1-2 full pages and doing this will free up a lot of
  // memory.
  // Spotted when reading Intermezzo, there are some really long text blocks in there.
  if (self->currentTextBlock->size() > 750) {
    LOG_DBG("EHP", "Text block too long, splitting into multiple pages");
    const int horizontalInset = self->currentTextBlock->getBlockStyle().totalHorizontalInset();
    const uint16_t effectiveWidth = (horizontalInset < self->viewportWidth)
                                        ? static_cast<uint16_t>(self->viewportWidth - horizontalInset)
                                        : self->viewportWidth;
    self->currentTextBlock->layoutAndExtractLines(
        self->renderer, self->fontId, effectiveWidth,
        [self](const std::shared_ptr<TextBlock>& textBlock, const bool lineEndsWithHyphenatedWord,
               const bool suppressHyphenationRetry) {
          return self->addLineToPage(textBlock, lineEndsWithHyphenatedWord, suppressHyphenationRetry);
        },
        false);
  }
}

void XMLCALL ChapterHtmlSlimParser::defaultHandlerExpand(void* userData, const XML_Char* s, const int len) {
  // Check if this looks like an entity reference (&...;)
  if (len >= 3 && s[0] == '&' && s[len - 1] == ';') {
    const char* utf8Value = lookupHtmlEntity(s, static_cast<size_t>(len));
    if (utf8Value != nullptr) {
      // Known entity: expand to its UTF-8 value
      characterData(userData, utf8Value, strlen(utf8Value));
      return;
    }
    // Unknown entity: preserve original &...; sequence
    characterData(userData, s, len);
    return;
  }
  // Not an entity we recognize - skip it
}

void XMLCALL ChapterHtmlSlimParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);

  // Check if any style state will change after we decrement depth
  // If so, we MUST flush the partWordBuffer with the CURRENT style first
  // Note: depth hasn't been decremented yet, so we check against (depth - 1)
  const bool willPopStyleStack =
      !self->inlineStyleStack.empty() && self->inlineStyleStack.back().depth == self->depth - 1;
  const bool willClearBold = self->boldUntilDepth == self->depth - 1;
  const bool willClearItalic = self->italicUntilDepth == self->depth - 1;
  const bool willClearUnderline = self->underlineUntilDepth == self->depth - 1;

  const bool styleWillChange = willPopStyleStack || willClearBold || willClearItalic || willClearUnderline;
  const bool headerOrBlockTag = isHeaderOrBlock(name);
  const bool tableStructuralTag = isTableStructuralTag(name);

  if (self->tableDepth > 1 && strcmp(name, "table") == 0) {
    // get rid of all text inside the nested table
    self->partWordBufferIndex = 0;
    self->tableDepth -= 1;
    LOG_DBG("EHP", "nested table detected, get rid of its content");
    return;
  }

  // Flush buffer with current style BEFORE any style changes
  if (self->partWordBufferIndex > 0) {
    // Flush if style will change OR if we're closing a block/structural element
    const bool isInlineTag =
        !headerOrBlockTag && !tableStructuralTag && !matches(name, IMAGE_TAGS, NUM_IMAGE_TAGS) && self->depth != 1;
    const bool shouldFlush = styleWillChange || headerOrBlockTag || matches(name, BOLD_TAGS, NUM_BOLD_TAGS) ||
                             matches(name, ITALIC_TAGS, NUM_ITALIC_TAGS) ||
                             matches(name, UNDERLINE_TAGS, NUM_UNDERLINE_TAGS) || tableStructuralTag ||
                             matches(name, IMAGE_TAGS, NUM_IMAGE_TAGS) || self->depth == 1;

    if (shouldFlush) {
      self->flushPartWordBuffer();
      // If closing an inline element, the next word fragment continues the same visual word
      if (isInlineTag) {
        self->nextWordContinues = true;
      }
    }
  }

  self->depth -= 1;

  // Pop list entries whose ul/ol is now out of scope
  while (!self->listStack.empty() && self->listStack.back().depth >= self->depth) {
    self->listStack.pop_back();
  }

  // Closing a footnote link — create entry from collected text and href
  if (self->insideFootnoteLink && self->depth == self->footnoteLinkDepth) {
    if (self->currentFootnote.number[0] != '\0' && self->currentFootnote.href[0] != '\0') {
      FootnoteEntry entry;
      strncpy(entry.number, self->currentFootnote.number, sizeof(entry.number) - 1);
      entry.number[sizeof(entry.number) - 1] = '\0';
      strncpy(entry.href, self->currentFootnote.href, sizeof(entry.href) - 1);
      entry.href[sizeof(entry.href) - 1] = '\0';
      int wordIndex =
          self->wordsExtractedInBlock + (self->currentTextBlock ? static_cast<int>(self->currentTextBlock->size()) : 0);
      self->pendingFootnotes.push_back({wordIndex, entry});
    }
    self->insideFootnoteLink = false;
  }

  // Leaving skip
  if (self->skipUntilDepth == self->depth) {
    self->skipUntilDepth = INT_MAX;
  }

  // Leaving zero-height spacer paragraph text-skip scope
  if (self->skipTextUntilDepth == self->depth) {
    self->skipTextUntilDepth = INT_MAX;
  }

  if (self->tableDepth == 1 && (strcmp(name, "td") == 0 || strcmp(name, "th") == 0)) {
    self->nextWordContinues = false;
  }

  if (self->tableDepth == 1 && (strcmp(name, "tr") == 0)) {
    self->nextWordContinues = false;
  }

  if (self->tableDepth == 1 && strcmp(name, "table") == 0) {
    self->tableDepth -= 1;
    self->tableRowIndex = 0;
    self->tableColIndex = 0;
    self->nextWordContinues = false;
  }

  // Leaving bold tag
  if (self->boldUntilDepth == self->depth) {
    self->boldUntilDepth = INT_MAX;
  }

  // Leaving italic tag
  if (self->italicUntilDepth == self->depth) {
    self->italicUntilDepth = INT_MAX;
  }

  // Leaving underline tag
  if (self->underlineUntilDepth == self->depth) {
    self->underlineUntilDepth = INT_MAX;
  }

  // Leaving pre tag
  if (self->preUntilDepth == self->depth) {
    self->preUntilDepth = INT_MAX;
  }

  // Pop from inline style stack if we pushed an entry at this depth
  // This handles all inline elements: b, i, u, span, etc.
  if (!self->inlineStyleStack.empty() && self->inlineStyleStack.back().depth == self->depth) {
    self->inlineStyleStack.pop_back();
    self->updateEffectiveInlineStyle();
  }

  // Clear block style when leaving header or block elements
  if (headerOrBlockTag) {
    self->currentCssStyle.reset();
    self->updateEffectiveInlineStyle();

    // Reset alignment on empty text blocks to prevent stale alignment from bleeding
    // into the next sibling element. This fixes issue #1026 where an empty <h1> (default
    // Center) followed by an image-only <p> causes Center to persist through the chain
    // of empty block reuse into subsequent text paragraphs.
    // Margins/padding are preserved so parent element spacing still accumulates correctly.
    if (self->currentTextBlock && self->currentTextBlock->isEmpty()) {
      auto style = self->currentTextBlock->getBlockStyle();
      // Keep alignment only when closing the <br> separator itself so subsequent text
      // within the same block container stays aligned. Reset alignment when closing
      // other block tags (e.g. div/p) to avoid leaking centered/right alignment globally.
      const bool preserveForBrClose = style.fromBrElement && strcmp(name, "br") == 0;
      if (!preserveForBrClose) {
        style.textAlignDefined = false;
        style.alignment = (self->paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None))
                              ? CssTextAlign::Justify
                              : static_cast<CssTextAlign>(self->paragraphAlignment);
        self->currentTextBlock->setBlockStyle(style);
      }
    }
  }
}

ChapterHtmlSlimParser::~ChapterHtmlSlimParser() {
  if (activeParser) {
    XML_StopParser(activeParser, XML_FALSE);
    XML_SetElementHandler(activeParser, nullptr, nullptr);
    XML_SetCharacterDataHandler(activeParser, nullptr);
    XML_ParserFree(activeParser);
    activeParser = nullptr;
  }
}

bool ChapterHtmlSlimParser::setup(const size_t totalInflatedSize) {
  auto paragraphAlignmentBlockStyle = BlockStyle();
  paragraphAlignmentBlockStyle.textAlignDefined = true;
  // Resolve None sentinel to Justify for initial block (no CSS context yet)
  const auto align = (this->paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None))
                         ? CssTextAlign::Justify
                         : static_cast<CssTextAlign>(this->paragraphAlignment);
  paragraphAlignmentBlockStyle.alignment = align;
  startNewTextBlock(paragraphAlignmentBlockStyle);

  XML_Parser parser = XML_ParserCreate(nullptr);
  if (!parser) {
    LOG_ERR("EHP", "Couldn't allocate memory for parser");
    return false;
  }

  // Handle HTML entities (like &nbsp;) that aren't in XML spec or DTD.
  // Using DefaultHandlerExpand preserves normal entity expansion from DOCTYPE.
  XML_SetDefaultHandlerExpand(parser, defaultHandlerExpand);
  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);
  activeParser = parser;

  totalStreamSize = totalInflatedSize;
  bytesStreamed = 0;
  lastReportedProgress = -1;
  streamFailed = false;
  streamStartTimeMs = millis();

  // Choose progress granularity by chapter size. Each callback drives a full-screen
  // e-ink refresh (~640ms), so smaller chapters skip mid-parse ticks entirely.
  // progressStepPercent == 0 means "popup only, no mid-parse updates".
  progressStepPercent = 0;
  if (totalStreamSize >= SIZE_FOR_PROGRESS_FINE) {
    progressStepPercent = 25;
  } else if (totalStreamSize >= SIZE_FOR_PROGRESS_HEARTBEAT) {
    progressStepPercent = 50;
  }

  // Show initial progress popup for files above threshold.
  if (progressFn && totalStreamSize >= MIN_SIZE_FOR_POPUP) {
    progressFn(0);
  }
  return true;
}

size_t ChapterHtmlSlimParser::write(const uint8_t data) { return write(&data, 1); }

size_t ChapterHtmlSlimParser::write(const uint8_t* buffer, const size_t size) {
  if (size == 0) return 0;
  if (!activeParser || streamFailed) return 0;

  size_t remaining = size;
  const uint8_t* cursor = buffer;
  while (remaining > 0) {
    const size_t chunk = remaining < PARSE_BUFFER_SIZE ? remaining : PARSE_BUFFER_SIZE;
    void* const buf = XML_GetBuffer(activeParser, static_cast<int>(chunk));
    if (!buf) {
      LOG_ERR("EHP", "Couldn't allocate buffer");
      streamFailed = true;
      return 0;
    }
    memcpy(buf, cursor, chunk);

    bytesStreamed += chunk;
    // The streaming source doesn't know "this was the last chunk" — pass isFinal=false
    // here and let finalize() emit the terminating empty parse with isFinal=true.
    if (XML_ParseBuffer(activeParser, static_cast<int>(chunk), 0) == XML_STATUS_ERROR) {
      LOG_ERR("EHP", "Parse error at line %lu:\n%s", XML_GetCurrentLineNumber(activeParser),
              XML_ErrorString(XML_GetErrorCode(activeParser)));
      streamFailed = true;
      return 0;
    }

    cursor += chunk;
    remaining -= chunk;
  }

  // Report progress at the granularity chosen up-front (see progressStepPercent).
  // Skip the 100% callback — the page render that follows immediately replaces the popup,
  // so the final tick is wasted work.
  if (progressFn && progressStepPercent > 0 && totalStreamSize > 0) {
    const int progress = static_cast<int>(bytesStreamed * 100 / totalStreamSize);
    if (progress < 100 && progress / progressStepPercent > lastReportedProgress / progressStepPercent) {
      lastReportedProgress = progress;
      progressFn(progress);
    }
  }

  return size;
}

bool ChapterHtmlSlimParser::finalize() {
  if (!activeParser) {
    return false;
  }

  bool success = !streamFailed;
  if (success) {
    // Emit terminating empty parse so Expat finalizes any pending tokens.
    if (XML_ParseBuffer(activeParser, 0, 1) == XML_STATUS_ERROR) {
      LOG_ERR("EHP", "Parse error at line %lu (finalize):\n%s", XML_GetCurrentLineNumber(activeParser),
              XML_ErrorString(XML_GetErrorCode(activeParser)));
      success = false;
      streamFailed = true;
    }
  }

  XML_StopParser(activeParser, XML_FALSE);
  XML_SetElementHandler(activeParser, nullptr, nullptr);
  XML_SetCharacterDataHandler(activeParser, nullptr);
  XML_ParserFree(activeParser);
  activeParser = nullptr;

  const uint32_t totalTimeMs = millis() - streamStartTimeMs;
  LOG_DBG("EHP", "Time to parse and build pages: %lu ms", totalTimeMs);

  // Process last page if there is still text. Done unconditionally so that a partial
  // success scenario still flushes whatever pages were produced.
  if (currentTextBlock) {
    makePages();
    if (!pendingAnchorId.empty()) {
      anchorData.push_back({std::move(pendingAnchorId), static_cast<uint16_t>(completedPageCount)});
      pendingAnchorId.clear();
    }
    emitPage(0u);  // post-parse: no byte offset available
    currentPage.reset();
    currentTextBlock.reset();
  }

  return success;
}

ParsedText::LineProcessResult ChapterHtmlSlimParser::addLineToPage(std::shared_ptr<TextBlock> line,
                                                                   const bool lineEndsWithHyphenatedWord,
                                                                   const bool suppressHyphenationRetry) {
  const int lineHeight = renderer.getLineHeight(fontId) * lineCompression;

  if (!currentPage) {
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }

  if (currentPageNextY + lineHeight > viewportHeight) {
    emitPage(lastBodyChildByteOffset);
  }

  const bool noRoomForAnotherLine =
      currentPageNextY + lineHeight <= viewportHeight && currentPageNextY + (lineHeight * 2) > viewportHeight;
  if (lineEndsWithHyphenatedWord && !suppressHyphenationRetry && noRoomForAnotherLine) {
    const std::string linePreview = buildTextBlockPreview(line);
    LOG_DBG("EHP", "Requesting line rerender without hyphenation to avoid page-break split word: %s",
            linePreview.c_str());
    return ParsedText::LineProcessResult::RetryWithoutHyphenation;
  }

  // Track cumulative words to assign footnotes to the page containing their anchor
  wordsExtractedInBlock += line->wordCount();
  auto footnoteIt = pendingFootnotes.begin();
  while (footnoteIt != pendingFootnotes.end() && footnoteIt->first <= wordsExtractedInBlock) {
    currentPage->addFootnote(footnoteIt->second.number, footnoteIt->second.href);
    ++footnoteIt;
  }
  pendingFootnotes.erase(pendingFootnotes.begin(), footnoteIt);

  // Apply horizontal left inset (margin + padding) as x position offset
  const int16_t xOffset = line->getBlockStyle().leftInset();
  currentPage->elements.push_back(std::make_shared<PageLine>(line, xOffset, currentPageNextY));
  currentPageNextY += lineHeight;
  return ParsedText::LineProcessResult::Accepted;
}

void ChapterHtmlSlimParser::makePages() {
  if (!currentTextBlock) {
    LOG_ERR("EHP", "!! No text block to make pages for !!");
    return;
  }

  if (!currentPage) {
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }

  const int lineHeight = renderer.getLineHeight(fontId) * lineCompression;

  // Apply top spacing before the paragraph (stored in pixels)
  const BlockStyle& blockStyle = currentTextBlock->getBlockStyle();
  if (blockStyle.marginTop > 0) {
    currentPageNextY += blockStyle.marginTop;
  }
  if (blockStyle.paddingTop > 0) {
    currentPageNextY += blockStyle.paddingTop;
  }

  // Calculate effective width accounting for horizontal margins/padding
  const int horizontalInset = blockStyle.totalHorizontalInset();
  const uint16_t effectiveWidth =
      (horizontalInset < viewportWidth) ? static_cast<uint16_t>(viewportWidth - horizontalInset) : viewportWidth;

  currentTextBlock->layoutAndExtractLines(
      renderer, fontId, effectiveWidth,
      [this](const std::shared_ptr<TextBlock>& textBlock, const bool lineEndsWithHyphenatedWord,
             const bool suppressHyphenationRetry) {
        return addLineToPage(textBlock, lineEndsWithHyphenatedWord, suppressHyphenationRetry);
      });

  // Fallback: transfer any remaining pending footnotes to current page.
  // Normally addLineToPage handles this via word-index tracking, but this catches
  // edge cases where a footnote's word index equals the exact block size.
  if (!pendingFootnotes.empty() && currentPage) {
    for (const auto& [idx, fn] : pendingFootnotes) {
      currentPage->addFootnote(fn.number, fn.href);
    }
    pendingFootnotes.clear();
  }

  // Apply bottom spacing after the paragraph (stored in pixels)
  if (blockStyle.marginBottom > 0) {
    currentPageNextY += blockStyle.marginBottom;
  }
  if (blockStyle.paddingBottom > 0) {
    currentPageNextY += blockStyle.paddingBottom;
  }

  // Extra paragraph spacing if enabled (default behavior).
  // Suppressed between lines within a <pre> block so code/preformatted text is not
  // double-spaced; the last line of the block is flushed after </pre> is closed and
  // preUntilDepth has already been reset, so it still receives normal paragraph spacing.
  if (extraParagraphSpacing && preUntilDepth == INT_MAX) {
    currentPageNextY += lineHeight / 2;
  }
}
