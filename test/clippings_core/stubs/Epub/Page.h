#pragma once

#include <EpdFontFamily.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

enum class CssTextAlign : uint8_t { Justify, Left, Right, Center, None };

struct BlockStyle {
  CssTextAlign alignment = CssTextAlign::Justify;
  int16_t marginTop = 0;
  int16_t marginBottom = 0;
  int16_t marginLeft = 0;
  int16_t marginRight = 0;
  int16_t paddingTop = 0;
  int16_t paddingBottom = 0;
  int16_t paddingLeft = 0;
  int16_t paddingRight = 0;
  int16_t textIndent = 0;
  bool textIndentDefined = false;
  bool textAlignDefined = false;
  bool isRtl = false;
  bool directionDefined = false;
  bool fromBrElement = false;
};

class TextBlock {
 public:
  TextBlock(std::vector<std::string> words, std::vector<int16_t> xPositions, std::vector<EpdFontFamily::Style> styles,
            std::vector<uint8_t> focusBoundaries = {}, std::vector<uint16_t> focusSuffixes = {}, BlockStyle style = {})
      : words_(std::move(words)),
        xPositions_(std::move(xPositions)),
        styles_(std::move(styles)),
        focusBoundaries_(std::move(focusBoundaries)),
        focusSuffixes_(std::move(focusSuffixes)),
        style_(style) {}

  bool valid() const { return words_.size() == xPositions_.size() && words_.size() == styles_.size(); }
  uint16_t wordCount() const { return static_cast<uint16_t>(words_.size()); }
  const char* wordText(const uint16_t index) const { return words_[index].c_str(); }
  uint16_t wordTextLen(const uint16_t index) const { return static_cast<uint16_t>(words_[index].size()); }
  int16_t wordXpos(const uint16_t index) const { return xPositions_[index]; }
  EpdFontFamily::Style wordStyle(const uint16_t index) const { return styles_[index]; }
  uint8_t focusBoundary(const uint16_t index) const {
    return index < focusBoundaries_.size() ? focusBoundaries_[index] : 0;
  }
  uint16_t focusSuffixX(const uint16_t index) const {
    return index < focusSuffixes_.size() ? focusSuffixes_[index] : 0;
  }
  const BlockStyle& getBlockStyle() const { return style_; }

 private:
  std::vector<std::string> words_;
  std::vector<int16_t> xPositions_;
  std::vector<EpdFontFamily::Style> styles_;
  std::vector<uint8_t> focusBoundaries_;
  std::vector<uint16_t> focusSuffixes_;
  BlockStyle style_;
};

enum PageElementTag : uint8_t { TAG_PageLine = 1, TAG_PageImage = 2, TAG_PageHorizontalRule = 3 };

class PageElement {
 public:
  PageElement(const int16_t x, const int16_t y) : xPos(x), yPos(y) {}
  virtual ~PageElement() = default;
  virtual PageElementTag getTag() const = 0;

  int16_t xPos;
  int16_t yPos;
};

class PageLine final : public PageElement {
 public:
  PageLine(std::shared_ptr<TextBlock> block, const int16_t x, const int16_t y)
      : PageElement(x, y), block_(std::move(block)) {}
  PageElementTag getTag() const override { return TAG_PageLine; }
  const std::shared_ptr<TextBlock>& getBlock() const { return block_; }

 private:
  std::shared_ptr<TextBlock> block_;
};

class ImageBlock {
 public:
  ImageBlock(const int width, const int height) : width_(width), height_(height) {}
  int getWidth() const { return width_; }
  int getHeight() const { return height_; }

 private:
  int width_;
  int height_;
};

class PageImage final : public PageElement {
 public:
  PageImage(const int width, const int height, const int16_t x, const int16_t y)
      : PageElement(x, y), block_(width, height) {}
  PageElementTag getTag() const override { return TAG_PageImage; }
  const ImageBlock& getImageBlock() const { return block_; }

 private:
  ImageBlock block_;
};

class PageHorizontalRule final : public PageElement {
 public:
  PageHorizontalRule(const int16_t x, const int16_t y) : PageElement(x, y) {}
  PageElementTag getTag() const override { return TAG_PageHorizontalRule; }
};

class Page {
 public:
  std::vector<std::shared_ptr<PageElement>> elements;
};
