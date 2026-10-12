#include "BootDiagnosticsActivity.h"

#include <BoardConfig.h>
#include <CrossPointSettings.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <I18n.h>
#include <XteinkDetect.h>
#include <esp_sleep.h>
#include <esp_system.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "SystemStatus.h"  // displayControllerName()
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

using BootDiag::resetReasonName;
using BootDiag::wakeCauseName;

// The verdict line — the one sentence a reporter is being asked to read. Three states,
// not two: a sleep that cut the battery latch (the X4 default) leaves no breadcrumb to
// finalise from, so its success is deduced from the next boot's reset reason rather than
// measured, and the page says which of the two it was.
const char* outcomeText(const BootDiag::Record& record) {
  switch (BootDiag::outcomeOf(record)) {
    case BootDiag::SleepOutcome::ReachedDeepSleep:
      return tr(STR_DIAG_SLEEP_OK);
    case BootDiag::SleepOutcome::ReleaseTimedOut:
      return tr(STR_DIAG_SLEEP_REL_TIMEOUT);
    case BootDiag::SleepOutcome::PoweredOffAsAsked:
      return tr(STR_DIAG_SLEEP_POWERED_OFF);
    case BootDiag::SleepOutcome::PoweredOffUnasked:
      return tr(STR_DIAG_SLEEP_POWERED_OFF_BAD);
    case BootDiag::SleepOutcome::Unfinished:
      return tr(STR_DIAG_SLEEP_UNFINISHED);
    case BootDiag::SleepOutcome::DidNotSleep:
      break;
  }
  return tr(STR_DIAG_SLEEP_STUCK);
}

// Compact marker for the same thing on a history line, where a full sentence will not fit.
const char* outcomeTag(const BootDiag::Record& record) {
  switch (BootDiag::outcomeOf(record)) {
    case BootDiag::SleepOutcome::ReachedDeepSleep:
      return "slept";
    case BootDiag::SleepOutcome::ReleaseTimedOut:
      return "STUCK";
    case BootDiag::SleepOutcome::PoweredOffAsAsked:
      return "off";
    case BootDiag::SleepOutcome::PoweredOffUnasked:
      return "off!";
    case BootDiag::SleepOutcome::Unfinished:
      return "open";
    case BootDiag::SleepOutcome::DidNotSleep:
      break;
  }
  return "STUCK";
}

}  // namespace

void BootDiagnosticsActivity::onEnter() {
  Activity::onEnter();
  loaded_ = false;
  requestUpdate();
}

void BootDiagnosticsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
      mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    finish();
    return;
  }
  // Read the ring on the first loop pass rather than in onEnter(), so the header paints
  // immediately and the SD read (which shares the display's SPI bus) does not sit in
  // front of the first frame.
  if (!loaded_) {
    recordCount_ = BootDiag::loadRecords(records_, BootDiag::kCapacity);
    loaded_ = true;
    requestUpdate();
  }
}

void BootDiagnosticsActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, /*hasBottomHints=*/true, /*hasSideHints=*/false);

  renderer.clearScreen();
  GUI.drawHeader(renderer,
                 Rect{contentRect.x, contentRect.y + metrics.topPadding, contentRect.width, metrics.headerHeight},
                 tr(STR_BOOT_DIAGNOSTICS), CROSSPOINT_VERSION);

  const int fontId = UI_10_FONT_ID;
  const int leftX = contentRect.x + metrics.verticalSpacing * 3;
  // Labels are translated and so vary in length; give the value column a fixed start far
  // enough right that the longest German label still clears it.
  const int valueX = contentRect.x + contentRect.width * 42 / 100;
  const int valueW = contentRect.x + contentRect.width - valueX - metrics.verticalSpacing * 3;
  const int lineH = renderer.getLineHeight(fontId);
  const int rowStep = lineH + 2;
  const int subHeaderHeight = lineH + 6;
  const int hintsTop = contentRect.y + contentRect.height - metrics.buttonHintsHeight;
  int y = contentRect.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  auto room = [&](int rows) { return y + rows * rowStep <= hintsTop; };

  auto drawSection = [&](const char* title) {
    GUI.drawSubHeader(renderer, Rect{contentRect.x, y, contentRect.width, subHeaderHeight}, title);
    y += subHeaderHeight + 2;
  };
  const int labelW = valueX - leftX - 4;
  auto drawRow = [&](const char* label, const char* value) {
    // Both halves are clipped: labels are translated (German runs noticeably longer than
    // English) and values are free-form technical strings, so either can overrun.
    renderer.drawText(fontId, leftX, y, renderer.truncatedText(fontId, label, labelW, EpdFontFamily::BOLD).c_str(),
                      true, EpdFontFamily::BOLD);
    renderer.drawText(fontId, valueX, y, renderer.truncatedText(fontId, value, valueW).c_str());
    y += rowStep;
  };
  // A row whose value spans the full width — used for the phase trace and the history
  // lines, which are long, monospaced-ish token runs with no useful label/value split.
  const int wideW = contentRect.x + contentRect.width - leftX - metrics.verticalSpacing * 3;
  auto drawWide = [&](const char* text) {
    renderer.drawText(fontId, leftX, y, renderer.truncatedText(fontId, text, wideW).c_str());
    y += rowStep;
  };

  char buf[96];

  // ---- Hardware variant ----------------------------------------------------------
  // First, because it is the identity line of any bug report. The X3 and X4 each ship in
  // more than one silicon variant (per-batch panel swaps UC8253 -> UC8279d and
  // SSD1677 -> UC8179), and two reports of "the same" fault are only comparable once it is
  // known whether they came from the same variant. All of this is already resolved at boot
  // by a live bus probe — it just had nowhere to be read.
  drawSection(tr(STR_SEC_HARDWARE));
  drawRow(tr(STR_DIAG_BOARD_PROFILE), BoardConfig::ACTIVE.name);
  {
    const auto& probe = freeink::getXteinkDisplayProbeDiag();
    snprintf(buf, sizeof(buf), "%s%s", displayControllerName(BoardConfig::ACTIVE.displayController),
             probe.promoted ? " (promoted by probe)" : "");
    drawRow(tr(STR_DISPLAY_CONTROLLER), buf);
    // The raw fingerprint, not just the name it resolved to: two units can report the same
    // controller and still differ here, and an unrun or inconclusive probe is itself a
    // finding. Bytes are what a cross-reference between two reporters actually compares.
    if (probe.valid) {
      snprintf(buf, sizeof(buf), "ver %02X %02X %02X %02X %02X flg %02X v%u%s", probe.ver[0], probe.ver[1],
               probe.ver[2], probe.ver[3], probe.ver[4], probe.flg, probe.verdict, probe.mtpValid ? " mtp" : "");
    } else {
      snprintf(buf, sizeof(buf), "%s", tr(STR_DIAG_UNKNOWN));
    }
    drawRow(tr(STR_DIAG_PANEL_PROBE), buf);
  }

  // ---- This boot -----------------------------------------------------------------
  drawSection(tr(STR_DIAG_THIS_BOOT));
  // On the C3 a reset-button press and a real power-on are the SAME reset reason — the
  // chip has no ESP_RST_EXT — so this row cannot tell them apart and the row below is what
  // does. Shown anyway: it still separates a panic, a watchdog and a deep-sleep wake.
  drawRow(tr(STR_DIAG_RESET_REASON), resetReasonName(static_cast<uint8_t>(esp_reset_reason())));
  drawRow(tr(STR_DIAG_PREV_SESSION),
          BootDiag::previousSessionEndedWithoutSleep() ? tr(STR_DIAG_PREV_NO_SLEEP) : tr(STR_DIAG_PREV_SLEPT));
  // Headline row rather than history-only: a device that wakes, refuses the press and
  // sleeps again leaves the panel untouched, so this counter is the only thing separating
  // "it is looping" from "it is dead". Non-zero here IS the diagnosis.
  {
    const uint16_t aborted = BootDiag::abortsInCurrentRun(records_, recordCount_);
    // Read straight out of NVS as well as out of the ring. A zero here with a non-zero
    // lifetime means the aborts were counted but lost on the way to the card; a zero
    // lifetime means the abort path never ran at all. Those two look identical if only the
    // ring is consulted, which is why an X3 reporting nothing could not be narrowed down.
    const BootDiag::AbortCounts counts = BootDiag::abortCounts();
    if (aborted == 0 && counts.lifetime == 0) {
      drawRow(tr(STR_DIAG_ABORTED_BOOTS), tr(STR_DIAG_ABORTED_NONE));
    } else if (aborted == 0) {
      snprintf(buf, sizeof(buf), "%s (%lu %s)", tr(STR_DIAG_ABORTED_NONE), static_cast<unsigned long>(counts.lifetime),
               tr(STR_DIAG_ABORTED_LIFETIME));
      drawRow(tr(STR_DIAG_ABORTED_BOOTS), buf);
    } else {
      snprintf(buf, sizeof(buf), "%u — %s (%lu %s)", aborted, tr(STR_DIAG_ABORTED_FMT),
               static_cast<unsigned long>(counts.lifetime), tr(STR_DIAG_ABORTED_LIFETIME));
      drawRow(tr(STR_DIAG_ABORTED_BOOTS), buf);
      // Every reason on its own continuation row. The total alone says "it is looping"; the
      // breakdown says which way to look — a run of released-early is a user pressing too
      // briefly, a run of not-pressed is the device waking with nothing on the button at
      // all, and usb-boot is the cable rather than the button.
      uint8_t first = 0;
      uint8_t last = 0;
      if (BootDiag::currentRunRange(records_, recordCount_, first, last)) {
        for (uint8_t i = first; i < last && room(1); i++) {
          const BootDiag::Record& r = records_[i];
          if (r.kind != BootDiag::KindAborted) continue;
          snprintf(buf, sizeof(buf), "   %u%s  %s / gate %s", r.msA, r.msA >= BootDiag::kAbortCountCap ? "+" : "",
                   BootDiag::triggerName(static_cast<BootDiag::SleepTrigger>(r.reason)),
                   HalGPIO::wakeVerdictName(static_cast<HalGPIO::WakeVerdict>(r.code)));
          drawWide(buf);
        }
      }
    }
    // Non-zero only when a completed boot could not write the ring — normally invisible.
    if (counts.undrained > 0 && room(1)) {
      snprintf(buf, sizeof(buf), "   %u %s", counts.undrained, tr(STR_DIAG_ABORTED_PENDING));
      drawWide(buf);
    }
  }
  drawRow(tr(STR_DIAG_WAKE_CAUSE), wakeCauseName(static_cast<uint8_t>(esp_sleep_get_wakeup_cause())));

  const auto& check = BootDiag::wakeCheck();
  snprintf(buf, sizeof(buf), "%s (held %u ms, needs %u ms)", HalGPIO::wakeVerdictName(check.verdict), check.heldMs,
           CrossPointSettings::getPowerWakeHoldDuration());
  drawRow(tr(STR_DIAG_WAKE_GATE), buf);

  snprintf(buf, sizeof(buf), "%u ms", BootDiag::phaseMs(BootDiag::BootPhase::FirstPaint));
  drawRow(tr(STR_DIAG_FIRST_PAINT), buf);

  // Per-phase deltas, wrapped across as many full-width rows as they need. Deltas rather
  // than absolutes: the cost of a phase is the actionable figure, and absolutes are just
  // their running sum. Unreached phases are skipped, so a short trace is itself a signal
  // about which path the boot took.
  renderer.drawText(fontId, leftX, y,
                    renderer.truncatedText(fontId, tr(STR_DIAG_BOOT_PHASES), labelW, EpdFontFamily::BOLD).c_str(), true,
                    EpdFontFamily::BOLD);
  y += rowStep;
  {
    size_t used = 0;
    uint16_t previous = 0;
    buf[0] = '\0';
    for (uint8_t i = 0; i < static_cast<uint8_t>(BootDiag::BootPhase::Count) && room(1); i++) {
      const auto phase = static_cast<BootDiag::BootPhase>(i);
      if (!BootDiag::phaseReached(phase)) continue;
      char token[24];
      const int len = snprintf(token, sizeof(token), "%s+%u", BootDiag::phaseName(phase),
                               static_cast<unsigned>(BootDiag::phaseMs(phase) - previous));
      previous = BootDiag::phaseMs(phase);
      if (len < 0) continue;
      // Wrap on measured width, not on a token count: the labels differ in length, the
      // stamps grow from one digit to five as the boot proceeds, and the page renders in
      // both orientations — any fixed count either clips in portrait or wastes half the
      // line in landscape. Buffer overrun is impossible by the same test: the append only
      // happens when the result still fits, and a single token is 23 chars at most.
      if (used > 0) {
        char candidate[sizeof(buf)];
        snprintf(candidate, sizeof(candidate), "%s %s", buf, token);
        if (renderer.getTextWidth(fontId, candidate) > wideW || used + 1 + static_cast<size_t>(len) >= sizeof(buf)) {
          drawWide(buf);
          used = 0;
          buf[0] = '\0';
        }
      }
      used += static_cast<size_t>(snprintf(buf + used, sizeof(buf) - used, "%s%s", used ? " " : "", token));
    }
    if (used > 0 && room(1)) drawWide(buf);
  }

  // Idle duty cycle. The closest proxy for average current without a meter, and readable
  // only here: the CDC guard turns light sleep off for as long as a serial monitor is
  // attached, so a serial log of this would only ever print zeroes.
  {
    const auto& stats = powerManager.lightSleepStats();
    const uint32_t totalMs = stats.sleptMs + stats.awakeMs;
    const unsigned pct = totalMs > 0 ? static_cast<unsigned>((stats.sleptMs * 100ULL) / totalMs) : 0;
    snprintf(buf, sizeof(buf), "%us slept (%u%%), %u tries, %u gpio wakes", stats.sleptMs / 1000, pct, stats.attempts,
             stats.wakeGpio);
    if (room(1)) drawRow(tr(STR_LIGHT_SLEEP), buf);
  }

  // ---- Last sleep ----------------------------------------------------------------
  // Sourced from the newest sleep record rather than from live state: the interesting
  // case is a sleep that ended in a way this boot could not observe.
  const BootDiag::Record* lastSleep = nullptr;
  for (uint8_t i = 0; i < recordCount_; i++) {
    if (records_[i].kind == BootDiag::KindSleep) {
      lastSleep = &records_[i];
      break;
    }
  }

  if (room(7)) {
    drawSection(tr(STR_DIAG_LAST_SLEEP));
    if (lastSleep == nullptr) {
      drawRow(tr(STR_DIAG_STOPPED_AT), tr(STR_DIAG_NO_RECORDS));
    } else {
      drawRow(tr(STR_DIAG_STOPPED_AT), BootDiag::stageName(static_cast<BootDiag::SleepStage>(lastSleep->code)));
      // The verdict gets a full-width row of its own rather than sharing the value
      // column: it is the one line a reporter is being asked to read, and the value
      // column is too narrow to hold it next to the stage name without clipping.
      drawWide(outcomeText(*lastSleep));
      drawRow(tr(STR_DIAG_TRIGGER), BootDiag::triggerName(static_cast<BootDiag::SleepTrigger>(lastSleep->reason)));
      drawRow(tr(STR_DIAG_BATTERY_LATCH),
              (lastSleep->flags & BootDiag::kFlagKeepClock) ? tr(STR_DIAG_LATCH_KEPT) : tr(STR_DIAG_LATCH_CUT));
      snprintf(buf, sizeof(buf), "%u ms%s", lastSleep->msA,
               (lastSleep->flags & BootDiag::kFlagReleaseTimeout) ? " (TIMED OUT)" : "");
      drawRow(tr(STR_DIAG_RELEASE_WAIT), buf);
    }
  }

  // ---- History -------------------------------------------------------------------
  // Alternating sleep/boot lines are what make a report self-explanatory: a
  // "await-release" sleep followed by an "ext-reset" boot is a hang, while a "wake-armed"
  // sleep followed by a "power-on" boot is a healthy cycle.
  if (room(3)) {
    drawSection(tr(STR_DIAG_HISTORY));
    if (recordCount_ == 0) {
      drawWide(tr(STR_DIAG_NO_RECORDS));
    }
    for (uint8_t i = 0; i < recordCount_ && room(1); i++) {
      const BootDiag::Record& r = records_[i];
      if (r.kind == BootDiag::KindSleep) {
        snprintf(buf, sizeof(buf), "%lu sleep %s %s %s %s%s", static_cast<unsigned long>(r.seq), outcomeTag(r),
                 BootDiag::stageName(static_cast<BootDiag::SleepStage>(r.code)),
                 BootDiag::triggerName(static_cast<BootDiag::SleepTrigger>(r.reason)),
                 (r.flags & BootDiag::kFlagKeepClock) ? "latch-kept" : "latch-cut",
                 (r.flags & BootDiag::kFlagReleaseTimeout) ? " rel-timeout" : "");
      } else if (r.kind == BootDiag::KindAborted) {
        // Its slots hold a trigger, a gate verdict and a count, not a reset reason and timings.
        snprintf(buf, sizeof(buf), "%lu abort %u%s %s / gate %s", static_cast<unsigned long>(r.seq), r.msA,
                 r.msA >= BootDiag::kAbortCountCap ? "+" : "",
                 BootDiag::triggerName(static_cast<BootDiag::SleepTrigger>(r.reason)),
                 HalGPIO::wakeVerdictName(static_cast<HalGPIO::WakeVerdict>(r.code)));
      } else {
        snprintf(buf, sizeof(buf), "%lu boot  %s/%s %s %ums sd@%ums", static_cast<unsigned long>(r.seq),
                 resetReasonName(r.reason), wakeCauseName(r.flags),
                 HalGPIO::wakeVerdictName(static_cast<HalGPIO::WakeVerdict>(r.code)), r.msB, r.msC);
      }
      drawWide(buf);
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
