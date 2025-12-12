#pragma once
#include <utility>
#include <vector>

#include "blocks/TextBlock.h"

enum PageElementTag : uint8_t {
  TAG_PageLine = 1,
};

// represents something that has been added to a page
class PageElement {
 public:
  int xPos;
  int yPos;
  explicit PageElement(const int xPos, const int yPos) : xPos(xPos), yPos(yPos) {}
  virtual ~PageElement() = default;
  virtual void render(GfxRenderer& renderer, int fontId) = 0;
  virtual void serialize(std::ostream& os) = 0;
};

// a line from a block element
class PageLine final : public PageElement {
  std::shared_ptr<TextBlock> block;

 public:
  PageLine(std::shared_ptr<TextBlock> block, const int xPos, const int yPos)
      : PageElement(xPos, yPos), block(std::move(block)) {}
  void render(GfxRenderer& renderer, int fontId) override;
  void serialize(std::ostream& os) override;
  static std::unique_ptr<PageLine> deserialize(std::istream& is);
};

class Page {
 public:
  // the list of block index and line numbers on this page
  std::vector<std::shared_ptr<PageElement>> elements;
  void render(GfxRenderer& renderer, int fontId) const;
  void serialize(std::ostream& os) const;
  static std::unique_ptr<Page> deserialize(std::istream& is);
};
