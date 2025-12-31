#include "ChapterHtmlSlimParser.h"

#include <Bitmap.h>
#include <Epub.h>
#include <GfxRenderer.h>
#include <HardwareSerial.h>
#include <JpegToBmpConverter.h>
#include <SDCardManager.h>
#include <expat.h>

#include "../Page.h"
#include "../blocks/ImageBlock.h"
#include "../htmlEntities.h"

const char* HEADER_TAGS[] = {"h1", "h2", "h3", "h4", "h5", "h6"};
constexpr int NUM_HEADER_TAGS = sizeof(HEADER_TAGS) / sizeof(HEADER_TAGS[0]);

// Minimum file size (in bytes) to show progress bar - smaller chapters don't benefit from it
constexpr size_t MIN_SIZE_FOR_PROGRESS = 50 * 1024;  // 50KB

const char* BLOCK_TAGS[] = {"p", "li", "div", "br", "blockquote"};
constexpr int NUM_BLOCK_TAGS = sizeof(BLOCK_TAGS) / sizeof(BLOCK_TAGS[0]);

const char* BOLD_TAGS[] = {"b", "strong"};
constexpr int NUM_BOLD_TAGS = sizeof(BOLD_TAGS) / sizeof(BOLD_TAGS[0]);

const char* ITALIC_TAGS[] = {"i", "em"};
constexpr int NUM_ITALIC_TAGS = sizeof(ITALIC_TAGS) / sizeof(ITALIC_TAGS[0]);

const char* IMAGE_TAGS[] = {"img"};
constexpr int NUM_IMAGE_TAGS = sizeof(IMAGE_TAGS) / sizeof(IMAGE_TAGS[0]);

const char* SKIP_TAGS[] = {"head", "table"};
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

// start a new text block if needed
void ChapterHtmlSlimParser::startNewTextBlock(const TextBlock::BLOCK_STYLE style) {
  if (currentTextBlock) {
    // already have a text block running and it is empty - just reuse it
    if (currentTextBlock->isEmpty()) {
      currentTextBlock->setStyle(style);
      return;
    }

    makePages();
  }
  currentTextBlock.reset(new ParsedText(style, extraParagraphSpacing));
}

void XMLCALL ChapterHtmlSlimParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);

  // Middle of skip
  if (self->skipUntilDepth < self->depth) {
    self->depth += 1;
    return;
  }

  if (matches(name, IMAGE_TAGS, NUM_IMAGE_TAGS)) {
    // Process image tag - extract src attribute
    const char* src = nullptr;
    if (atts != nullptr) {
      for (int i = 0; atts[i]; i += 2) {
        if (strcmp(atts[i], "src") == 0) {
          src = atts[i + 1];
          break;
        }
      }
    }

    if (src && self->epub) {
      self->processImage(src);
    }

    // Skip content inside image tag
    self->skipUntilDepth = self->depth;
    self->depth += 1;
    return;
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

  if (matches(name, HEADER_TAGS, NUM_HEADER_TAGS)) {
    self->startNewTextBlock(TextBlock::CENTER_ALIGN);
    self->boldUntilDepth = std::min(self->boldUntilDepth, self->depth);
  } else if (matches(name, BLOCK_TAGS, NUM_BLOCK_TAGS)) {
    if (strcmp(name, "br") == 0) {
      self->startNewTextBlock(self->currentTextBlock->getStyle());
    } else {
      self->startNewTextBlock(TextBlock::JUSTIFIED);
    }
  } else if (matches(name, BOLD_TAGS, NUM_BOLD_TAGS)) {
    self->boldUntilDepth = std::min(self->boldUntilDepth, self->depth);
  } else if (matches(name, ITALIC_TAGS, NUM_ITALIC_TAGS)) {
    self->italicUntilDepth = std::min(self->italicUntilDepth, self->depth);
  }

  self->depth += 1;
}

void XMLCALL ChapterHtmlSlimParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);

  // Middle of skip
  if (self->skipUntilDepth < self->depth) {
    return;
  }

  EpdFontStyle fontStyle = REGULAR;
  if (self->boldUntilDepth < self->depth && self->italicUntilDepth < self->depth) {
    fontStyle = BOLD_ITALIC;
  } else if (self->boldUntilDepth < self->depth) {
    fontStyle = BOLD;
  } else if (self->italicUntilDepth < self->depth) {
    fontStyle = ITALIC;
  }

  for (int i = 0; i < len; i++) {
    if (isWhitespace(s[i])) {
      // Currently looking at whitespace, if there's anything in the partWordBuffer, flush it
      if (self->partWordBufferIndex > 0) {
        self->partWordBuffer[self->partWordBufferIndex] = '\0';
        self->currentTextBlock->addWord(std::move(replaceHtmlEntities(self->partWordBuffer)), fontStyle);
        self->partWordBufferIndex = 0;
      }
      // Skip the whitespace char
      continue;
    }

    // If we're about to run out of space, then cut the word off and start a new one
    if (self->partWordBufferIndex >= MAX_WORD_SIZE) {
      self->partWordBuffer[self->partWordBufferIndex] = '\0';
      self->currentTextBlock->addWord(std::move(replaceHtmlEntities(self->partWordBuffer)), fontStyle);
      self->partWordBufferIndex = 0;
    }

    self->partWordBuffer[self->partWordBufferIndex++] = s[i];
  }

  // If we have > 750 words buffered up, perform the layout and consume out all but the last line
  // There should be enough here to build out 1-2 full pages and doing this will free up a lot of
  // memory.
  // Spotted when reading Intermezzo, there are some really long text blocks in there.
  if (self->currentTextBlock->size() > 750) {
    Serial.printf("[%lu] [EHP] Text block too long, splitting into multiple pages\n", millis());
    self->currentTextBlock->layoutAndExtractLines(
        self->renderer, self->fontId, self->viewportWidth,
        [self](const std::shared_ptr<TextBlock>& textBlock) { self->addLineToPage(textBlock); }, false);
  }
}

void XMLCALL ChapterHtmlSlimParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);

  if (self->partWordBufferIndex > 0) {
    // Only flush out part word buffer if we're closing a block tag or are at the top of the HTML file.
    // We don't want to flush out content when closing inline tags like <span>.
    // Currently this also flushes out on closing <b> and <i> tags, but they are line tags so that shouldn't happen,
    // text styling needs to be overhauled to fix it.
    const bool shouldBreakText =
        matches(name, BLOCK_TAGS, NUM_BLOCK_TAGS) || matches(name, HEADER_TAGS, NUM_HEADER_TAGS) ||
        matches(name, BOLD_TAGS, NUM_BOLD_TAGS) || matches(name, ITALIC_TAGS, NUM_ITALIC_TAGS) || self->depth == 1;

    if (shouldBreakText) {
      EpdFontStyle fontStyle = REGULAR;
      if (self->boldUntilDepth < self->depth && self->italicUntilDepth < self->depth) {
        fontStyle = BOLD_ITALIC;
      } else if (self->boldUntilDepth < self->depth) {
        fontStyle = BOLD;
      } else if (self->italicUntilDepth < self->depth) {
        fontStyle = ITALIC;
      }

      self->partWordBuffer[self->partWordBufferIndex] = '\0';
      self->currentTextBlock->addWord(std::move(replaceHtmlEntities(self->partWordBuffer)), fontStyle);
      self->partWordBufferIndex = 0;
    }
  }

  self->depth -= 1;

  // Leaving skip
  if (self->skipUntilDepth == self->depth) {
    self->skipUntilDepth = INT_MAX;
  }

  // Leaving bold
  if (self->boldUntilDepth == self->depth) {
    self->boldUntilDepth = INT_MAX;
  }

  // Leaving italic
  if (self->italicUntilDepth == self->depth) {
    self->italicUntilDepth = INT_MAX;
  }
}

bool ChapterHtmlSlimParser::parseAndBuildPages() {
  startNewTextBlock(TextBlock::JUSTIFIED);

  const XML_Parser parser = XML_ParserCreate(nullptr);
  int done;

  if (!parser) {
    Serial.printf("[%lu] [EHP] Couldn't allocate memory for parser\n", millis());
    return false;
  }

  FsFile file;
  if (!SdMan.openFileForRead("EHP", filepath, file)) {
    XML_ParserFree(parser);
    return false;
  }

  // Get file size for progress calculation
  const size_t totalSize = file.size();
  size_t bytesRead = 0;
  int lastProgress = -1;

  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);

  do {
    void* const buf = XML_GetBuffer(parser, 1024);
    if (!buf) {
      Serial.printf("[%lu] [EHP] Couldn't allocate memory for buffer\n", millis());
      XML_StopParser(parser, XML_FALSE);                // Stop any pending processing
      XML_SetElementHandler(parser, nullptr, nullptr);  // Clear callbacks
      XML_SetCharacterDataHandler(parser, nullptr);
      XML_ParserFree(parser);
      file.close();
      return false;
    }

    const size_t len = file.read(buf, 1024);

    if (len == 0 && file.available() > 0) {
      Serial.printf("[%lu] [EHP] File read error\n", millis());
      XML_StopParser(parser, XML_FALSE);                // Stop any pending processing
      XML_SetElementHandler(parser, nullptr, nullptr);  // Clear callbacks
      XML_SetCharacterDataHandler(parser, nullptr);
      XML_ParserFree(parser);
      file.close();
      return false;
    }

    // Update progress (call every 10% change to avoid too frequent updates)
    // Only show progress for larger chapters where rendering overhead is worth it
    bytesRead += len;
    if (progressFn && totalSize >= MIN_SIZE_FOR_PROGRESS) {
      const int progress = static_cast<int>((bytesRead * 100) / totalSize);
      if (lastProgress / 10 != progress / 10) {
        lastProgress = progress;
        progressFn(progress);
      }
    }

    done = file.available() == 0;

    if (XML_ParseBuffer(parser, static_cast<int>(len), done) == XML_STATUS_ERROR) {
      Serial.printf("[%lu] [EHP] Parse error at line %lu:\n%s\n", millis(), XML_GetCurrentLineNumber(parser),
                    XML_ErrorString(XML_GetErrorCode(parser)));
      XML_StopParser(parser, XML_FALSE);                // Stop any pending processing
      XML_SetElementHandler(parser, nullptr, nullptr);  // Clear callbacks
      XML_SetCharacterDataHandler(parser, nullptr);
      XML_ParserFree(parser);
      file.close();
      return false;
    }
  } while (!done);

  XML_StopParser(parser, XML_FALSE);                // Stop any pending processing
  XML_SetElementHandler(parser, nullptr, nullptr);  // Clear callbacks
  XML_SetCharacterDataHandler(parser, nullptr);
  XML_ParserFree(parser);
  file.close();

  // Process last page if there is still text
  if (currentTextBlock) {
    makePages();
    completePageFn(std::move(currentPage));
    currentPage.reset();
    currentTextBlock.reset();
  }

  return true;
}

void ChapterHtmlSlimParser::addLineToPage(std::shared_ptr<TextBlock> line) {
  const int lineHeight = renderer.getLineHeight(fontId) * lineCompression;

  if (currentPageNextY + lineHeight > viewportHeight) {
    completePageFn(std::move(currentPage));
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }

  currentPage->elements.push_back(std::make_shared<PageLine>(line, 0, currentPageNextY));
  currentPageNextY += lineHeight;
}

void ChapterHtmlSlimParser::makePages() {
  if (!currentTextBlock) {
    Serial.printf("[%lu] [EHP] !! No text block to make pages for !!\n", millis());
    return;
  }

  if (!currentPage) {
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }

  const int lineHeight = renderer.getLineHeight(fontId) * lineCompression;
  currentTextBlock->layoutAndExtractLines(
      renderer, fontId, viewportWidth,
      [this](const std::shared_ptr<TextBlock>& textBlock) { addLineToPage(textBlock); });
  // Extra paragraph spacing if enabled
  if (extraParagraphSpacing) {
    currentPageNextY += lineHeight / 2;
  }
}

std::string ChapterHtmlSlimParser::resolveImagePath(const char* src) const {
  if (!src || !epub) {
    return "";
  }

  std::string srcPath(src);

  // If the path is absolute (starts with /), it's relative to the EPUB root
  if (srcPath[0] == '/') {
    return srcPath.substr(1);  // Remove leading slash
  }

  // Otherwise, resolve relative to the current chapter's directory
  std::string basePath = epub->getBasePath();

  // Simple path resolution - combine basePath with src
  if (basePath.empty()) {
    return srcPath;
  }

  // Ensure basePath ends with /
  if (basePath.back() != '/') {
    basePath += '/';
  }

  return basePath + srcPath;
}

void ChapterHtmlSlimParser::processImage(const char* src) {
  if (!src || !epub) {
    Serial.printf("[%lu] [IMG] Invalid image source or epub pointer\n", millis());
    return;
  }

  // Flush any pending text before adding image
  if (currentTextBlock && !currentTextBlock->isEmpty()) {
    makePages();
    currentTextBlock.reset(new ParsedText(TextBlock::JUSTIFIED, extraParagraphSpacing));
  }

  // Resolve the image path relative to the EPUB structure
  std::string imagePath = resolveImagePath(src);
  if (imagePath.empty()) {
    Serial.printf("[%lu] [IMG] Failed to resolve image path: %s\n", millis(), src);
    return;
  }

  // Generate cache path for this image
  std::string cachePath = epub->getCachePath() + "/sections/" + std::to_string(spineIndex) + "_img_" +
                          std::to_string(imageCounter++) + ".bmp";

  // Check if image is already cached
  if (!SdMan.exists(cachePath.c_str())) {
    // Determine file extension
    bool isJpeg = false;
    bool isBmp = false;
    size_t dotPos = imagePath.rfind('.');
    if (dotPos != std::string::npos) {
      std::string ext = imagePath.substr(dotPos);
      isJpeg = (ext == ".jpg" || ext == ".jpeg" || ext == ".JPG" || ext == ".JPEG");
      isBmp = (ext == ".bmp" || ext == ".BMP");
    }

    if (isJpeg) {
      // Extract JPEG and convert to BMP
      std::string tempJpgPath = epub->getCachePath() + "/.temp_img.jpg";

      FsFile tempJpg;
      if (!SdMan.openFileForWrite("IMG", tempJpgPath.c_str(), tempJpg)) {
        Serial.printf("[%lu] [IMG] Failed to create temp JPEG file\n", millis());
        return;
      }

      if (!epub->readItemContentsToStream(imagePath, tempJpg, 1024)) {
        Serial.printf("[%lu] [IMG] Failed to extract JPEG: %s\n", millis(), imagePath.c_str());
        tempJpg.close();
        SdMan.remove(tempJpgPath.c_str());
        return;
      }
      tempJpg.close();

      // Convert JPEG to BMP
      if (!SdMan.openFileForRead("IMG", tempJpgPath.c_str(), tempJpg)) {
        Serial.printf("[%lu] [IMG] Failed to reopen temp JPEG\n", millis());
        SdMan.remove(tempJpgPath.c_str());
        return;
      }

      FsFile bmpFile;
      if (!SdMan.openFileForWrite("IMG", cachePath.c_str(), bmpFile)) {
        Serial.printf("[%lu] [IMG] Failed to create BMP file\n", millis());
        tempJpg.close();
        SdMan.remove(tempJpgPath.c_str());
        return;
      }

      bool success = JpegToBmpConverter::jpegFileToBmpStream(tempJpg, bmpFile);
      tempJpg.close();
      bmpFile.close();
      SdMan.remove(tempJpgPath.c_str());

      if (!success) {
        Serial.printf("[%lu] [IMG] Failed to convert JPEG to BMP: %s\n", millis(), imagePath.c_str());
        SdMan.remove(cachePath.c_str());
        return;
      }
    } else if (isBmp) {
      // Extract BMP directly
      FsFile bmpFile;
      if (!SdMan.openFileForWrite("IMG", cachePath.c_str(), bmpFile)) {
        Serial.printf("[%lu] [IMG] Failed to create BMP file\n", millis());
        return;
      }

      if (!epub->readItemContentsToStream(imagePath, bmpFile, 1024)) {
        Serial.printf("[%lu] [IMG] Failed to extract BMP: %s\n", millis(), imagePath.c_str());
        bmpFile.close();
        SdMan.remove(cachePath.c_str());
        return;
      }
      bmpFile.close();
    } else {
      // Unsupported format
      Serial.printf("[%lu] [IMG] Unsupported image format (not JPEG or BMP): %s\n", millis(), imagePath.c_str());
      return;
    }
  }

  // Read image dimensions
  FsFile imageFile;
  if (!SdMan.openFileForRead("IMG", cachePath.c_str(), imageFile)) {
    Serial.printf("[%lu] [IMG] Failed to open cached image: %s\n", millis(), cachePath.c_str());
    return;
  }

  Bitmap bitmap(imageFile);
  BmpReaderError err = bitmap.parseHeaders();
  if (err != BmpReaderError::Ok) {
    Serial.printf("[%lu] [IMG] Failed to parse BMP headers: %s\n", millis(), Bitmap::errorToString(err));
    imageFile.close();
    return;
  }

  int width = bitmap.getWidth();
  int height = bitmap.getHeight();
  imageFile.close();

  // Add image to page
  addImageToPage(cachePath, width, height);

  Serial.printf("[%lu] [IMG] Successfully processed image: %s (%dx%d)\n", millis(), imagePath.c_str(), width, height);
}

void ChapterHtmlSlimParser::addImageToPage(const std::string& cachePath, int width, int height) {
  // Calculate scaled dimensions to fit viewport
  ImageBlock imageBlock(cachePath, width, height);
  int scaledWidth, scaledHeight;
  imageBlock.getScaledDimensions(viewportWidth, viewportHeight, &scaledWidth, &scaledHeight);

  // Check if image fits on current page
  if (currentPageNextY + scaledHeight > viewportHeight && currentPageNextY > 0) {
    // Start new page for image
    if (currentPage && !currentPage->elements.empty()) {
      completePageFn(std::move(currentPage));
    }
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }

  if (!currentPage) {
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }

  // Center image horizontally
  int xPos = (viewportWidth - scaledWidth) / 2;
  if (xPos < 0) xPos = 0;

  // Add image to page
  currentPage->elements.push_back(std::make_shared<PageImage>(cachePath, scaledWidth, scaledHeight, xPos, currentPageNextY));
  currentPageNextY += scaledHeight;

  // Add some spacing after image
  const int lineHeight = renderer.getLineHeight(fontId) * lineCompression;
  currentPageNextY += lineHeight;
}
