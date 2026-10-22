#include "ChordProParser.h"

#include <initializer_list>

namespace songbook {

namespace {

constexpr std::string_view UTF8_BOM = "\xEF\xBB\xBF";

std::string_view trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
  return text;
}

char lowerAscii(const char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool equalsIgnoreCase(const std::string_view a, const std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (lowerAscii(a[i]) != lowerAscii(b[i])) return false;
  }
  return true;
}

bool startsWithIgnoreCase(const std::string_view text, const std::string_view prefix) {
  return text.size() >= prefix.size() && equalsIgnoreCase(text.substr(0, prefix.size()), prefix);
}

bool isAny(const std::string_view name, const std::initializer_list<std::string_view> names) {
  for (const auto candidate : names) {
    if (equalsIgnoreCase(name, candidate)) return true;
  }
  return false;
}

uint32_t parseUnsigned(const std::string_view text, bool& ok) {
  uint32_t value = 0;
  ok = !text.empty();
  for (const char c : text) {
    if (c < '0' || c > '9' || value > 100000000) {
      ok = false;
      return 0;
    }
    value = value * 10 + static_cast<uint32_t>(c - '0');
  }
  return value;
}

// ChordPro 6 accepts label="Text" as well as a bare label value.
std::string_view sectionLabel(std::string_view value) {
  value = trim(value);
  if (!startsWithIgnoreCase(value, "label=")) return value;
  value.remove_prefix(6);
  if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'')) {
    const size_t close = value.find(value.front(), 1);
    return close == std::string_view::npos ? value.substr(1) : value.substr(1, close - 1);
  }
  return value;
}

SectionType sectionTypeFor(const std::string_view kind) {
  if (equalsIgnoreCase(kind, "verse")) return SectionType::Verse;
  if (equalsIgnoreCase(kind, "chorus")) return SectionType::Chorus;
  if (equalsIgnoreCase(kind, "bridge")) return SectionType::Bridge;
  if (equalsIgnoreCase(kind, "tab")) return SectionType::Tab;
  if (equalsIgnoreCase(kind, "grid")) return SectionType::Grid;
  if (equalsIgnoreCase(kind, "intro")) return SectionType::Intro;
  if (equalsIgnoreCase(kind, "outro")) return SectionType::Outro;
  return SectionType::Other;
}

}  // namespace

ChordProParser::ChordProParser(Song& song, const bool metadataOnly) : song(song), metadataOnly(metadataOnly) {}

uint32_t ChordProParser::parseDuration(std::string_view value) {
  value = trim(value);
  uint32_t total = 0;
  int fields = 0;
  while (!value.empty()) {
    const size_t colon = value.find(':');
    const std::string_view field = value.substr(0, colon);
    bool ok = false;
    const uint32_t number = parseUnsigned(trim(field), ok);
    if (!ok || ++fields > 3) return 0;
    total = total * 60 + number;
    if (colon == std::string_view::npos) break;
    value.remove_prefix(colon + 1);
    if (value.empty()) return 0;
  }
  return total;
}

bool ChordProParser::appendText(const std::string_view bytes, TextRef& ref) {
  if (song.text.size() + bytes.size() > MAX_TEXT_BYTES) {
    truncatedContent = true;
    return false;
  }
  ref.offset = static_cast<uint32_t>(song.text.size());
  ref.length = static_cast<uint16_t>(bytes.size());
  song.text.append(bytes.data(), bytes.size());
  return true;
}

bool ChordProParser::feedLine(std::string_view line) {
  if (firstLine) {
    firstLine = false;
    if (line.substr(0, UTF8_BOM.size()) == UTF8_BOM) line.remove_prefix(UTF8_BOM.size());
  }
  if (line.size() > MAX_LINE_BYTES) {
    truncatedContent = true;
    line = line.substr(0, MAX_LINE_BYTES);
  }
  const std::string_view trimmed = trim(line);

  // '#' lines are ChordPro source comments, never rendered.
  if (!trimmed.empty() && trimmed.front() == '#') return true;

  if (trimmed.size() >= 2 && trimmed.front() == '{' && trimmed.back() == '}') {
    const std::string_view inner = trim(trimmed.substr(1, trimmed.size() - 2));
    size_t split = 0;
    while (split < inner.size() && inner[split] != ':' && inner[split] != ' ' && inner[split] != '\t') ++split;
    const std::string_view name = inner.substr(0, split);
    std::string_view value = split < inner.size() ? inner.substr(split + 1) : std::string_view{};
    value = trim(value);
    const bool opensBody = (startsWithIgnoreCase(name, "start_of_") && !equalsIgnoreCase(name, "start_of_x_chart")) ||
                           isAny(name, {"sov", "soc", "sob", "sot", "sog"});
    if (metadataOnly && opensBody) return false;
    handleDirective(name, value);
    return true;
  }

  if (inChart) {
    // Song lists read only metadata; the chart is body content.
    if (!metadataOnly) handleChartLine(trimmed);
    return true;
  }
  if (metadataOnly) return trimmed.empty();

  if (trimmed.empty()) {
    if (sectionOpen) addTextLine(LineKind::Empty, {});
    return true;
  }

  const bool tabLike =
      sectionOpen && !song.sections.empty() &&
      (song.sections.back().type == SectionType::Tab || song.sections.back().type == SectionType::Grid);
  if (tabLike) {
    // Keep tab/grid alignment; only the line terminator is removed.
    std::string_view raw = line;
    while (!raw.empty() && (raw.back() == '\r' || raw.back() == ' ')) raw.remove_suffix(1);
    addTextLine(LineKind::Tab, raw);
    return true;
  }
  handleLyricLine(trimmed);
  return true;
}

void ChordProParser::handleDirective(const std::string_view name, const std::string_view value) {
  if (isAny(name, {"title", "t"})) {
    if (song.meta.title.empty()) song.meta.title.assign(value.data(), value.size());
  } else if (isAny(name, {"artist", "key", "capo", "tempo", "duration"})) {
    handleMeta(name, value);
  } else if (equalsIgnoreCase(name, "meta")) {
    size_t split = 0;
    while (split < value.size() && value[split] != ' ' && value[split] != '\t') ++split;
    handleMeta(value.substr(0, split), trim(value.substr(split)));
  } else if (metadataOnly) {
    // Section and comment directives carry no song-list metadata.
  } else if (isAny(name, {"comment", "c", "comment_italic", "ci", "comment_box", "cb", "highlight"})) {
    if (inChart) return;
    if (!value.empty() && ensureImplicitSection()) addTextLine(LineKind::Comment, value);
  } else if (equalsIgnoreCase(name, "start_of_x_chart")) {
    inChart = true;
  } else if (equalsIgnoreCase(name, "end_of_x_chart")) {
    inChart = false;
  } else if (equalsIgnoreCase(name, "chorus")) {
    addRepeat(sectionLabel(value));
  } else if (isAny(name, {"sov", "soc", "sob", "sot", "sog"})) {
    static constexpr SectionType TYPES[] = {SectionType::Verse, SectionType::Chorus, SectionType::Bridge,
                                            SectionType::Tab, SectionType::Grid};
    static constexpr const char* NAMES[] = {"sov", "soc", "sob", "sot", "sog"};
    for (size_t i = 0; i < 5; ++i) {
      if (equalsIgnoreCase(name, NAMES[i])) openSection(TYPES[i], sectionLabel(value));
    }
  } else if (isAny(name, {"eov", "eoc", "eob", "eot", "eog"})) {
    closeSection();
  } else if (startsWithIgnoreCase(name, "start_of_")) {
    const std::string_view kind = name.substr(9);
    const SectionType type = sectionTypeFor(kind);
    const std::string_view label = sectionLabel(value);
    // Generic sections without a label show their own name (e.g. "solo").
    openSection(type, label.empty() && type == SectionType::Other ? kind : label);
  } else if (startsWithIgnoreCase(name, "end_of_")) {
    closeSection();
  } else {
    ++skipped;
  }
}

void ChordProParser::handleMeta(const std::string_view name, const std::string_view value) {
  auto& meta = song.meta;
  if (equalsIgnoreCase(name, "artist")) {
    if (meta.artist.empty()) meta.artist.assign(value.data(), value.size());
  } else if (equalsIgnoreCase(name, "key")) {
    meta.key.assign(value.data(), value.size());
  } else if (equalsIgnoreCase(name, "capo")) {
    meta.capo.assign(value.data(), value.size());
  } else if (equalsIgnoreCase(name, "tempo")) {
    bool ok = false;
    const uint32_t tempo = parseUnsigned(value, ok);
    meta.tempo = ok && tempo <= 1000 ? static_cast<uint16_t>(tempo) : 0;
  } else if (equalsIgnoreCase(name, "duration")) {
    meta.durationSec = parseDuration(value);
  } else if (equalsIgnoreCase(name, "title")) {
    if (meta.title.empty()) meta.title.assign(value.data(), value.size());
  } else {
    ++skipped;
  }
}

void ChordProParser::handleChartLine(const std::string_view line) {
  if (line.empty()) return;
  if (song.chart.size() >= MAX_CHART_ROWS) {
    truncatedContent = true;
    return;
  }
  const size_t colon = line.find(':');
  const std::string_view label = colon == std::string_view::npos ? std::string_view{} : trim(line.substr(0, colon));
  const std::string_view chords = colon == std::string_view::npos ? line : trim(line.substr(colon + 1));
  ChartRow row;
  if (!appendText(label, row.label) || !appendText(chords, row.chords)) return;
  song.chart.push_back(row);
}

void ChordProParser::handleLyricLine(const std::string_view line) {
  if (!ensureImplicitSection()) return;
  if (song.lines.size() >= MAX_LINES) {
    truncatedContent = true;
    return;
  }

  // Lyric bytes stay contiguous in the pool; chord names follow them.
  SongLine songLine;
  songLine.kind = LineKind::Lyrics;
  songLine.section = static_cast<uint16_t>(song.sections.size() - 1);
  songLine.lyric.offset = static_cast<uint32_t>(song.text.size());
  size_t pendingCount = 0;
  size_t lyricBytes = 0;
  size_t i = 0;
  while (i < line.size()) {
    const char c = line[i];
    if (c == '[') {
      const size_t close = line.find(']', i + 1);
      if (close != std::string_view::npos) {
        std::string_view name = trim(line.substr(i + 1, close - i - 1));
        // [*text] is an annotation; it is shown like a chord without the marker.
        if (!name.empty() && name.front() == '*') name.remove_prefix(1);
        if (!name.empty() && name.size() <= 255) {
          if (pendingCount < MAX_CHORDS_PER_LINE) {
            pending[pendingCount++] =
                PendingChord{static_cast<uint16_t>(name.data() - line.data()), static_cast<uint8_t>(name.size()),
                             static_cast<uint16_t>(lyricBytes)};
          } else {
            truncatedContent = true;
          }
        }
        i = close + 1;
        continue;
      }
    }
    if (song.text.size() + 1 > MAX_TEXT_BYTES) {
      truncatedContent = true;
      break;
    }
    song.text.push_back(c == '\t' ? ' ' : c);
    ++lyricBytes;
    ++i;
  }
  songLine.lyric.length = static_cast<uint16_t>(lyricBytes);
  songLine.firstChord = static_cast<uint16_t>(song.chords.size());
  for (size_t k = 0; k < pendingCount; ++k) {
    if (song.chords.size() >= MAX_CHORDS) {
      truncatedContent = true;
      break;
    }
    ChordAnchor anchor;
    if (!appendText(line.substr(pending[k].nameStart, pending[k].nameLength), anchor.name)) break;
    anchor.position = pending[k].position;
    song.chords.push_back(anchor);
  }
  songLine.chordCount = static_cast<uint16_t>(song.chords.size() - songLine.firstChord);
  if (songLine.lyric.length == 0 && songLine.chordCount == 0) return;
  song.lines.push_back(songLine);
  ++song.sections.back().lineCount;
}

void ChordProParser::addTextLine(const LineKind kind, const std::string_view text) {
  if (!sectionOpen) return;
  if (song.lines.size() >= MAX_LINES) {
    truncatedContent = true;
    return;
  }
  Section& section = song.sections.back();
  if (kind == LineKind::Empty) {
    // Collapse runs of blank lines and drop a section's leading blanks.
    if (section.lineCount == 0 || song.lines.back().kind == LineKind::Empty) return;
  }
  SongLine songLine;
  songLine.kind = kind;
  songLine.section = static_cast<uint16_t>(song.sections.size() - 1);
  songLine.firstChord = static_cast<uint16_t>(song.chords.size());
  if (!text.empty() && !appendText(text, songLine.lyric)) return;
  song.lines.push_back(songLine);
  ++section.lineCount;
}

void ChordProParser::openSection(const SectionType type, const std::string_view label) {
  closeSection();
  if (song.sections.size() >= MAX_SECTIONS) {
    truncatedContent = true;
    return;
  }
  Section section;
  section.type = type;
  section.firstLine = static_cast<uint16_t>(song.lines.size());
  if (!label.empty()) appendText(label, section.label);
  song.sections.push_back(section);
  sectionOpen = true;
}

bool ChordProParser::ensureImplicitSection() {
  if (sectionOpen) return true;
  if (song.sections.size() >= MAX_SECTIONS) {
    truncatedContent = true;
    return false;
  }
  Section section;
  section.firstLine = static_cast<uint16_t>(song.lines.size());
  song.sections.push_back(section);
  sectionOpen = true;
  return true;
}

void ChordProParser::closeSection() {
  if (!sectionOpen) return;
  sectionOpen = false;
  Section& section = song.sections.back();
  while (section.lineCount > 0 && song.lines.back().kind == LineKind::Empty) {
    song.lines.pop_back();
    --section.lineCount;
  }
  if (section.lineCount == 0) song.sections.pop_back();
}

void ChordProParser::addRepeat(const std::string_view label) {
  closeSection();
  if (song.sections.size() >= MAX_SECTIONS) {
    truncatedContent = true;
    return;
  }
  Section section;
  section.type = SectionType::Chorus;
  section.firstLine = static_cast<uint16_t>(song.lines.size());
  for (size_t i = song.sections.size(); i-- > 0;) {
    if (song.sections[i].type == SectionType::Chorus && song.sections[i].repeatOf < 0) {
      section.repeatOf = static_cast<int16_t>(i);
      break;
    }
  }
  if (!label.empty()) {
    appendText(label, section.label);
  } else if (section.repeatOf >= 0) {
    section.label = song.sections[section.repeatOf].label;
  }
  // An unresolved {chorus} still marks where the chorus is sung.
  section.repeat = true;
  song.sections.push_back(section);
}

void ChordProParser::finish() {
  closeSection();
  inChart = false;
}

}  // namespace songbook
