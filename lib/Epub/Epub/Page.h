#include <SdFat.h>

#include <utility>
#include <vector>

#include "FootnoteEntry.h"
#include "blocks/TextBlock.h"

enum PageElementTag : uint8_t {
  TAG_PageLine = 1,
};

class PageElement {
 public:
  int16_t xPos;
  int16_t yPos;
  explicit PageElement(const int16_t xPos, const int16_t yPos) : xPos(xPos), yPos(yPos) {}
  virtual ~PageElement() = default;
  virtual void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset) = 0;
  virtual bool serialize(FsFile& file) = 0;
};

class PageLine final : public PageElement {
  std::shared_ptr<TextBlock> block;

 public:
  PageLine(std::shared_ptr<TextBlock> block, const int16_t xPos, const int16_t yPos)
      : PageElement(xPos, yPos), block(std::move(block)) {}
  void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset) override;
  bool serialize(FsFile& file) override;
  static std::unique_ptr<PageLine> deserialize(FsFile& file);
};

class Page {
 public:
  // the list of block index and line numbers on this page
  std::vector<std::shared_ptr<PageElement>> elements;
  std::vector<FootnoteEntry> footnotes;

  void addFootnote(const char* number, const char* href) {
    FootnoteEntry entry;
    // ensure null termination and bounds
    strncpy(entry.number, number, 2);
    entry.number[2] = '\0';
    strncpy(entry.href, href, 63);
    entry.href[63] = '\0';
    entry.isInline = false;  // Default
    footnotes.push_back(entry);
  }

  void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset) const;
  bool serialize(FsFile& file) const;
  static std::unique_ptr<Page> deserialize(FsFile& file);
};
