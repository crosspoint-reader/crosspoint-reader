#include "EpubHtmlParserSlim.h"

#include <EpdRenderer.h>
#include <HardwareSerial.h>

#include "Page.h"
#include "htmlEntities.h"

const char* HEADER_TAGS[] = {"h1", "h2", "h3", "h4", "h5", "h6"};
constexpr int NUM_HEADER_TAGS = sizeof(HEADER_TAGS) / sizeof(HEADER_TAGS[0]);

const char* BLOCK_TAGS[] = {"p", "li", "div", "br"};
constexpr int NUM_BLOCK_TAGS = sizeof(BLOCK_TAGS) / sizeof(BLOCK_TAGS[0]);

const char* BOLD_TAGS[] = {"b"};
constexpr int NUM_BOLD_TAGS = sizeof(BOLD_TAGS) / sizeof(BOLD_TAGS[0]);

const char* ITALIC_TAGS[] = {"i"};
constexpr int NUM_ITALIC_TAGS = sizeof(ITALIC_TAGS) / sizeof(ITALIC_TAGS[0]);

const char* IMAGE_TAGS[] = {"img"};
constexpr int NUM_IMAGE_TAGS = sizeof(IMAGE_TAGS) / sizeof(IMAGE_TAGS[0]);

const char* SKIP_TAGS[] = {"head", "table"};
constexpr int NUM_SKIP_TAGS = sizeof(SKIP_TAGS) / sizeof(SKIP_TAGS[0]);

// given the start and end of a tag, check to see if it matches a known tag
bool matches_s(const char* tag_name, const char* possible_tags[], const int possible_tag_count) {
  for (int i = 0; i < possible_tag_count; i++) {
    if (strcmp(tag_name, possible_tags[i]) == 0) {
      return true;
    }
  }
  return false;
}

// start a new text block if needed
void EpubHtmlParserSlim::startNewTextBlock(const BLOCK_STYLE style) {
  if (currentTextBlock) {
    // already have a text block running and it is empty - just reuse it
    if (currentTextBlock->isEmpty()) {
      currentTextBlock->setStyle(style);
      return;
    }

    currentTextBlock->finish();
    makePages();
    delete currentTextBlock;
  }
  currentTextBlock = new TextBlock(style);
}

#include <expat.h>

void XMLCALL EpubHtmlParserSlim::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<EpubHtmlParserSlim*>(userData);
  (void)atts;

  // Middle of skip
  if (self->skipUntilDepth < self->depth) {
    self->depth += 1;
    return;
  }

  if (matches_s(name, IMAGE_TAGS, NUM_IMAGE_TAGS)) {
    // const char* src = element.Attribute("src");
    // if (src) {
    //   // don't leave an empty text block in the list
    //   // const BLOCK_STYLE style = currentTextBlock->get_style();
    //   if (currentTextBlock->isEmpty()) {
    //     delete currentTextBlock;
    //     currentTextBlock = nullptr;
    //   }
    //   // TODO: Fix this
    //   // blocks.push_back(new ImageBlock(m_base_path + src));
    //   // start a new text block - with the same style as before
    //   // startNewTextBlock(style);
    // } else {
    //   // ESP_LOGE(TAG, "Could not find src attribute");
    // }

    // start skip
    self->skipUntilDepth = self->depth;
    self->depth += 1;
    return;
  }

  if (matches_s(name, SKIP_TAGS, NUM_SKIP_TAGS)) {
    // start skip
    self->skipUntilDepth = self->depth;
    self->depth += 1;
    return;
  }

  if (matches_s(name, HEADER_TAGS, NUM_HEADER_TAGS)) {
    self->startNewTextBlock(CENTER_ALIGN);
    self->boldUntilDepth = min(self->boldUntilDepth, self->depth);
  } else if (matches_s(name, BLOCK_TAGS, NUM_BLOCK_TAGS)) {
    if (strcmp(name, "br") == 0) {
      self->startNewTextBlock(self->currentTextBlock->getStyle());
    } else {
      self->startNewTextBlock(JUSTIFIED);
    }
  } else if (matches_s(name, BOLD_TAGS, NUM_BOLD_TAGS)) {
    self->boldUntilDepth = min(self->boldUntilDepth, self->depth);
  } else if (matches_s(name, ITALIC_TAGS, NUM_ITALIC_TAGS)) {
    self->italicUntilDepth = min(self->italicUntilDepth, self->depth);
  }

  self->depth += 1;
}

void XMLCALL EpubHtmlParserSlim::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<EpubHtmlParserSlim*>(userData);

  // Middle of skip
  if (self->skipUntilDepth < self->depth) {
    return;
  }

  for (int i = 0; i < len; i++) {
    // TODO: Extract check
    if (s[i] == ' ' || s[i] == '\r' || s[i] == '\n') {
      if (self->partWordBufferIndex > 0) {
        self->partWordBuffer[self->partWordBufferIndex] = '\0';
        self->currentTextBlock->addWord(replaceHtmlEntities(self->partWordBuffer), self->boldUntilDepth < self->depth,
                                        self->italicUntilDepth < self->depth);
        self->partWordBufferIndex = 0;
      }
      continue;
    }

    if (self->partWordBufferIndex >= PART_WORD_BUFFER_SIZE - 2) {
      self->partWordBuffer[self->partWordBufferIndex] = '\0';
      self->currentTextBlock->addWord(replaceHtmlEntities(self->partWordBuffer), self->boldUntilDepth < self->depth,
                                      self->italicUntilDepth < self->depth);
      self->partWordBufferIndex = 0;
    }

    self->partWordBuffer[self->partWordBufferIndex++] = s[i];
  }
}

void XMLCALL EpubHtmlParserSlim::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<EpubHtmlParserSlim*>(userData);
  (void)name;

  if (self->partWordBufferIndex > 0) {
    self->partWordBuffer[self->partWordBufferIndex] = '\0';
    self->currentTextBlock->addWord(replaceHtmlEntities(self->partWordBuffer), self->boldUntilDepth < self->depth,
                                    self->italicUntilDepth < self->depth);
    self->partWordBufferIndex = 0;
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

bool EpubHtmlParserSlim::parseAndBuildPages() {
  startNewTextBlock(JUSTIFIED);

  const XML_Parser parser = XML_ParserCreate(nullptr);
  int done;

  if (!parser) {
    Serial.println("Couldn't allocate memory for parser");
    return false;
  }

  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);

  FILE* file = fopen(filepath, "r");

  do {
    void* const buf = XML_GetBuffer(parser, BUFSIZ);
    if (!buf) {
      Serial.println("Couldn't allocate memory for buffer");
      XML_ParserFree(parser);
      fclose(file);
      return false;
    }

    const size_t len = fread(buf, 1, BUFSIZ, file);

    if (ferror(file)) {
      Serial.println("Read error");
      XML_ParserFree(parser);
      fclose(file);
      return false;
    }

    done = feof(file);

    if (XML_ParseBuffer(parser, static_cast<int>(len), done) == XML_STATUS_ERROR) {
      Serial.printf("Parse error at line %lu:\n%s\n", XML_GetCurrentLineNumber(parser),
                    XML_ErrorString(XML_GetErrorCode(parser)));
      XML_ParserFree(parser);
      fclose(file);
      return false;
    }
  } while (!done);

  XML_ParserFree(parser);
  fclose(file);

  // Process last page if there is still text
  if (currentTextBlock) {
    makePages();
    completePageFn(currentPage);
    currentPage = nullptr;
    delete currentTextBlock;
    currentTextBlock = nullptr;
  }

  return true;
}

void EpubHtmlParserSlim::makePages() {
  if (!currentTextBlock) {
    Serial.println("!! No text block to make pages for !!");
    return;
  }

  if (!currentPage) {
    currentPage = new Page();
  }

  const int lineHeight = renderer.getLineHeight();
  const int pageHeight = renderer.getPageHeight();

  // Long running task, make sure to let other things happen
  vTaskDelay(1);

  if (currentTextBlock->getType() == TEXT_BLOCK) {
    const auto lines = currentTextBlock->splitIntoLines(renderer);

    for (const auto line : lines) {
      if (currentPage->nextY + lineHeight > pageHeight) {
        completePageFn(currentPage);
        currentPage = new Page();
      }

      currentPage->elements.push_back(new PageLine(line, currentPage->nextY));
      currentPage->nextY += lineHeight;
    }
    // add some extra line between blocks
    currentPage->nextY += lineHeight / 2;
  }
  // TODO: Image block support
  // if (block->getType() == BlockType::IMAGE_BLOCK) {
  //   ImageBlock *imageBlock = (ImageBlock *)block;
  //   if (y + imageBlock->height > page_height) {
  //     pages.push_back(new Page());
  //     y = 0;
  //   }
  //   pages.back()->elements.push_back(new PageImage(imageBlock, y));
  //   y += imageBlock->height;
  // }
}
