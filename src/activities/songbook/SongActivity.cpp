#include "SongActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalMemory.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <functional>

#include "ChordProParser.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SongbookPrefs.h"
#include "activities/reader/ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"

using songbook::FontSize;
using songbook::TextRole;
using State = songbook::AutoScrollController::State;

namespace {

constexpr unsigned long HOLD_MS = ReaderUtils::SKIP_HOLD_MS;
constexpr unsigned long LEAVE_HOLD_MS = ReaderUtils::GO_BACK_OR_HOME_MS;
constexpr uint32_t NOTICE_MS = 2500;
// Used when a song has no {duration}; the header marks it with "~".
constexpr uint32_t DEFAULT_DURATION_MS = 180000;
constexpr uint32_t MAX_DURATION_SEC = 6 * 60 * 60;
constexpr int HEADER_PADDING = 4;
constexpr int CHORUS_BAR_WIDTH = 3;
constexpr int SCROLL_BAR_WIDTH = 3;
constexpr size_t SCRATCH_BYTES = songbook::ChordProParser::MAX_LINE_BYTES + 1;
// Kept free after both layouts are reserved.
constexpr size_t LAYOUT_HEADROOM_BYTES = 8 * 1024;

void fontFor(const TextRole role, const FontSize size, int& fontId, EpdFontFamily::Style& style) {
#ifdef OMIT_FONTS
  (void)size;
  fontId = NOTOSANS_14_FONT_ID;
#else
  static constexpr int BODY[] = {NOTOSANS_18_FONT_ID, NOTOSANS_16_FONT_ID, NOTOSANS_14_FONT_ID};
  static constexpr int LABEL[] = {NOTOSANS_14_FONT_ID, NOTOSANS_14_FONT_ID, NOTOSANS_12_FONT_ID};
  static constexpr int COMMENT[] = {NOTOSANS_16_FONT_ID, NOTOSANS_14_FONT_ID, NOTOSANS_12_FONT_ID};
  const auto index = static_cast<size_t>(size);
  switch (role) {
    case TextRole::Lyric:
    case TextRole::Chord:
      fontId = BODY[index];
      break;
    case TextRole::Label:
    case TextRole::Cue:
      fontId = LABEL[index];
      break;
    case TextRole::Comment:
      fontId = COMMENT[index];
      break;
  }
#endif
  switch (role) {
    case TextRole::Lyric:
    case TextRole::Cue:
      style = EpdFontFamily::REGULAR;
      break;
    case TextRole::Chord:
    case TextRole::Label:
      style = EpdFontFamily::BOLD;
      break;
    case TextRole::Comment:
      style = EpdFontFamily::ITALIC;
      break;
  }
}

songbook::SectionNames sectionNames() {
  songbook::SectionNames names;
  names.verse = tr(STR_SONGBOOK_VERSE);
  names.chorus = tr(STR_SONGBOOK_CHORUS);
  names.bridge = tr(STR_SONGBOOK_BRIDGE);
  names.intro = tr(STR_SONGBOOK_INTRO);
  names.outro = tr(STR_SONGBOOK_OUTRO);
  names.tab = tr(STR_SONGBOOK_TAB);
  names.grid = tr(STR_SONGBOOK_GRID);
  names.repeat = tr(STR_SONGBOOK_REPEAT);
  names.sameChords = tr(STR_SONGBOOK_SAME_CHORDS);
  return names;
}

// Firmware builds do not warn on a missing switch case; a new instrument needs a name here.
static_assert(songbook::INSTRUMENT_COUNT == 5);

const char* instrumentName(const songbook::Instrument instrument) {
  switch (instrument) {
    case songbook::Instrument::Guitar:
      return tr(STR_SONGBOOK_GUITAR);
    case songbook::Instrument::Ukulele:
      return tr(STR_SONGBOOK_UKULELE);
    case songbook::Instrument::Baritone:
      return tr(STR_SONGBOOK_BARITONE);
    case songbook::Instrument::Bass:
      return tr(STR_SONGBOOK_BASS);
    case songbook::Instrument::Bass5:
      return tr(STR_SONGBOOK_BASS5);
  }
  return "";
}

songbook::Instrument nextInstrument(const songbook::Instrument instrument) {
  return static_cast<songbook::Instrument>((static_cast<size_t>(instrument) + 1) % songbook::INSTRUMENT_COUNT);
}

void formatTime(char* out, const size_t size, const uint32_t ms) {
  const uint32_t seconds = ms / 1000;
  snprintf(out, size, "%u:%02u", static_cast<unsigned>(seconds / 60), static_cast<unsigned>(seconds % 60));
}

}  // namespace

SongActivity::SongActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const songbook::Setlist& setlist,
                           const size_t index)
    : Activity("SongbookSong", renderer, mappedInput), setlist(setlist), index(index) {}

int SongActivity::Measurer::width(const TextRole role, const FontSize size, const std::string_view text) const {
  int fontId = 0;
  EpdFontFamily::Style style = EpdFontFamily::REGULAR;
  fontFor(role, size, fontId, style);
  return owner.renderer.getTextAdvanceX(fontId, owner.terminated(text), style);
}

int SongActivity::Measurer::lineHeight(const TextRole role, const FontSize size) const {
  int fontId = 0;
  EpdFontFamily::Style style = EpdFontFamily::REGULAR;
  fontFor(role, size, fontId, style);
  return owner.renderer.getLineHeight(fontId);
}

const char* SongActivity::terminated(const std::string_view text) const {
  const size_t length = std::min(text.size(), SCRATCH_BYTES - 1);
  memcpy(scratch.get(), text.data(), length);
  scratch[length] = '\0';
  return scratch.get();
}

void SongActivity::onEnter() {
  Activity::onEnter();
  scratch = makeUniqueNoThrow<char[]>(SCRATCH_BYTES);
  if (!scratch) {
    LOG_ERR("SONG", "OOM: %u byte text buffer", static_cast<unsigned>(SCRATCH_BYTES));
    loadResult = songbook::SongLoadResult::OutOfMemory;
  } else {
    SONGBOOK_PREFS.ensureLoaded();
    RenderLock lock(*this);
    instrument = SONGBOOK_PREFS.instrument;
    openSong(index);
  }
  requestUpdate();
}

void SongActivity::onExit() {
  Activity::onExit();
  scroll.stop();
  song.clear();
  scratch.reset();
}

SongActivity::Geometry SongActivity::geometry() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  Geometry g{};
  g.headerY = safe.y + metrics.topPadding;
  g.headerHeight = renderer.getLineHeight(UI_12_FONT_ID) + renderer.getLineHeight(UI_10_FONT_ID) + 3 * HEADER_PADDING;
  g.bodyX = safe.x + metrics.contentSidePadding;
  g.bodyWidth = std::max(1, safe.width - 2 * metrics.contentSidePadding);
  g.bodyY = g.headerY + g.headerHeight + 2 * HEADER_PADDING;
  g.bodyHeight = std::max(1, safe.y + safe.height - g.bodyY - HEADER_PADDING);
  return g;
}

void SongActivity::openSong(const size_t songIndex) {
  index = songIndex;
  chartPage = 0;
  diagramPage = 0;
  notice = nullptr;
  // A new song gets a clean (half) refresh to clear the previous one's ghosting.
  pagesUntilFullRefresh = 0;

  // Free the previous song first so its capacity neither counts against the
  // heap checks below nor stays reserved for the rest of the set.
  song = songbook::Song{};
  lyrics = songbook::SongLayout{};
  chart = songbook::SongLayout{};

  loadResult = songbook::loadSong(setlist.songs[index], song, false);
  if (loadResult == songbook::SongLoadResult::Ok && !layoutsFit()) loadResult = songbook::SongLoadResult::OutOfMemory;
  if (loadResult != songbook::SongLoadResult::Ok) {
    LOG_ERR("SONG", "Cannot open %s (%d)", setlist.songs[index].c_str(), static_cast<int>(loadResult));
    std::string title = song.meta.title.empty() ? setlist.songs[index] : song.meta.title;
    song = songbook::Song{};
    song.meta.title = std::move(title);
  }

  const Geometry g = geometry();
  const songbook::Viewport viewport{g.bodyWidth, g.bodyHeight};
  const Measurer measurer(*this);
  const songbook::SectionNames names = sectionNames();
  lyrics.buildLyrics(song, measurer, viewport, names);
  chart.buildChart(song, measurer, viewport, names);
  if (lyrics.truncated() || chart.truncated()) LOG_ERR("SONG", "Layout truncated: %s", setlist.songs[index].c_str());
  songChords.collect(song);
  if (songChords.truncated()) {
    LOG_ERR("SONG", "Over %u chords: %s", static_cast<unsigned>(songbook::SongChords::MAX_CHORDS),
            setlist.songs[index].c_str());
  }
  buildDiagrams();

  durationEstimated = song.meta.durationSec == 0;
  const uint32_t durationMs =
      durationEstimated ? DEFAULT_DURATION_MS : std::min(song.meta.durationSec, MAX_DURATION_SEC) * 1000;
  scroll.configure(lyrics.scrollStops(), lyrics.contentHeight(), viewport.height, durationMs);

  char position[16];
  snprintf(position, sizeof(position), "%u/%u  ", static_cast<unsigned>(index + 1),
           static_cast<unsigned>(setlist.songs.size()));
  keyLine.clear();
  if (!song.meta.key.empty()) keyLine.append(tr(STR_SONGBOOK_KEY)).append(" ").append(song.meta.key);
  if (!song.meta.capo.empty()) {
    if (!keyLine.empty()) keyLine.append("  ");
    keyLine.append(tr(STR_SONGBOOK_CAPO)).append(" ").append(song.meta.capo);
  }
  const int keyWidth = keyLine.empty() ? 0 : renderer.getTextWidth(UI_12_FONT_ID, keyLine.c_str(), EpdFontFamily::BOLD);
  const std::string title = std::string(position) + song.meta.title;
  titleLine = renderer.truncatedText(UI_12_FONT_ID, title.c_str(), std::max(1, g.bodyWidth - keyWidth - 16),
                                     EpdFontFamily::BOLD);
  artistLine = renderer.truncatedText(UI_10_FONT_ID, song.meta.artist.c_str(), g.bodyWidth / 2);
}

bool SongActivity::layoutsFit() {
  const auto lyricsBudget = songbook::SongLayout::lyricsBudget(song);
  const auto chartBudget = songbook::SongLayout::chartBudget(song);
  const size_t needed = lyricsBudget.bytes() + chartBudget.bytes() + LAYOUT_HEADROOM_BYTES;
  // Fragmentation can split the free heap, so the largest block must hold the
  // two biggest tables together, not just one.
  std::array<size_t, 8> tables{};
  const auto lyricsTables = lyricsBudget.tables();
  const auto chartTables = chartBudget.tables();
  std::copy(lyricsTables.begin(), lyricsTables.end(), tables.begin());
  std::copy(chartTables.begin(), chartTables.end(), tables.begin() + lyricsTables.size());
  std::partial_sort(tables.begin(), tables.begin() + 2, tables.end(), std::greater<>());
  const size_t block = tables[0] + tables[1];
  const auto heap = HalMemory::getDefaultHeap();
  if (heap.freeBytes >= needed && heap.largestBlockBytes >= block) return true;
  LOG_ERR("SONG", "OOM: layouts need %u (block %u), free %u, largest %u", static_cast<unsigned>(needed),
          static_cast<unsigned>(block), static_cast<unsigned>(heap.freeBytes),
          static_cast<unsigned>(heap.largestBlockBytes));
  return false;
}

SongActivity::ViewMode SongActivity::shownMode() const {
  switch (mode) {
    case ViewMode::Lyrics:
      return ViewMode::Lyrics;
    case ViewMode::Chords:
      return chart.rows().empty() ? ViewMode::Lyrics : ViewMode::Chords;
    case ViewMode::Diagrams:
      return songChords.size() == 0 ? ViewMode::Lyrics : ViewMode::Diagrams;
  }
  return ViewMode::Lyrics;
}

SongActivity::ViewMode SongActivity::nextMode() const {
  switch (mode) {
    case ViewMode::Chords:
      return ViewMode::Lyrics;
    case ViewMode::Lyrics:
      return songChords.size() == 0 ? ViewMode::Chords : ViewMode::Diagrams;
    case ViewMode::Diagrams:
      return ViewMode::Chords;
  }
  return ViewMode::Chords;
}

void SongActivity::buildDiagrams() {
  const Geometry g = geometry();
  const Measurer measurer(*this);
  diagrams.build(song, songChords, songbook::stringCount(instrument), measurer, {g.bodyWidth, g.bodyHeight});
}

void SongActivity::selectNextInstrument() {
  instrument = nextInstrument(instrument);
  diagramPage = 0;
  buildDiagrams();
  instrumentChanged = true;
}

bool SongActivity::handleHomeGesture() {
  if (scroll.state() != State::Playing) return false;
  {
    RenderLock lock(*this);
    notice = tr(STR_SONGBOOK_HOLD_TO_LEAVE);
    noticeUntilMs = millis() + NOTICE_MS;
  }
  requestUpdate();
  return true;
}

bool SongActivity::changeSong(const int delta) {
  // Measuring needs the scratch buffer; without it no song can be laid out.
  if (!scratch) return false;
  const int count = static_cast<int>(setlist.songs.size());
  const int target = std::clamp(static_cast<int>(index) + delta, 0, count - 1);
  if (target == static_cast<int>(index)) return false;
  openSong(static_cast<size_t>(target));
  return true;
}

void SongActivity::leave() {
  setResult(PageResult{static_cast<uint32_t>(index)});
  finish();
}

SongActivity::Action SongActivity::readAction() const {
  using Button = MappedInputManager::Button;
  // Long presses are polled first: a fired long press consumes its release.
  if (mappedInput.wasLongPressed(Button::Back, LEAVE_HOLD_MS)) return Action::Leave;
  const bool loaded = loadResult == songbook::SongLoadResult::Ok;
  if (loaded && mappedInput.wasLongPressed(Button::Confirm, HOLD_MS)) return Action::ToggleMode;
  // A short press changes the song in every state (see apply() for playback).
  if (mappedInput.wasReleased(Button::Left)) return Action::PreviousSong;
  if (mappedInput.wasReleased(Button::Right)) return Action::NextSong;
  if (mappedInput.wasReleased(Button::Back)) return Action::Back;
  if (!loaded) return Action::None;
  if (mappedInput.wasReleased(Button::Confirm)) return Action::Confirm;
  // Left/Right change the song here, so single-direction side layouts step
  // with top = back and bottom = forward (orientation-aware). Disabled stays disabled.
  const auto layout = SETTINGS.sideButtonLayout;
  if (layout == CrossPointSettings::PREV_PREV || layout == CrossPointSettings::NEXT_NEXT) {
    const bool swapped = mappedInput.isNavDirectionSwapped();
    if (mappedInput.wasPressed(swapped ? Button::Up : Button::Down)) return Action::StepForward;
    if (mappedInput.wasPressed(swapped ? Button::Down : Button::Up)) return Action::StepBack;
    return Action::None;
  }
  if (mappedInput.wasPressed(Button::PageForward)) return Action::StepForward;
  if (mappedInput.wasPressed(Button::PageBack)) return Action::StepBack;
  return Action::None;
}

bool SongActivity::apply(const Action action, const uint32_t nowMs) {
  const auto showNotice = [&](const char* text) {
    notice = text;
    noticeUntilMs = nowMs + NOTICE_MS;
    return true;
  };
  switch (action) {
    case Action::None:
    case Action::Leave:
      return false;
    case Action::ToggleMode:
      // With an empty chart there is nothing to toggle; keep the view for the next song.
      if (chart.rows().empty()) return false;
      mode = nextMode();
      return true;
    case Action::Confirm:
      // Diagrams are for looking up shapes, so Confirm picks the instrument there.
      if (shownMode() == ViewMode::Diagrams) {
        selectNextInstrument();
        return true;
      }
      switch (scroll.state()) {
        case State::Idle:
        case State::Finished:
          scroll.start(nowMs);
          break;
        case State::Playing:
          scroll.pause(nowMs);
          break;
        case State::Paused:
          scroll.resume(nowMs);
          break;
      }
      return true;
    case Action::Back:
      // Idle and Finished leave before apply(); only an active performance gets here.
      if (scroll.state() == State::Playing) return showNotice(tr(STR_SONGBOOK_HOLD_TO_LEAVE));
      scroll.stop();
      return true;
    case Action::StepForward:
    case Action::StepBack: {
      const int delta = action == Action::StepForward ? 1 : -1;
      if (shownMode() == ViewMode::Lyrics) return scroll.stepBy(delta, nowMs);
      const bool onDiagrams = shownMode() == ViewMode::Diagrams;
      uint16_t& current = onDiagrams ? diagramPage : chartPage;
      const int pages = onDiagrams ? diagrams.pageCount() : chart.pageCount();
      const int page = std::clamp(current + delta, 0, pages - 1);
      if (page == current) return false;
      current = static_cast<uint16_t>(page);
      return true;
    }
    case Action::PreviousSong:
    case Action::NextSong: {
      // A switch during playback keeps the performance going from the new song's top.
      const bool wasPlaying = scroll.state() == State::Playing;
      if (!changeSong(action == Action::NextSong ? 1 : -1)) return false;
      // millis(), not nowMs: the timeline starts once the SD load and layout are done.
      if (wasPlaying && loadResult == songbook::SongLoadResult::Ok) scroll.start(millis());
      return true;
    }
  }
  return false;
}

void SongActivity::loop() {
  const uint32_t now = millis();
  const Action action = readAction();
  if (action == Action::Leave || (action == Action::Back && !scroll.active())) {
    leave();
    return;
  }
  const bool noticeExpired = notice && static_cast<int32_t>(now - noticeUntilMs) >= 0;
  // The timer is polled every pass; the render lock is taken only for a change.
  if (action == Action::None && !noticeExpired && !scroll.pending(now)) return;

  bool redraw = false;
  {
    RenderLock lock(*this);
    if (noticeExpired) {
      notice = nullptr;
      redraw = true;
    }
    redraw |= apply(action, now);
    // The chart ignores scroll steps; it redraws only when the state changes.
    const State before = scroll.state();
    const bool stepped = scroll.update(now);
    redraw |= stepped && (shownMode() == ViewMode::Lyrics || scroll.state() != before);
  }
  if (instrumentChanged) {
    instrumentChanged = false;
    // Start the redraw first; the SD write, outside the lock, overlaps the panel refresh.
    requestUpdate(true);
    SONGBOOK_PREFS.instrument = instrument;
    SONGBOOK_PREFS.saveToFile();
    return;
  }
  if (redraw) requestUpdate();
}

void SongActivity::formatStatus(char* out, const size_t size, const uint32_t nowMs) const {
  if (notice) {
    snprintf(out, size, "%s", notice);
    return;
  }
  char total[16];
  formatTime(total + 1, sizeof(total) - 1, scroll.durationMs());
  total[0] = '~';
  const char* duration = durationEstimated ? total : total + 1;

  const char* modeName = tr(STR_SONGBOOK_LYRICS);
  unsigned page = 0;
  unsigned pages = 1;
  bool truncated = lyrics.truncated();
  switch (shownMode()) {
    case ViewMode::Lyrics:
      break;
    case ViewMode::Chords:
      modeName = tr(STR_SONGBOOK_CHORDS);
      page = chartPage;
      pages = chart.pageCount();
      truncated = chart.truncated();
      break;
    case ViewMode::Diagrams:
      modeName = instrumentName(instrument);
      page = diagramPage;
      pages = diagrams.pageCount();
      truncated = songChords.truncated();
      break;
  }
  char modeText[64];
  if (pages > 1) {
    snprintf(modeText, sizeof(modeText), "%s %u/%u", modeName, page + 1, pages);
  } else {
    snprintf(modeText, sizeof(modeText), "%s", modeName);
  }
  // A layout that ran out of budget is missing its end; say so on stage.
  if (truncated) {
    const size_t used = strlen(modeText);
    snprintf(modeText + used, sizeof(modeText) - used, " (%s)", tr(STR_SONGBOOK_CUT));
  }
  switch (scroll.state()) {
    case State::Idle:
      snprintf(out, size, "%s \xC2\xB7 %s", modeText, duration);
      break;
    case State::Playing:
      snprintf(out, size, "%s \xC2\xB7 %s %s", modeText, tr(STR_SONGBOOK_PLAYING), duration);
      break;
    case State::Paused: {
      char elapsed[16];
      formatTime(elapsed, sizeof(elapsed), scroll.elapsedMs(nowMs));
      snprintf(out, size, "%s \xC2\xB7 %s %s/%s", modeText, tr(STR_SONGBOOK_PAUSED), elapsed, duration);
      break;
    }
    case State::Finished:
      snprintf(out, size, "%s \xC2\xB7 %s %s", modeText, tr(STR_SONGBOOK_FINISHED), duration);
      break;
  }
}

void SongActivity::drawHeader(const Geometry& g) {
  int y = g.headerY + HEADER_PADDING;
  renderer.drawText(UI_12_FONT_ID, g.bodyX, y, titleLine.c_str(), true, EpdFontFamily::BOLD);
  if (!keyLine.empty()) {
    const int keyWidth = renderer.getTextWidth(UI_12_FONT_ID, keyLine.c_str(), EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, g.bodyX + g.bodyWidth - keyWidth, y, keyLine.c_str(), true, EpdFontFamily::BOLD);
  }
  y += renderer.getLineHeight(UI_12_FONT_ID) + HEADER_PADDING;
  char status[96];
  formatStatus(status, sizeof(status), millis());
  const int statusWidth = renderer.getTextWidth(UI_10_FONT_ID, status);
  const int statusX = g.bodyX + g.bodyWidth - statusWidth;
  if (!artistLine.empty()) {
    // The status keeps priority; the artist is clipped short of it.
    const auto clip = renderer.getClipRect();
    renderer.setClipRect(g.bodyX, y, std::max(0, statusX - 12 - g.bodyX), renderer.getLineHeight(UI_10_FONT_ID));
    renderer.drawText(UI_10_FONT_ID, g.bodyX, y, artistLine.c_str());
    renderer.setClipRect(clip[0], clip[1], clip[2], clip[3]);
  }
  renderer.drawText(UI_10_FONT_ID, statusX, y, status);
  const int ruleY = g.headerY + g.headerHeight;
  renderer.drawLine(g.bodyX, ruleY, g.bodyX + g.bodyWidth, ruleY);
}

void SongActivity::drawPiece(const songbook::SongLayout& layout, const songbook::Piece& piece, const int x,
                             const int y) {
  int fontId = 0;
  EpdFontFamily::Style style = EpdFontFamily::REGULAR;
  fontFor(piece.role, piece.size, fontId, style);
  renderer.drawText(fontId, x + piece.x, y + piece.dy, terminated(layout.text(song, piece)), true, style);
}

void SongActivity::drawLyrics(const Geometry& g) {
  const auto& stops = lyrics.scrollStops();
  const int32_t top = stops[std::min(scroll.step(), stops.size() - 1)];
  const int32_t bottom = top + g.bodyHeight;
  const auto& pieces = lyrics.pieces();
  for (const auto& row : lyrics.rows()) {
    if (row.y < top) continue;
    // Rows cut by the bottom edge wait for the next step.
    if (row.y + row.height > bottom) break;
    const int rowY = g.bodyY + row.y - top;
    if (row.chorusBar) renderer.fillRect(g.bodyX + row.x + 2, rowY, CHORUS_BAR_WIDTH, row.height);
    for (uint32_t i = row.firstPiece; i < row.firstPiece + row.pieceCount; ++i) {
      drawPiece(lyrics, pieces[i], g.bodyX + row.x, rowY);
    }
  }
  const int32_t contentHeight = lyrics.contentHeight();
  if (stops.size() > 1 && contentHeight > 0) {
    // Position marker in the right margin.
    const int barX = g.bodyX + g.bodyWidth + 4;
    const int thumbY = g.bodyY + static_cast<int>(static_cast<int64_t>(g.bodyHeight) * top / contentHeight);
    const int thumbHeight =
        std::max(8, static_cast<int>(static_cast<int64_t>(g.bodyHeight) * g.bodyHeight / contentHeight));
    renderer.fillRect(barX, thumbY, SCROLL_BAR_WIDTH, std::min(thumbHeight, g.bodyY + g.bodyHeight - thumbY));
  }
}

void SongActivity::drawChart(const Geometry& g) {
  const auto& pieces = chart.pieces();
  for (const auto& row : chart.rows()) {
    if (row.page != chartPage) continue;
    const int rowY = g.bodyY + row.y;
    for (uint32_t i = row.firstPiece; i < row.firstPiece + row.pieceCount; ++i) {
      drawPiece(chart, pieces[i], g.bodyX + row.x, rowY);
    }
  }
}

void SongActivity::drawDiagrams(const Geometry& g) {
  for (size_t i = 0; i < songChords.size(); ++i) {
    const auto at = diagrams.place(i);
    if (at.page == diagramPage) drawDiagram(g, songChords[i], at);
  }
}

void SongActivity::drawDiagram(const Geometry& g, const songbook::SongChord& entry,
                               const songbook::DiagramLayout::Placement& at) {
  const auto& m = diagrams.metrics();
  int nameFont = 0;
  EpdFontFamily::Style nameStyle = EpdFontFamily::REGULAR;
  fontFor(TextRole::Chord, diagrams.size(), nameFont, nameStyle);

  const int cellX = g.bodyX + at.cellX;
  const int cellY = g.bodyY + at.cellY;
  const int left = g.bodyX + at.gridX;
  // Text is centered over the grid and kept inside the cell when it fits.
  const auto centeredX = [&](const int width) {
    return std::max(cellX, std::min(left + (at.gridWidth - width) / 2, cellX + diagrams.cellWidth() - width));
  };
  // An unknown name may be longer than the cell, so the name is clipped to it.
  const char* name = terminated(song.view(entry.name));
  const auto clip = renderer.getClipRect();
  renderer.setClipRect(cellX, cellY, diagrams.cellWidth(), diagrams.cellHeight());
  renderer.drawText(nameFont, centeredX(renderer.getTextAdvanceX(nameFont, name, nameStyle)), cellY + m.padding, name,
                    true, nameStyle);
  renderer.setClipRect(clip[0], clip[1], clip[2], clip[3]);

  const bool bass = songbook::isBass(instrument);
  songbook::ChordDiagram shape;
  songbook::ToneMap tones;
  const bool found = entry.known && (bass ? songbook::findToneMap(entry.chord, instrument, tones)
                                          : songbook::findChordDiagram(entry.chord, instrument, shape));
  if (!found) {
    // A word would not fit every cell; the mark always does.
    const char* unknown = "?";
    renderer.drawText(nameFont, centeredX(renderer.getTextAdvanceX(nameFont, unknown, nameStyle)),
                      g.bodyY + at.gridY + 2 * m.fretGap, unknown, true, nameStyle);
    return;
  }
  if (bass) {
    drawToneMap(g, at, tones);
  } else {
    drawChordShape(g, at, shape);
  }
}

void SongActivity::drawFretGrid(const Geometry& g, const songbook::DiagramLayout::Placement& at, const int strings,
                                const int firstFret) {
  const auto& m = diagrams.metrics();
  const int left = g.bodyX + at.gridX;
  const int top = g.bodyY + at.gridY;
  const int gridHeight = songbook::DIAGRAM_FRETS * m.fretGap;
  for (int s = 0; s < strings; ++s) {
    renderer.drawLine(left + s * m.stringGap, top, left + s * m.stringGap, top + gridHeight);
  }
  for (int row = 0; row <= songbook::DIAGRAM_FRETS; ++row) {
    renderer.drawLine(left, top + row * m.fretGap, left + at.gridWidth, top + row * m.fretGap);
  }
  if (firstFret == 1) {
    const int nut = std::max(songbook::DIAGRAM_LINE_WIDTH + 1, m.fretGap / 6);
    renderer.fillRect(left, top - nut + 1, at.gridWidth + 1, nut);
    return;
  }
  int labelFont = 0;
  EpdFontFamily::Style labelStyle = EpdFontFamily::REGULAR;
  fontFor(TextRole::Label, diagrams.size(), labelFont, labelStyle);
  char fret[8];
  snprintf(fret, sizeof(fret), "%d", firstFret);
  const int labelY = top + (m.fretGap - renderer.getLineHeight(labelFont)) / 2;
  renderer.drawText(labelFont, g.bodyX + at.fretLabelX, labelY, fret, true, labelStyle);
}

void SongActivity::drawDot(const int centerX, const int centerY, const int radius, const bool solid) {
  const int x = centerX - radius;
  const int y = centerY - radius;
  if (solid) {
    renderer.fillRoundedRect(x, y, 2 * radius, 2 * radius, radius, Color::Black);
    return;
  }
  renderer.fillRoundedRect(x, y, 2 * radius, 2 * radius, radius, Color::White);
  renderer.drawRoundedRect(x, y, 2 * radius, 2 * radius, songbook::DIAGRAM_LINE_WIDTH, radius, true);
}

void SongActivity::drawChordShape(const Geometry& g, const songbook::DiagramLayout::Placement& at,
                                  const songbook::ChordDiagram& shape) {
  const auto& m = diagrams.metrics();
  drawFretGrid(g, at, shape.stringCount, shape.firstFret);
  const auto stringX = [&](const int string) { return g.bodyX + at.gridX + string * m.stringGap; };
  // Center of the row that holds fret, counted from the window's first fret.
  const auto fretY = [&](const int fret) { return g.bodyY + diagrams.rowCenterY(at, fret - shape.firstFret); };

  const int markRadius = m.markRadius;
  const int markY = g.bodyY + at.markY;
  for (int s = 0; s < shape.stringCount; ++s) {
    const int x = stringX(s);
    if (shape.frets[s] == songbook::MUTED) {
      renderer.drawLine(x - markRadius, markY - markRadius, x + markRadius, markY + markRadius,
                        songbook::DIAGRAM_LINE_WIDTH, true);
      renderer.drawLine(x - markRadius, markY + markRadius, x + markRadius, markY - markRadius,
                        songbook::DIAGRAM_LINE_WIDTH, true);
    } else if (shape.frets[s] == 0) {
      drawDot(x, markY, markRadius, false);
    }
  }

  const int r = m.dotRadius;
  if (shape.barreFret > 0) {
    const int from = stringX(shape.barreFrom);
    renderer.fillRoundedRect(from - r, fretY(shape.barreFret) - r, stringX(shape.barreTo) - from + 2 * r, 2 * r, r,
                             Color::Black);
  }
  for (int s = 0; s < shape.stringCount; ++s) {
    const int fret = shape.frets[s];
    if (fret <= 0) continue;
    const bool underBarre =
        shape.barreFret > 0 && fret == shape.barreFret && s >= shape.barreFrom && s <= shape.barreTo;
    if (!underBarre) drawDot(stringX(s), fretY(fret), r, true);
  }
}

void SongActivity::drawToneMap(const Geometry& g, const songbook::DiagramLayout::Placement& at,
                               const songbook::ToneMap& tones) {
  const auto& m = diagrams.metrics();
  drawFretGrid(g, at, tones.stringCount, tones.firstFret);
  // The bass note is solid; the other chord tones are rings. Strings without a mark are not played.
  for (int s = 0; s < tones.stringCount; ++s) {
    const int x = g.bodyX + at.gridX + s * m.stringGap;
    const auto bit = static_cast<uint8_t>(1u << s);
    if (tones.openTones & bit) drawDot(x, g.bodyY + at.markY, m.markRadius, (tones.openBassNotes & bit) != 0);
    for (int row = 0; row < songbook::DIAGRAM_FRETS; ++row) {
      const auto rowBit = static_cast<uint8_t>(1u << row);
      if (!(tones.tones[s] & rowBit)) continue;
      drawDot(x, g.bodyY + diagrams.rowCenterY(at, row), m.dotRadius, (tones.bassNotes[s] & rowBit) != 0);
    }
  }
}

void SongActivity::drawFooter() {
  const char* back = tr(STR_BACK);
  const char* confirm = tr(STR_SONGBOOK_PLAY);
  const char* previous = tr(STR_SONGBOOK_PREV_SONG);
  const char* next = tr(STR_SONGBOOK_NEXT_SONG);
  if (loadResult != songbook::SongLoadResult::Ok) {
    confirm = "";
  } else {
    switch (scroll.state()) {
      case State::Idle:
      case State::Finished:
        break;
      case State::Playing:
        // Leaving needs a hold while playing, so Back stays unlabeled.
        back = "";
        confirm = tr(STR_SONGBOOK_PAUSE);
        break;
      case State::Paused:
        back = tr(STR_SONGBOOK_STOP);
        confirm = tr(STR_SONGBOOK_RESUME);
        break;
    }
  }
  if (loadResult == songbook::SongLoadResult::Ok && shownMode() == ViewMode::Diagrams) {
    confirm = instrumentName(nextInstrument(instrument));
  }
  if (index == 0) previous = "";
  if (index + 1 >= setlist.songs.size()) next = "";
  const auto labels = mappedInput.mapLabels(back, confirm, previous, next);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void SongActivity::render(RenderLock&&) {
  const Geometry g = geometry();
  renderer.clearScreen();
  if (scratch) drawHeader(g);

  if (loadResult != songbook::SongLoadResult::Ok || !scratch) {
    const char* message = tr(STR_SONGBOOK_SONG_MISSING);
    switch (loadResult) {
      case songbook::SongLoadResult::Ok:
      case songbook::SongLoadResult::OutOfMemory:
        message = tr(STR_SONGBOOK_OUT_OF_MEMORY);
        break;
      case songbook::SongLoadResult::Missing:
        break;
      case songbook::SongLoadResult::TooLarge:
        message = tr(STR_SONGBOOK_SONG_TOO_LARGE);
        break;
      case songbook::SongLoadResult::Empty:
        message = tr(STR_SONGBOOK_SONG_EMPTY);
        break;
    }
    renderer.drawCenteredText(UI_12_FONT_ID, g.bodyY + g.bodyHeight / 2, message, true, EpdFontFamily::BOLD);
  } else {
    // Scan pass records the glyphs, then one batched prewarm before the real draw.
    auto* fontCache = renderer.getFontCacheManager();
    const auto drawBody = [&] {
      switch (shownMode()) {
        case ViewMode::Lyrics:
          drawLyrics(g);
          break;
        case ViewMode::Chords:
          drawChart(g);
          break;
        case ViewMode::Diagrams:
          drawDiagrams(g);
          break;
      }
    };
    if (fontCache) {
      auto scope = fontCache->createPrewarmScope();
      drawBody();
      scope.endScanAndPrewarm();
      drawBody();
    } else {
      drawBody();
    }
  }
  drawFooter();
  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
}
