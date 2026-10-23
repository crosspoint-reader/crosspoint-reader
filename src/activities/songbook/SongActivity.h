#pragma once

#include <memory>
#include <string>

#include "AutoScrollController.h"
#include "ChordShapes.h"
#include "DiagramLayout.h"
#include "Setlist.h"
#include "Song.h"
#include "SongChords.h"
#include "SongLayout.h"
#include "SongbookFiles.h"
#include "activities/Activity.h"

// Performance view for one song of a setlist: lyrics with chords (timed
// auto-scroll), the chords-only chart, or chord diagrams for the chosen
// instrument. Only the current song is in RAM.
class SongActivity final : public Activity {
 public:
  // setlist is owned by the stacked SetlistActivity, which outlives this screen.
  SongActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const songbook::Setlist& setlist, size_t index);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Only a running song holds off sleep; Pause, Stop, and the song's end
  // restart the normal sleep timeout.
  bool preventAutoSleep() override { return scroll.state() == songbook::AutoScrollController::State::Playing; }
  // The global Home gesture must not end a performance (Home-key and touch boards).
  bool handleHomeGesture() override;

 private:
  enum class ViewMode : uint8_t { Lyrics, Chords, Diagrams };
  enum class Action : uint8_t {
    None,
    Leave,
    ToggleMode,
    Confirm,
    Back,
    StepForward,
    StepBack,
    PreviousSong,
    NextSong,
  };

  struct Geometry {
    int headerY;
    int headerHeight;
    int bodyX;
    int bodyY;
    int bodyWidth;
    int bodyHeight;
  };

  class Measurer final : public songbook::TextMeasurer {
   public:
    explicit Measurer(SongActivity& owner) : owner(owner) {}
    int width(songbook::TextRole role, songbook::FontSize size, std::string_view text) const override;
    int lineHeight(songbook::TextRole role, songbook::FontSize size) const override;

   private:
    SongActivity& owner;
  };

  Action readAction() const;
  // Applies the action; true when the screen must be redrawn. Caller holds RenderLock.
  bool apply(Action action, uint32_t nowMs);
  void leave();
  // Loads and lays out a song and resets the timer. Caller holds RenderLock.
  void openSong(size_t songIndex);
  // False (with OutOfMemory) when the heap cannot hold both layouts' budgets.
  bool layoutsFit();
  bool changeSong(int delta);
  // A song without chords has an empty chart and no diagrams, so it shows its lyrics.
  ViewMode shownMode() const;
  // Hold Confirm cycles Chords, Lyrics, Diagrams; Diagrams is skipped without chords.
  ViewMode nextMode() const;
  // Lays out the diagram grid for the current song and instrument. Caller holds RenderLock.
  void buildDiagrams();
  void selectNextInstrument();
  Geometry geometry() const;
  // NUL-terminated copy of text in the shared scratch buffer.
  const char* terminated(std::string_view text) const;

  void drawHeader(const Geometry& g);
  void drawLyrics(const Geometry& g);
  void drawChart(const Geometry& g);
  void drawDiagrams(const Geometry& g);
  void drawDiagram(const Geometry& g, const songbook::SongChord& entry, const songbook::DiagramLayout::Placement& at);
  void drawFretGrid(const Geometry& g, const songbook::DiagramLayout::Placement& at, int strings, int firstFret);
  void drawChordShape(const Geometry& g, const songbook::DiagramLayout::Placement& at,
                      const songbook::ChordDiagram& shape);
  void drawToneMap(const Geometry& g, const songbook::DiagramLayout::Placement& at, const songbook::ToneMap& tones);
  // A filled dot, or a ring when not solid.
  void drawDot(int centerX, int centerY, int radius, bool solid);
  void drawPiece(const songbook::SongLayout& layout, const songbook::Piece& piece, int x, int y);
  void drawFooter();
  void formatStatus(char* out, size_t size, uint32_t nowMs) const;

  const songbook::Setlist& setlist;
  size_t index;
  songbook::Song song;
  songbook::SongLayout lyrics;
  songbook::SongLayout chart;
  songbook::AutoScrollController scroll;
  songbook::SongLoadResult loadResult = songbook::SongLoadResult::Missing;
  // The chart is the performance view; lyrics are mostly for rehearsal. Left
  // and Right keep the chosen view; read it through shownMode().
  ViewMode mode = ViewMode::Chords;
  uint16_t chartPage = 0;
  songbook::SongChords songChords;
  songbook::DiagramLayout diagrams;
  songbook::Instrument instrument = songbook::Instrument::Guitar;
  uint16_t diagramPage = 0;
  // Set under RenderLock; loop() saves the choice after releasing it.
  bool instrumentChanged = false;
  bool durationEstimated = false;
  // Header strings truncated once per song, outside the render path.
  std::string titleLine;
  std::string keyLine;
  std::string artistLine;
  const char* notice = nullptr;
  uint32_t noticeUntilMs = 0;
  int pagesUntilFullRefresh = 0;
  // Shared by layout (main task) and drawing (render task); both run under RenderLock.
  std::unique_ptr<char[]> scratch;
};
