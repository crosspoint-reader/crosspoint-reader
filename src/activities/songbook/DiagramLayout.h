#pragma once

#include <cstddef>
#include <cstdint>

#include "SongChords.h"
#include "SongLayout.h"

namespace songbook {

// Width of the open/muted marks and of the ring around a bass chord tone.
constexpr int DIAGRAM_LINE_WIDTH = 2;

// Grid of chord diagrams for one instrument. Picks the largest size that fits
// every chord on one screen; otherwise the smallest size, over several pages.
// Sizes derive from the chord font, so the grid follows the UI scale.
class DiagramLayout {
 public:
  struct Metrics {
    int stringGap = 0;
    int fretGap = 0;
    int dotRadius = 0;
    // Row above the nut for the open and muted marks.
    int markHeight = 0;
    int markRadius = 0;
    int nameHeight = 0;
    // Room right of the grid for the first-fret number.
    int fretLabelWidth = 0;
    int padding = 0;
  };

  // Where one diagram goes, relative to the viewport.
  struct Placement {
    uint16_t page = 0;
    int cellX = 0;
    int cellY = 0;
    // First drawn string and the top line of the grid.
    int gridX = 0;
    int gridY = 0;
    int gridWidth = 0;
    // Left edge of the first-fret number and center line of the open/muted marks.
    int fretLabelX = 0;
    int markY = 0;
  };

  void build(const Song& song, const SongChords& chords, uint8_t strings, const TextMeasurer& measurer,
             Viewport viewport);

  FontSize size() const { return fontSize; }
  const Metrics& metrics() const { return sizes; }
  int cellWidth() const { return width; }
  int cellHeight() const { return height; }
  int columns() const { return columnCount; }
  size_t perPage() const { return static_cast<size_t>(columnCount) * rowCount; }
  uint16_t pageCount() const { return pages; }
  Placement place(size_t index) const;
  // Vertical center of grid row (0 = the row under the top line) at placement.
  int rowCenterY(const Placement& at, const int row) const {
    return at.gridY + row * sizes.fretGap + sizes.fretGap / 2;
  }

 private:
  bool tryBuild(FontSize size, const Song& song, const SongChords& chords, const TextMeasurer& measurer);

  FontSize fontSize = FontSize::Large;
  Metrics sizes;
  Viewport viewport;
  uint8_t stringCount = 0;
  int width = 0;
  int height = 0;
  int columnCount = 1;
  int rowCount = 1;
  int originX = 0;
  uint16_t pages = 1;
};

}  // namespace songbook
