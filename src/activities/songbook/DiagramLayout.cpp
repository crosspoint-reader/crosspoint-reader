#include "DiagramLayout.h"

#include <algorithm>

#include "ChordShapes.h"

namespace songbook {

void DiagramLayout::build(const Song& song, const SongChords& chords, const uint8_t strings,
                          const TextMeasurer& measurer, const Viewport area) {
  viewport = area;
  stringCount = std::max<uint8_t>(strings, 1);
  for (const FontSize size : {FontSize::Large, FontSize::Medium}) {
    if (tryBuild(size, song, chords, measurer)) return;
  }
  tryBuild(FontSize::Small, song, chords, measurer);
}

bool DiagramLayout::tryBuild(const FontSize size, const Song& song, const SongChords& chords,
                             const TextMeasurer& measurer) {
  fontSize = size;
  const int unit = measurer.lineHeight(TextRole::Chord, size);
  sizes.stringGap = std::max(6, unit * 9 / 20);
  sizes.fretGap = std::max(6, unit * 3 / 5);
  sizes.dotRadius = std::max(2, sizes.stringGap * 2 / 5);
  sizes.markHeight = sizes.stringGap;
  sizes.markRadius = std::max(2, sizes.dotRadius * 3 / 4);
  sizes.nameHeight = unit;
  sizes.padding = std::max(2, unit / 6);
  sizes.fretLabelWidth = measurer.width(TextRole::Label, size, "12") + sizes.padding;

  // Unknown names can be long text; they are clipped to the cell instead.
  int widestName = 0;
  for (size_t i = 0; i < chords.size(); ++i) {
    if (!chords[i].known) continue;
    widestName = std::max(widestName, measurer.width(TextRole::Chord, size, song.view(chords[i].name)));
  }
  const int gridWidth = (stringCount - 1) * sizes.stringGap;
  const int diagramWidth = gridWidth + 2 * sizes.dotRadius + sizes.fretLabelWidth;
  width = std::max(diagramWidth, widestName) + 2 * sizes.padding;
  height = sizes.nameHeight + sizes.markHeight + DIAGRAM_FRETS * sizes.fretGap + 2 * sizes.padding;

  columnCount = std::max(1, viewport.width / width);
  rowCount = std::max(1, viewport.height / height);
  originX = std::max(0, (viewport.width - columnCount * width) / 2);
  const size_t capacity = perPage();
  pages = static_cast<uint16_t>(std::max<size_t>(1, (chords.size() + capacity - 1) / capacity));
  return pages == 1;
}

DiagramLayout::Placement DiagramLayout::place(const size_t index) const {
  const size_t capacity = perPage();
  const size_t onPage = index % capacity;
  Placement placement;
  placement.page = static_cast<uint16_t>(index / capacity);
  placement.cellX = originX + static_cast<int>(onPage % columnCount) * width;
  placement.cellY = static_cast<int>(onPage / columnCount) * height;
  // The grid and the fret number right of it are centered as one block.
  placement.gridWidth = (stringCount - 1) * sizes.stringGap;
  const int diagramWidth = placement.gridWidth + 2 * sizes.dotRadius + sizes.fretLabelWidth;
  placement.gridX = placement.cellX + (width - diagramWidth) / 2 + sizes.dotRadius;
  placement.gridY = placement.cellY + sizes.padding + sizes.nameHeight + sizes.markHeight;
  placement.fretLabelX = placement.gridX + placement.gridWidth + sizes.dotRadius + sizes.padding;
  placement.markY = placement.gridY - sizes.markHeight / 2 - DIAGRAM_LINE_WIDTH;
  return placement;
}

}  // namespace songbook
