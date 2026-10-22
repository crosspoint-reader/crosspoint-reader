#include "SongLayout.h"

#include <algorithm>
#include <cstdio>

namespace songbook {

namespace {

std::string_view trimTrailingSpaces(std::string_view text) {
  while (!text.empty() && text.back() == ' ') text.remove_suffix(1);
  return text;
}

bool sameChords(const Song& song, const SongLine& a, const SongLine& b) {
  if (a.chordCount != b.chordCount) return false;
  for (uint16_t i = 0; i < a.chordCount; ++i) {
    if (song.view(song.chords[a.firstChord + i].name) != song.view(song.chords[b.firstChord + i].name)) return false;
  }
  return true;
}

bool isChordLine(const SongLine& line) { return line.kind == LineKind::Lyrics && line.chordCount > 0; }

// True when both sections play the same non-empty sequence of chord lines.
bool sameProgression(const Song& song, const Section& a, const Section& b) {
  uint16_t ia = a.firstLine;
  uint16_t ib = b.firstLine;
  const uint16_t endA = a.firstLine + a.lineCount;
  const uint16_t endB = b.firstLine + b.lineCount;
  bool any = false;
  while (true) {
    while (ia < endA && !isChordLine(song.lines[ia])) ++ia;
    while (ib < endB && !isChordLine(song.lines[ib])) ++ib;
    if (ia == endA || ib == endB) return any && ia == endA && ib == endB;
    if (!sameChords(song, song.lines[ia], song.lines[ib])) return false;
    any = true;
    ++ia;
    ++ib;
  }
}

bool collapsesRepeats(const SectionType type) {
  switch (type) {
    case SectionType::Verse:
    case SectionType::Chorus:
    case SectionType::Bridge:
    case SectionType::Intro:
    case SectionType::Outro:
      return true;
    case SectionType::None:
    case SectionType::Tab:
    case SectionType::Grid:
    case SectionType::Other:
      return false;
  }
  return false;
}

// Bytes of the lyric lines a layout prints, with {chorus} repeats counted again.
struct PrintedText {
  size_t lines = 0;
  size_t bytes = 0;
  size_t chords = 0;
};

PrintedText printedText(const Song& song) {
  PrintedText total;
  for (const Section& section : song.sections) {
    const Section* source =
        section.repeat ? (section.repeatOf >= 0 ? &song.sections[section.repeatOf] : nullptr) : &section;
    if (!source) continue;
    for (uint16_t i = 0; i < source->lineCount; ++i) {
      const SongLine& line = song.lines[source->firstLine + i];
      ++total.lines;
      total.bytes += line.lyric.length;
      total.chords += line.chordCount;
    }
  }
  return total;
}

// Lines this long wrap at least once per this many bytes on supported panels.
constexpr size_t BYTES_PER_WRAPPED_ROW = 16;
constexpr size_t ATOM_CAPACITY = 256;
constexpr size_t ATOM_BYTES = 8;
// Room per section for a generated label such as "Verse 12 (same chords)".
constexpr size_t LABEL_BYTES_PER_SECTION = 64;
constexpr size_t MAX_TABLE_ENTRIES = UINT16_MAX;

LayoutBudget clampBudget(LayoutBudget budget) {
  budget.rows = std::min(budget.rows, MAX_TABLE_ENTRIES);
  budget.pieces = std::min(budget.pieces, MAX_TABLE_ENTRIES);
  budget.ownBytes = std::min(budget.ownBytes, MAX_TABLE_ENTRIES);
  return budget;
}

// Wrapped label rows: a generated label may take two rows in a narrow
// column, and a long label from the file one more per 8 bytes.
size_t labelRows(const Song& song) {
  size_t rows = 2 * song.sections.size();
  for (const Section& section : song.sections) rows += section.label.length / 8;
  for (const ChartRow& row : song.chart) rows += row.label.length / 8 + 1;
  return rows;
}

// Labels copied from the file plus a generated label for every section.
size_t labelBytes(const Song& song) {
  size_t total = 128;
  for (const Section& section : song.sections) total += section.label.length + LABEL_BYTES_PER_SECTION;
  return total;
}

}  // namespace

size_t LayoutBudget::bytes() const {
  size_t total = 64 * sizeof(int32_t);
  for (const size_t table : tables()) total += table;
  return total;
}

std::array<size_t, 4> LayoutBudget::tables() const {
  return {rows * sizeof(Row), pieces * sizeof(Piece), ownBytes, atoms * ATOM_BYTES};
}

// Every lyric piece starts a row or follows a chord, so pieces <= rows + 2 * chords.
LayoutBudget SongLayout::lyricsBudget(const Song& song) {
  const PrintedText printed = printedText(song);
  LayoutBudget budget;
  // Chord-dense lines wrap on chord width, not lyric bytes.
  budget.rows = labelRows(song) + printed.lines + printed.bytes / BYTES_PER_WRAPPED_ROW + printed.chords / 4 + 8;
  budget.pieces = budget.rows + 2 * printed.chords + 8;
  budget.ownBytes = labelBytes(song);
  budget.atoms = ATOM_CAPACITY;
  return clampBudget(budget);
}

// Chart rows hold labels, chord runs (at least two chords each), comments, and grid text.
LayoutBudget SongLayout::chartBudget(const Song& song) {
  size_t chartBytes = 0;
  for (const ChartRow& row : song.chart) chartBytes += row.chords.length;
  size_t textBytes = 0;
  for (const SongLine& line : song.lines) {
    if (line.kind != LineKind::Lyrics) textBytes += line.lyric.length;
  }
  LayoutBudget budget;
  budget.rows = labelRows(song) + song.lines.size() + song.chords.size() / 2 + 2 * song.chart.size() +
                (chartBytes + textBytes) / 8 + 8;
  budget.pieces = 2 * budget.rows + song.chords.size() + 8;
  // Chart labels may add "(same chords)" plus the "×N" marks of collapsed lines.
  budget.ownBytes = labelBytes(song) + song.lines.size() * 8;
  return clampBudget(budget);
}

std::string_view SongLayout::text(const Song& song, const Piece& piece) const {
  return refText(song, TextRef{piece.offset, piece.length}, piece.own);
}

std::string_view SongLayout::refText(const Song& song, const TextRef ref, const bool own) const {
  if (own) return std::string_view(ownText).substr(ref.offset, ref.length);
  return song.view(ref);
}

void SongLayout::reserve(const LayoutBudget& budget) {
  static_assert(sizeof(Atom) <= ATOM_BYTES, "LayoutBudget undercounts atoms");
  rowList.reserve(budget.rows);
  pieceList.reserve(budget.pieces);
  ownText.reserve(budget.ownBytes);
  atoms.reserve(budget.atoms);
  stops.reserve(64);
}

void SongLayout::clear() {
  rowList.clear();
  pieceList.clear();
  stops.clear();
  ownText.clear();
  height = 0;
  pages = 1;
  columnCount = 1;
  rowOpen = false;
  full = false;
}

void SongLayout::beginRow(const int32_t y, const bool chorusBar) {
  rowOpen = false;
  if (full || rowList.size() >= rowList.capacity()) {
    full = true;
    return;
  }
  Row row;
  row.y = y;
  row.chorusBar = chorusBar;
  row.firstPiece = static_cast<uint16_t>(pieceList.size());
  rowList.push_back(row);
  rowOpen = true;
}

void SongLayout::endRow(const uint16_t rowHeight) {
  if (!rowOpen) return;
  rowOpen = false;
  Row& row = rowList.back();
  row.height = rowHeight;
  row.pieceCount = static_cast<uint16_t>(pieceList.size() - row.firstPiece);
}

void SongLayout::addPiece(const int x, const int dy, const TextRef ref, const bool own, const TextRole role,
                          const FontSize pieceSize) {
  if (ref.length == 0 || !rowOpen) return;
  if (pieceList.size() >= pieceList.capacity()) {
    full = true;
    return;
  }
  Piece piece;
  piece.x = static_cast<int16_t>(x);
  piece.dy = static_cast<int16_t>(dy);
  piece.offset = static_cast<uint16_t>(ref.offset);
  piece.length = ref.length;
  piece.role = role;
  piece.size = pieceSize;
  piece.own = own;
  pieceList.push_back(piece);
}

TextRef SongLayout::addOwnText(const std::string_view value) {
  TextRef ref;
  // A label that does not fit is skipped; the rows around it are kept.
  if (ownText.size() + value.size() > ownText.capacity()) return ref;
  ref.offset = static_cast<uint32_t>(ownText.size());
  ref.length = static_cast<uint16_t>(value.size());
  ownText.append(value.data(), value.size());
  return ref;
}

void SongLayout::sectionLabel(const Song& song, const Section& section, const int ordinal, const SectionNames& names,
                              std::string& out) const {
  out.clear();
  if (section.label.length > 0) {
    out.assign(song.view(section.label));
    return;
  }
  const char* name = nullptr;
  switch (section.type) {
    case SectionType::Verse:
      name = names.verse;
      break;
    case SectionType::Chorus:
      name = names.chorus;
      break;
    case SectionType::Bridge:
      name = names.bridge;
      break;
    case SectionType::Intro:
      name = names.intro;
      break;
    case SectionType::Outro:
      name = names.outro;
      break;
    case SectionType::Tab:
      name = names.tab;
      break;
    case SectionType::Grid:
      name = names.grid;
      break;
    case SectionType::None:
    case SectionType::Other:
      return;
  }
  out.assign(name);
  if (ordinal > 0) {
    char number[8];
    snprintf(number, sizeof(number), " %d", ordinal);
    out.append(number);
  }
}

void SongLayout::placeAtoms(const Song& song, const SongLine& line, const size_t first, const size_t last,
                            const TextMeasurer& measurer, PlaceState& state, const bool commit, bool& rowHasChord,
                            bool& rowHasLyric) {
  const std::string_view lyric = song.view(line.lyric);
  const int chordGap = measurer.width(TextRole::Chord, FontSize::Large, " ");
  for (size_t k = first; k < last; ++k) {
    const Atom& atom = atoms[k];
    if (atom.chord >= 0) {
      const ChordAnchor& anchor = song.chords[line.firstChord + atom.chord];
      const int chordWidth = measurer.width(TextRole::Chord, FontSize::Large, song.view(anchor.name));
      // A chord never overlaps the previous one; the lyric moves right with it.
      const int chordX = std::max(state.x, state.chordEnd);
      if (commit) addPiece(chordX, 0, anchor.name, false, TextRole::Chord, FontSize::Large);
      state.chordEnd = chordX + chordWidth + chordGap;
      state.x = chordX;
      state.lastLyricPiece = -1;
      state.extent = std::max(state.extent, chordX + chordWidth);
      rowHasChord = true;
    }
    const std::string_view run = lyric.substr(atom.start, atom.end - atom.start);
    const std::string_view visible = trimTrailingSpaces(run);
    if (!visible.empty()) {
      const int visibleWidth = measurer.width(TextRole::Lyric, FontSize::Large, visible);
      const uint32_t start = line.lyric.offset + atom.start;
      if (commit) {
        // Chordless words continue the previous run, so a piece usually spans
        // from one chord to the next rather than one word.
        if (state.lastLyricPiece >= 0 && state.lastLyricEnd == start) {
          Piece& piece = pieceList[state.lastLyricPiece];
          piece.length = static_cast<uint16_t>(start + visible.size() - piece.offset);
        } else {
          const size_t before = pieceList.size();
          addPiece(state.x, 0, TextRef{start, static_cast<uint16_t>(visible.size())}, false, TextRole::Lyric,
                   FontSize::Large);
          state.lastLyricPiece = pieceList.size() > before ? static_cast<int>(before) : -1;
        }
        state.lastLyricEnd = start + static_cast<uint32_t>(run.size());
      }
      state.extent = std::max(state.extent, state.x + visibleWidth);
      rowHasLyric = true;
      state.x += visible.size() == run.size() ? visibleWidth : measurer.width(TextRole::Lyric, FontSize::Large, run);
    } else if (!run.empty()) {
      state.x += measurer.width(TextRole::Lyric, FontSize::Large, run);
    }
  }
}

int SongLayout::finishLyricRow(const TextMeasurer& measurer, const bool hasChord, const bool hasLyric) {
  if (!rowOpen) return 0;
  const int chordHeight = hasChord ? measurer.lineHeight(TextRole::Chord, FontSize::Large) : 0;
  const int lyricHeight = hasLyric ? measurer.lineHeight(TextRole::Lyric, FontSize::Large) : 0;
  Row& row = rowList.back();
  for (size_t i = row.firstPiece; i < pieceList.size(); ++i) {
    if (pieceList[i].role == TextRole::Lyric) pieceList[i].dy = static_cast<int16_t>(chordHeight);
  }
  if (chordHeight + lyricHeight == 0) {
    rowList.pop_back();
    rowOpen = false;
    return 0;
  }
  endRow(static_cast<uint16_t>(chordHeight + lyricHeight));
  return chordHeight + lyricHeight;
}

int32_t SongLayout::layoutLyricLine(const Song& song, const SongLine& line, const TextMeasurer& measurer,
                                    const int width, const int indent, int32_t y, const bool chorus) {
  const std::string_view lyric = song.view(line.lyric);
  const size_t length = lyric.size();
  atoms.clear();
  size_t pos = 0;
  uint16_t chord = 0;
  while (pos < length || chord < line.chordCount) {
    const size_t start = pos;
    int16_t atomChord = -1;
    if (chord < line.chordCount && song.chords[line.firstChord + chord].position <= pos) {
      atomChord = static_cast<int16_t>(chord++);
    }
    const size_t nextChord =
        chord < line.chordCount ? std::min<size_t>(song.chords[line.firstChord + chord].position, length) : length;
    size_t end = pos;
    while (end < length && lyric[end] != ' ' && end < nextChord) ++end;
    while (end < length && lyric[end] == ' ' && end < nextChord) ++end;
    if (end == start && atomChord < 0) break;
    if (atoms.size() >= atoms.capacity()) {
      // A pathological line keeps its text as one unbreakable run.
      if (!atoms.empty()) atoms.back().end = static_cast<uint16_t>(length);
      full = true;
      break;
    }
    atoms.push_back(Atom{static_cast<uint16_t>(start), static_cast<uint16_t>(end), atomChord,
                         start == 0 || lyric[start - 1] == ' '});
    pos = end;
  }

  PlaceState state{indent, indent, indent, -1, 0};
  bool hasChord = false;
  bool hasLyric = false;
  beginRow(y, chorus);
  size_t first = 0;
  while (first < atoms.size()) {
    // A word with mid-word chords is placed as one unit.
    size_t last = first + 1;
    while (last < atoms.size() && !atoms[last].breakBefore) ++last;
    PlaceState trial = state;
    bool trialChord = false;
    bool trialLyric = false;
    placeAtoms(song, line, first, last, measurer, trial, false, trialChord, trialLyric);
    if (trial.extent > width && state.extent > indent) {
      y += finishLyricRow(measurer, hasChord, hasLyric);
      beginRow(y, chorus);
      state = PlaceState{indent, indent, indent, -1, 0};
      hasChord = false;
      hasLyric = false;
    }
    placeAtoms(song, line, first, last, measurer, state, true, hasChord, hasLyric);
    first = last;
  }
  return y + finishLyricRow(measurer, hasChord, hasLyric);
}

int32_t SongLayout::layoutWrapped(const std::string_view value, const TextRef ref, const bool own, const TextRole role,
                                  const FontSize pieceSize, const TextMeasurer& measurer, const int width,
                                  const int indent, int32_t y, const bool chorus) {
  const int lineHeight = measurer.lineHeight(role, pieceSize);
  const size_t length = value.size();
  size_t pos = 0;
  while (pos < length && value[pos] == ' ') ++pos;
  while (pos < length) {
    const size_t lineStart = pos;
    size_t fitEnd = lineStart;
    size_t cursor = lineStart;
    while (cursor < length) {
      size_t wordEnd = cursor;
      while (wordEnd < length && value[wordEnd] != ' ') ++wordEnd;
      const int candidate = measurer.width(role, pieceSize, value.substr(lineStart, wordEnd - lineStart));
      if (indent + candidate > width && fitEnd > lineStart) break;
      fitEnd = wordEnd;
      cursor = wordEnd;
      while (cursor < length && value[cursor] == ' ') ++cursor;
    }
    beginRow(y, chorus);
    addPiece(indent, 0,
             TextRef{ref.offset + static_cast<uint32_t>(lineStart), static_cast<uint16_t>(fitEnd - lineStart)}, own,
             role, pieceSize);
    endRow(static_cast<uint16_t>(lineHeight));
    y += lineHeight;
    pos = fitEnd;
    while (pos < length && value[pos] == ' ') ++pos;
  }
  return y;
}

void SongLayout::buildLyrics(const Song& song, const TextMeasurer& measurer, const Viewport viewport,
                             const SectionNames& names) {
  clear();
  reserve(lyricsBudget(song));
  size = FontSize::Large;
  const int lyricHeight = measurer.lineHeight(TextRole::Lyric, FontSize::Large);
  const int verseCount =
      static_cast<int>(std::count_if(song.sections.begin(), song.sections.end(), [](const Section& s) {
        return s.type == SectionType::Verse && s.label.length == 0;
      }));
  int verseOrdinal = 0;
  std::string label;
  int32_t y = 0;
  for (const Section& section : song.sections) {
    const int ordinal = section.type == SectionType::Verse && section.label.length == 0 ? ++verseOrdinal : 0;
    if (y > 0) y += lyricHeight / 2;
    sectionLabel(song, section, verseCount > 1 ? ordinal : 0, names, label);
    const Section* source = &section;
    if (section.repeat) {
      // The repeated chorus is printed again so the scroll keeps a steady pace.
      source = section.repeatOf >= 0 ? &song.sections[section.repeatOf] : nullptr;
      if (label.empty()) label.assign(names.chorus);
      if (!source) label.append(" ").append(names.repeat);
    }
    if (!label.empty()) {
      const TextRef ref = addOwnText(label);
      y = layoutWrapped(refText(song, ref, true), ref, true, TextRole::Label, FontSize::Large, measurer, viewport.width,
                        0, y, false);
    }
    if (!source) continue;
    const bool chorus = section.type == SectionType::Chorus;
    const int indent = chorus ? CHORUS_INDENT : 0;
    for (uint16_t i = 0; i < source->lineCount; ++i) {
      const SongLine& line = song.lines[source->firstLine + i];
      switch (line.kind) {
        case LineKind::Lyrics:
          y = layoutLyricLine(song, line, measurer, viewport.width, indent, y, chorus);
          break;
        case LineKind::Comment:
          y = layoutWrapped(song.view(line.lyric), line.lyric, false, TextRole::Comment, FontSize::Large, measurer,
                            viewport.width, indent, y, chorus);
          break;
        case LineKind::Tab:
          beginRow(y, chorus);
          addPiece(indent, 0, line.lyric, false, TextRole::Lyric, FontSize::Large);
          endRow(static_cast<uint16_t>(lyricHeight));
          y += lyricHeight;
          break;
        case LineKind::Empty:
          y += lyricHeight / 2;
          break;
      }
    }
  }
  height = y;
  computeScrollStops(rowList, height, viewport.height, stops);
}

void SongLayout::computeScrollStops(const std::vector<Row>& rows, const int32_t contentHeight, const int viewHeight,
                                    std::vector<int32_t>& out) {
  out.clear();
  out.push_back(0);
  if (rows.empty() || viewHeight <= 0 || contentHeight <= viewHeight) return;
  // The last top must show the final row whole.
  int32_t maxTop = rows.back().y;
  for (const Row& row : rows) {
    if (contentHeight - row.y <= viewHeight) {
      maxTop = row.y;
      break;
    }
  }
  const int32_t step = std::max(1, viewHeight / 3);
  int32_t current = 0;
  while (current < maxTop) {
    const int32_t target = current + step;
    int32_t next = -1;
    for (const Row& row : rows) {
      if (row.y <= current) continue;
      if (row.y <= target) {
        next = row.y;
      } else {
        // No row starts inside this step (a tall row): move to the next row.
        if (next < 0) next = row.y;
        break;
      }
    }
    if (next < 0 || next > maxTop) next = maxTop;
    out.push_back(next);
    current = next;
  }
}

int32_t SongLayout::layoutChartChords(const Song& song, const SongLine& line, const int repeatCount,
                                      const TextMeasurer& measurer, const FontSize pieceSize, const int width,
                                      int32_t y) {
  const int lineHeight = measurer.lineHeight(TextRole::Chord, pieceSize);
  const int gap = measurer.width(TextRole::Chord, pieceSize, "  ");
  int x = 0;
  beginRow(y, false);
  const auto place = [&](const TextRef ref, const bool own, const TextRole role) {
    const int pieceWidth = measurer.width(role, pieceSize, refText(song, ref, own));
    if (x > 0 && x + pieceWidth > width) {
      endRow(static_cast<uint16_t>(lineHeight));
      y += lineHeight;
      beginRow(y, false);
      x = 0;
    }
    addPiece(x, 0, ref, own, role, pieceSize);
    x += pieceWidth + gap;
  };
  for (uint16_t i = 0; i < line.chordCount; ++i) place(song.chords[line.firstChord + i].name, false, TextRole::Chord);
  if (repeatCount > 1) {
    char repeat[12];
    snprintf(repeat, sizeof(repeat), "\xC3\x97%d", repeatCount);  // "×N"
    place(addOwnText(repeat), true, TextRole::Comment);
  }
  endRow(static_cast<uint16_t>(lineHeight));
  return y + lineHeight;
}

void SongLayout::layoutChartAt(const Song& song, const TextMeasurer& measurer, const FontSize pieceSize,
                               const int width, const SectionNames& names, std::vector<uint32_t>& groupStarts) {
  clear();
  groupStarts.clear();
  size = pieceSize;
  chartGap = measurer.lineHeight(TextRole::Chord, pieceSize) / 3;
  int32_t y = 0;
  const auto startGroup = [&] {
    if (!rowList.empty()) y += chartGap;
    groupStarts.push_back(static_cast<uint32_t>(rowList.size()));
  };
  // Labels wrap to the column width so a long name never reaches the next column.
  const auto labelRow = [&](const TextRef ref, const bool own) {
    y = layoutWrapped(refText(song, ref, own), ref, own, TextRole::Label, pieceSize, measurer, width, 0, y, false);
  };

  if (!derivedChart) {
    for (const ChartRow& row : song.chart) {
      startGroup();
      if (row.label.length > 0) labelRow(row.label, false);
      y = layoutWrapped(song.view(row.chords), row.chords, false, TextRole::Chord, pieceSize, measurer, width, 0, y,
                        false);
    }
    height = y;
    return;
  }

  const int verseCount =
      static_cast<int>(std::count_if(song.sections.begin(), song.sections.end(), [](const Section& s) {
        return s.type == SectionType::Verse && s.label.length == 0;
      }));
  int verseOrdinal = 0;
  std::string label;
  for (size_t index = 0; index < song.sections.size(); ++index) {
    const Section& section = song.sections[index];
    const int ordinal = section.type == SectionType::Verse && section.label.length == 0 ? ++verseOrdinal : 0;
    if (section.type == SectionType::Tab) continue;
    sectionLabel(song, section, verseCount > 1 ? ordinal : 0, names, label);
    if (section.repeat) {
      if (label.empty()) label.assign(names.chorus);
      label.append(" ").append(names.repeat);
      startGroup();
      labelRow(addOwnText(label), true);
      continue;
    }

    bool hasContent = false;
    for (uint16_t i = 0; i < section.lineCount && !hasContent; ++i) {
      const SongLine& line = song.lines[section.firstLine + i];
      hasContent = isChordLine(line) || line.kind == LineKind::Comment || line.kind == LineKind::Tab;
    }
    if (!hasContent) continue;

    bool repeatsEarlier = false;
    if (collapsesRepeats(section.type)) {
      for (size_t prev = 0; prev < index && !repeatsEarlier; ++prev) {
        const Section& earlier = song.sections[prev];
        repeatsEarlier = earlier.type == section.type && !earlier.repeat && sameProgression(song, earlier, section);
      }
    }
    startGroup();
    if (repeatsEarlier) {
      label.append(" ").append(names.sameChords);
      labelRow(addOwnText(label), true);
      continue;
    }
    if (!label.empty()) labelRow(addOwnText(label), true);

    // Consecutive lines with the same chords collapse into one "×N" row.
    const SongLine* pending = nullptr;
    int pendingCount = 0;
    const auto flush = [&] {
      if (pending) y = layoutChartChords(song, *pending, pendingCount, measurer, pieceSize, width, y);
      pending = nullptr;
      pendingCount = 0;
    };
    for (uint16_t i = 0; i < section.lineCount; ++i) {
      const SongLine& line = song.lines[section.firstLine + i];
      if (isChordLine(line)) {
        if (pending && sameChords(song, *pending, line)) {
          ++pendingCount;
        } else {
          flush();
          pending = &line;
          pendingCount = 1;
        }
      } else if (line.kind == LineKind::Comment) {
        flush();
        y = layoutWrapped(song.view(line.lyric), line.lyric, false, TextRole::Comment, pieceSize, measurer, width, 0, y,
                          false);
      } else if (line.kind == LineKind::Tab) {
        flush();
        y = layoutWrapped(song.view(line.lyric), line.lyric, false, TextRole::Chord, pieceSize, measurer, width, 0, y,
                          false);
      }
    }
    flush();
  }
  height = y;
}

bool SongLayout::packColumns(const std::vector<uint32_t>& groupStarts, const int columns, const int columnWidth,
                             const int viewHeight) {
  int32_t columnY = 0;
  int32_t maxY = 0;
  int column = 0;
  for (size_t group = 0; group < groupStarts.size(); ++group) {
    const uint32_t first = groupStarts[group];
    const uint32_t last = group + 1 < groupStarts.size() ? groupStarts[group + 1] : rowList.size();
    if (first >= last) continue;
    const int32_t top = rowList[first].y;
    const int32_t groupHeight = rowList[last - 1].y + rowList[last - 1].height - top;
    if (groupHeight > viewHeight) return false;
    int32_t gap = columnY > 0 ? chartGap : 0;
    if (columnY + gap + groupHeight > viewHeight) {
      if (++column >= columns) return false;
      columnY = 0;
      gap = 0;
    }
    const int32_t base = columnY + gap;
    for (uint32_t r = first; r < last; ++r) {
      rowList[r].y = base + rowList[r].y - top;
      rowList[r].x = static_cast<int16_t>(column * (columnWidth + COLUMN_GAP));
    }
    columnY = base + groupHeight;
    maxY = std::max(maxY, columnY);
  }
  height = maxY;
  pages = 1;
  columnCount = static_cast<uint8_t>(columns);
  return true;
}

void SongLayout::paginate(const std::vector<uint32_t>& groupStarts, const int viewHeight) {
  uint16_t page = 0;
  int32_t pageY = 0;
  for (size_t group = 0; group < groupStarts.size(); ++group) {
    const uint32_t first = groupStarts[group];
    const uint32_t last = group + 1 < groupStarts.size() ? groupStarts[group + 1] : rowList.size();
    if (first >= last) continue;
    const int32_t top = rowList[first].y;
    const int32_t groupHeight = rowList[last - 1].y + rowList[last - 1].height - top;
    int32_t gap = pageY > 0 ? chartGap : 0;
    // Keep a section on one page when it fits on a fresh one.
    if (pageY > 0 && pageY + gap + groupHeight > viewHeight && groupHeight <= viewHeight) {
      ++page;
      pageY = 0;
      gap = 0;
    }
    int32_t previousEnd = top;
    for (uint32_t r = first; r < last; ++r) {
      Row& row = rowList[r];
      int32_t delta = r == first ? gap : row.y - previousEnd;
      previousEnd = row.y + row.height;
      if (pageY > 0 && pageY + delta + row.height > viewHeight) {
        ++page;
        pageY = 0;
        delta = 0;
      }
      row.y = pageY + delta;
      row.x = 0;
      row.page = page;
      pageY = row.y + row.height;
    }
  }
  pages = static_cast<uint16_t>(page + 1);
  columnCount = 1;
  height = pages == 1 ? pageY : viewHeight;
}

void SongLayout::buildChart(const Song& song, const TextMeasurer& measurer, const Viewport viewport,
                            const SectionNames& names) {
  derivedChart = song.chart.empty();
  // Every attempt below reuses this one reserve: clear() keeps capacity.
  reserve(chartBudget(song));
  std::vector<uint32_t> groupStarts;
  groupStarts.reserve(song.sections.size() + song.chart.size() + 1);
  static constexpr FontSize SIZES[] = {FontSize::Large, FontSize::Medium, FontSize::Small};
  for (const FontSize candidate : SIZES) {
    for (int columns = 1; columns <= 2; ++columns) {
      const int columnWidth = columns == 1 ? viewport.width : (viewport.width - COLUMN_GAP) / 2;
      if (columnWidth <= 0) continue;
      layoutChartAt(song, measurer, candidate, columnWidth, names, groupStarts);
      // An attempt that ran out of budget is missing rows; a smaller size may fit them all.
      if (!full && packColumns(groupStarts, columns, columnWidth, viewport.height)) return;
    }
  }
  layoutChartAt(song, measurer, FontSize::Small, viewport.width, names, groupStarts);
  paginate(groupStarts, viewport.height);
}

}  // namespace songbook
