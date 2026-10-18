#pragma once

#include "activities/Activity.h"

/// Settings > System > Font Scaling Test.
///
/// The reader's size ladder in one column per family, every rung marked as a real pre-rendered
/// face or as one rendered by scaling the 18 pt master.
///
/// It exists because the measurements could not answer the question. bench/font_main.cpp put the
/// mean absolute coverage error of an area-weighted resample at 6-8% across the ratios the ladder
/// uses, and scored the two families within 0.1% of each other — but a coverage average cannot see
/// the edge definition and stem weight a reader notices, so the panel is the instrument and this
/// is how it gets pointed at the ladder.
///
/// Draws through the SAME font IDs the reader selects, and takes each row's real/scaled marker
/// from GfxRenderer::fontBaseScale() rather than a table of its own — so it cannot report a size
/// as real when it is not, and cannot drift from what ships.
///
/// Renders with a FULL refresh per page, not FAST. FAST leaves the previous page as a differential
/// baseline, and a ghost sitting inside a stem reads exactly like a resampling artifact — which
/// happened, and cost an afternoon chasing a defect the font path did not have.
///
/// It once carried per-style and running-text pages comparing a real face against the same size
/// scaled from a smaller one. Those answered a question that is now closed — whether to synthesise
/// sizes at all, and from which master — and kept as decoration they would have implied the reader
/// produces some size by reduction, which it does not. What remains is the ladder, which is what a
/// reader meets: 18 pt real beside 20 pt scaled from it is the comparison that question needed.
class FontScalingTestActivity final : public Activity {
 public:
  explicit FontScalingTestActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("FontScalingTest", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  void renderContent() const;

  bool notoSans_ = false;  ///< which family's ladder is showing
};
