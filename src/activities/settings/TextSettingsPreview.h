#pragma once

#include <cstdint>
#include <memory>
#include <vector>

class GfxRenderer;
class TextBlock;

namespace textsettings {

// Settings + geometry that determine the laid-out lines; used to invalidate the cache.
struct PreviewKey {
  int fontId = -1;
  int fontSize = -1;
  int screenMargin = -1;
  int textWidth = -1;
  float lineCompression = -1.0f;
  uint8_t alignment = 0xFF;
  bool extraParagraphSpacing = false;
  bool focusReading = false;
  bool hyphenation = false;
  bool operator==(const PreviewKey&) const = default;
};

// Cached engine preview lines + the key that produced them
struct PreviewLayout {
  std::vector<std::shared_ptr<TextBlock>> lines;
  PreviewKey key;
};

// Draws the sample-text pane via the reader engine, reusing layout across redraws
void renderPreview(GfxRenderer& renderer, PreviewLayout& layout, int previewPadding, int labelGap, int top, int height,
                   const char* familyName, const char* sizeName);

}  // namespace textsettings
