#include <BoardConfig.h>
#include <BootHeapProbe.h>
#include <HalCapabilities.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <Logging.h>
#include <XteinkDetect.h>  // applyXteinkDisplayController() — see begin()
#include <esp_heap_caps.h>

#include "HalSpiBus.h"

// Global HalDisplay instance, bracketed by static-init heap probes (slots 0/1).
static BootHeapProbe s_probePreDisplay(0);
HalDisplay display;
static BootHeapProbe s_probePostDisplay(1);

// Display pins come from the board profile rather than the C3 macros in
// HalGPIO.h. Verified identical for the C3 before converting: X3, X3-UC8279 and
// X4 all specify {8, 10, 21, 4, 5, 6} = {sclk, mosi, cs, dc, rst, busy}.
//
// SUBTLETY, and the reason for the static_assert below. `display` is a GLOBAL,
// so this constructor runs during static init — long before gpio.begin() calls
// BoardConfig::selectDevice(). At that point BoardConfig::ACTIVE still holds
// DEFAULT_DEVICE (for the dual C3 binary, the X4 profile), not the
// runtime-detected board.
//
// That is safe here only because every profile that can share one binary agrees
// on the display pins. Reading ACTIVE is itself sound at this point — it is an
// inline variable with a constexpr initialiser, so it is constant-initialised
// and immune to static-init order — but its VALUE is the default, not the
// detection result. Anything that differs per board within a binary must be
// read in begin(), not here.
#if FREEINK_DEVICE_X3 && FREEINK_DEVICE_X4
static_assert(BoardConfig::XTEINK_X3.display.sclk == BoardConfig::XTEINK_X4.display.sclk &&
                  BoardConfig::XTEINK_X3.display.mosi == BoardConfig::XTEINK_X4.display.mosi &&
                  BoardConfig::XTEINK_X3.display.cs == BoardConfig::XTEINK_X4.display.cs &&
                  BoardConfig::XTEINK_X3.display.dc == BoardConfig::XTEINK_X4.display.dc &&
                  BoardConfig::XTEINK_X3.display.rst == BoardConfig::XTEINK_X4.display.rst &&
                  BoardConfig::XTEINK_X3.display.busy == BoardConfig::XTEINK_X4.display.busy,
              "X3 and X4 ship in one binary but disagree on display pins; HalDisplay's constructor "
              "runs before selectDevice() and would latch the wrong ones");
#endif

HalDisplay::HalDisplay()
    : einkDisplay(BoardConfig::ACTIVE.display.sclk, BoardConfig::ACTIVE.display.mosi, BoardConfig::ACTIVE.display.cs,
                  BoardConfig::ACTIVE.display.dc, BoardConfig::ACTIVE.display.rst, BoardConfig::ACTIVE.display.busy) {}

HalDisplay::~HalDisplay() {}

void HalDisplay::begin(bool seamless) {
#if !FREEINK_MCU_C3
  // Resolve which panel controller this unit actually carries, before
  // einkDisplay.begin() picks a driver from it.
  //
  // The Xteink 800x480 boards ship two different controllers depending on
  // production batch -- the original SSD1677 and, in newer batches, an UltraChip
  // UC8179/UC8279. FreeInkDisplay::begin() selects the driver purely from
  // BoardConfig::ACTIVE.displayController, and the ONLY thing that ever writes
  // that field is this probe. Without it a newer X4 Pro gets driven with
  // SSD1677 command streams and develops no image.
  //
  // The C3 already does this inside HalGPIO::begin(), which is where it has to
  // happen there: the probe bit-bangs the display pins, and the C3 pre-claims
  // them with SPI.begin() in that same function. Every other board leaves the
  // pins alone until EpdBus claims them inside einkDisplay.begin() below, so
  // here is that board's equivalent moment -- after gpio.begin(), before SPI.
  // Guarded rather than unconditional so the C3 does not probe a second time
  // with SPI already holding the bus.
  //
  // Safe to run on a unit that turns out NOT to be UltraChip: the probe only
  // resets and reads, and a part that does not answer the UC81xx read registers
  // leaves the profile's own controller in place. Matches upstream/develop,
  // which does the same call from setupDisplayAndFonts().
  static bool controllerResolved = false;
  if (!controllerResolved) {
    controllerResolved = true;
    if (freeink::applyXteinkDisplayController()) {
      LOG_INF("DISP", "Panel controller: UltraChip UC81xx variant detected");
    }
  }
#endif

  // Set X3-specific panel mode before initializing.
  if (gpio.deviceIsX3()) {
    einkDisplay.setDisplayX3();
  }

  // Drop the CPU clock while the render task sleeps out a waveform (any BUSY
  // wait that proves long — the SDK's bus fires the hooks around the ISR sleep
  // or the poll loop) and restore it before the post-waveform SPI work. Policy
  // (WiFi / lock / idle low-power guards) lives in HalPowerManager; the driver
  // just brackets the wait.
  einkDisplay.setBusyWaitHooks([] { powerManager.enterWaveformWait(); }, [] { powerManager.exitWaveformWait(); });

  {
    HalSpiBus::Lock spiLock;
    einkDisplay.begin();
  }

  // Request resync after specific wakeup events to ensure clean display state.
  // Skip when seamless=true so the current panel content is preserved (Quick Resume).
  if (!seamless) {
    const auto wakeupReason = gpio.getWakeupReason();
    if (wakeupReason == HalGPIO::WakeupReason::PowerButton || wakeupReason == HalGPIO::WakeupReason::AfterFlash ||
        wakeupReason == HalGPIO::WakeupReason::Other) {
      einkDisplay.requestResync();
    }
  }
}

void HalDisplay::clearScreen(uint8_t color) const { einkDisplay.clearScreen(color); }

void HalDisplay::drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           bool fromProgmem) const {
  einkDisplay.drawImage(imageData, x, y, w, h, fromProgmem);
}

void HalDisplay::drawImageTransparent(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                      bool fromProgmem) const {
  einkDisplay.drawImageTransparent(imageData, x, y, w, h, fromProgmem);
}

static uint8_t refreshModeToByte(HalDisplay::RefreshMode mode) {
  switch (mode) {
    case HalDisplay::RefreshMode::FAST_REFRESH:
      return 0x0C;
    case HalDisplay::RefreshMode::HALF_REFRESH:
      return 0xD4;
    case HalDisplay::RefreshMode::FULL_REFRESH:
    default:
      return 0x34;
  }
}

EInkDisplay::RefreshMode convertRefreshMode(HalDisplay::RefreshMode mode) {
  switch (mode) {
    case HalDisplay::FULL_REFRESH:
      return EInkDisplay::FULL_REFRESH;
    case HalDisplay::HALF_REFRESH:
      return EInkDisplay::HALF_REFRESH;
    case HalDisplay::FAST_REFRESH:
    default:
      return EInkDisplay::FAST_REFRESH;
  }
}

void HalDisplay::requestResync(uint8_t settlePasses) {
  if (HalCapabilities::panelNeedsHalfRefreshSettle() && settlePasses > pendingX3SettlePasses) {
    pendingX3SettlePasses = settlePasses;
  }
}

// Every path that reaches the panel logs one line with a shared sequence number.
// Only displayBuffer() used to log, so a second refresh arriving through
// refreshDisplay / triggerDisplay / displayWindow was invisible -- and an
// invisible extra refresh is exactly what "the menu update appeared twice" and a
// doubled displayBuffer time look like from the outside. Cheap: one counter and a
// DBG line per refresh, not per pixel.
static uint32_t panelSeq = 0;

void HalDisplay::displayBuffer(HalDisplay::RefreshMode mode, bool turnOffScreen) {
  HalSpiBus::Lock spiLock;

  lastRefreshMode = mode;
  lastDisplayModeByte = refreshModeToByte(mode);

  if (HalCapabilities::panelNeedsHalfRefreshSettle() && mode == RefreshMode::HALF_REFRESH) {
    einkDisplay.requestResync(pendingX3SettlePasses > 1 ? pendingX3SettlePasses : 1);
  } else if (pendingX3SettlePasses > 0) {
    einkDisplay.requestResync(pendingX3SettlePasses);
  }
  pendingX3SettlePasses = 0;

  // Panel time, measured at the one place every paint funnels through. The boot trace
  // (main.cpp) shows the splash costing seconds; without this the host-side compose and
  // the waveform itself are indistinguishable inside that number. FAST/HALF/FULL are
  // reported separately because promotions happen inside the driver (a FAST can be
  // upgraded to HALF on a cold panel), so the mode asked for is not always what ran.
  const unsigned long refreshStart = millis();
  einkDisplay.displayBuffer(convertRefreshMode(mode), turnOffScreen);
  const unsigned long refreshMs = millis() - refreshStart;
  // Keep the FAST figure: callers weighing an extra intermediate repaint need to know what one
  // costs on the controller actually in front of them. Clamped so a one-off stall (a contended
  // bus, a cold panel) cannot poison the measurement permanently.
  if (mode == RefreshMode::FAST_REFRESH) {
    lastFastRefreshMs = static_cast<uint16_t>(refreshMs > 60000 ? 60000 : refreshMs);
  }
  const char* const modeName =
      mode == RefreshMode::FAST_REFRESH ? "FAST" : (mode == RefreshMode::HALF_REFRESH ? "HALF" : "FULL");
  const unsigned long seq = ++panelSeq;

  // The X4 differential keeps its previous-frame baseline in the host-managed secondary buffer,
  // so a FAST requested without one is promoted to HALF inside the driver
  // (FreeInkDisplay::resolveRefreshMode) — roughly 500 ms becoming 1700 ms. That used to be
  // invisible: the line read "mode=FAST took 1755 ms", which reads as a stuck panel rather than
  // a promotion, and cost a whole debugging session when Background-B's borrow of the secondary
  // buffer degraded every refresh behind the reader menu.
  //
  // The two flags are reported rather than the resolved mode on purpose: resolveRefreshMode() is
  // internal to the driver, and re-deriving its verdict here would be a second copy of the rule
  // that can drift from the first. These are the inputs it decides on, which is enough to explain
  // the duration without asserting what the driver did.
  //
  // Gated on the panel keeping its baseline in a host buffer rather than on deviceIsX4(): that
  // question is about the display path, and asking it by board name answers "no" on every S3
  // board by construction, so an X4 Pro promoted to the same driver would log the misleading
  // line this exists to prevent.
  if (mode == RefreshMode::FAST_REFRESH && fastRefreshNeedsHostFrame() && !einkDisplay.hasSecondaryBuffer() &&
      !einkDisplay.singleBufferFastDiff()) {
    LOG_DBG("DISP",
            "#%lu displayBuffer mode=%s took %lu ms (no diff baseline: secondary=0 fastDiff=0 -> driver runs HALF)",
            seq, modeName, refreshMs);
  } else {
    LOG_DBG("DISP", "#%lu displayBuffer mode=%s took %lu ms", seq, modeName, refreshMs);
  }
}

void HalDisplay::refreshDisplay(HalDisplay::RefreshMode mode, bool turnOffScreen) {
  HalSpiBus::Lock spiLock;

  lastRefreshMode = mode;
  lastDisplayModeByte = refreshModeToByte(mode);

  if (HalCapabilities::panelNeedsHalfRefreshSettle() && mode == RefreshMode::HALF_REFRESH) {
    einkDisplay.requestResync(pendingX3SettlePasses > 1 ? pendingX3SettlePasses : 1);
  } else if (pendingX3SettlePasses > 0) {
    einkDisplay.requestResync(pendingX3SettlePasses);
  }
  pendingX3SettlePasses = 0;

  const unsigned long refreshStart = millis();
  einkDisplay.refreshDisplay(convertRefreshMode(mode), turnOffScreen);
  LOG_DBG("DISP", "#%lu refreshDisplay mode=%s took %lu ms", static_cast<unsigned long>(++panelSeq),
          mode == RefreshMode::FAST_REFRESH ? "FAST" : (mode == RefreshMode::HALF_REFRESH ? "HALF" : "FULL"),
          millis() - refreshStart);
}

void HalDisplay::deepSleep() {
  HalSpiBus::Lock spiLock;
  einkDisplay.deepSleep();
}

uint8_t* HalDisplay::getFrameBuffer() const { return einkDisplay.getFrameBuffer(); }

void HalDisplay::syncWriteBufferFromActive() const { einkDisplay.syncWriteBufferFromActive(); }

void HalDisplay::releaseBuffers() {
  HalSpiBus::Lock spiLock;
  einkDisplay.releaseBuffers();
}

// FBUF: centralized framebuffer-state trace. Every secondary-buffer / RED-RAM / single-buffer
// transition logs here so a ghosting regression can be tracked to the exact op that left the panel
// diffing against the wrong baseline. free=largest contiguous 8-bit block (what a realloc needs).
static uint32_t fbufContig() { return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT); }

// `redSynced=` in the FBUF lines is FreeInkDisplay's advisory RED-RAM flag. It exists for the
// SSD1677's host-managed previous-frame plane; on X3 the facade never sets it (its baseline is
// controller DTM1) and isRedRamSynced() hard-returns false there. Printing "0" on X3 reads as a
// fact about the panel and is not one -- it was misread as the driver's own sync flag once,
// which cost a debugging cycle. Say n/a where it means nothing.
static const char* redSyncedLabel(const bool hasRed, const bool synced) {
  return hasRed ? (synced ? "1" : "0") : "n/a";
}

bool HalDisplay::releaseSecondaryBuffer() {
  HalSpiBus::Lock spiLock;  // see the note above borrowSecondaryBuffer()
  // Double-release guard. releaseSecondaryBuffer() returning false means the
  // secondary buffer was already gone — i.e. a caller released it without
  // tracking that it had, then released again. The release windows here are
  // NOT lexically scoped (Background-C releases in buildSection and restores in
  // recoverSecondaryBufferIfNeeded(), across page turns), so a lost release-flag
  // is a real, plausible bug rather than a theoretical one. It is not fatal —
  // the buffer is already freed — but it signals confused ownership that can
  // strand the reader in degraded (single-buffer) mode, so log it loudly.
  if (!einkDisplay.hasSecondaryBuffer()) {
    LOG_INF("FBUF", "releaseSecondary called but secondary already released (double-release; caller lost track)");
    return false;
  }
  // Carry the DISPLAYED frame into the write buffer before the active one is freed.
  //
  // In dual-buffer mode the secondary buffer holds the frame on the panel and the write buffer
  // holds the frame from TWO refreshes ago (displayBuffer() ends in swapBuffers()). Freeing the
  // secondary without this leaves the write buffer holding that two-generations-stale frame while
  // the display is now in single-buffer mode -- where every other code path, including
  // reallocSecondaryBuffer()'s own re-seed ("correct when the caller reallocs before drawing the
  // next page (frameBuffer then still holds the on-screen frame)"), assumes the write buffer IS
  // the on-screen frame.
  //
  // A full render does not notice, because it clears first. A PARTIAL repaint does: it composites
  // onto whatever the write buffer holds, and syncWriteBufferFromActive() cannot rescue it either
  // -- that memcpy is gated on `frameBufferActive` being non-null, so once the secondary is gone
  // it silently does nothing. Symptom on device: an overlay drawn over the previous screen
  // instead of the current one, after any activity that releases the buffer (HomeActivity during
  // cover loading, the reader around a section build).
  //
  // One framebuffer-sized memcpy on a path that is about to spend hundreds of ms parsing CSS or
  // decoding a cover, so the cost does not register.
  einkDisplay.syncWriteBufferFromActive();
  const bool ok = einkDisplay.releaseSecondaryBuffer();
  LOG_INF("FBUF", "releaseSecondary -> %d (hasSecondary=%d redSynced=%s contig=%lu)", ok ? 1 : 0,
          einkDisplay.hasSecondaryBuffer() ? 1 : 0,
          redSyncedLabel(fastRefreshNeedsHostFrame(), einkDisplay.isRedRamSynced()),
          static_cast<unsigned long>(fbufContig()));
  return ok;
}

bool HalDisplay::reallocSecondaryBuffer() {
  HalSpiBus::Lock spiLock;  // see the note above borrowSecondaryBuffer()
  const bool ok = einkDisplay.reallocSecondaryBuffer();
  LOG_INF("FBUF", "reallocSecondary -> %d (hasSecondary=%d redSynced=%s contig=%lu)", ok ? 1 : 0,
          einkDisplay.hasSecondaryBuffer() ? 1 : 0,
          redSyncedLabel(fastRefreshNeedsHostFrame(), einkDisplay.isRedRamSynced()),
          static_cast<unsigned long>(fbufContig()));
  return ok;
}

bool HalDisplay::hasSecondaryBuffer() const { return einkDisplay.hasSecondaryBuffer(); }

// The four buffer-ownership transitions below all take the SPI bus lock, for two reasons that
// both trace back to them running on the LOOP task (reader background builds) while the render
// task drives the panel:
//
//  - borrowSecondaryBuffer() is not a pointer swap. It calls syncPendingAsync(), which runs the
//    driver's displayFinish() — real SPI traffic plus a BUSY wait. Unlocked, that collides with
//    the render task's own refresh and with SD transfers, which is precisely the unsynchronised
//    SdSpiCard failure this lock exists to prevent (see HalSpiBus.h).
//  - all four reassign frameBufferActive, which a deferred refresh still reads (X3's
//    post-waveform DTM1 sync). Serialising them behind the same lock the display paths take
//    means the swap cannot land mid-refresh.
//
// The mutex is recursive, so a caller that already holds it is unaffected, and the lock is
// uncontended on the normal path — this costs nothing when nothing else is on the bus.
uint8_t* HalDisplay::borrowSecondaryBuffer(size_t* size) {
  HalSpiBus::Lock spiLock;
  uint8_t* buf = einkDisplay.borrowSecondaryBuffer(size);
  LOG_INF("FBUF", "borrowSecondary -> %d (size=%lu hasSecondary=%d)", buf ? 1 : 0,
          static_cast<unsigned long>(buf && size ? *size : 0), einkDisplay.hasSecondaryBuffer() ? 1 : 0);
  return buf;
}

bool HalDisplay::returnSecondaryBuffer() {
  HalSpiBus::Lock spiLock;
  const bool ok = einkDisplay.returnSecondaryBuffer();
  LOG_INF("FBUF", "returnSecondary -> %d (hasSecondary=%d)", ok ? 1 : 0, einkDisplay.hasSecondaryBuffer() ? 1 : 0);
  return ok;
}

void HalDisplay::setSingleBufferFastDiff(bool enabled) {
  LOG_INF("FBUF", "singleBufferFastDiff=%d (hasSecondary=%d redSynced=%s)", enabled ? 1 : 0,
          einkDisplay.hasSecondaryBuffer() ? 1 : 0,
          redSyncedLabel(fastRefreshNeedsHostFrame(), einkDisplay.isRedRamSynced()));
  einkDisplay.setSingleBufferFastDiff(enabled);
}

void HalDisplay::triggerDisplay(RefreshMode mode, bool turnOffScreen) {
  HalSpiBus::Lock spiLock;

  // Record here too, not just in displayBuffer/refreshDisplay. The reader fires full
  // page renders through this path (triggerWithRefreshCycle), so without this
  // getLastRefreshMode() reports whatever the previous displayBuffer() did and the
  // "Page summary: refresh=..." line describes the wrong paint entirely.
  lastRefreshMode = mode;
  lastDisplayModeByte = refreshModeToByte(mode);

  LOG_DBG("DISP", "#%lu triggerDisplay mode=%s", static_cast<unsigned long>(++panelSeq),
          mode == RefreshMode::FAST_REFRESH ? "FAST" : (mode == RefreshMode::HALF_REFRESH ? "HALF" : "FULL"));
  einkDisplay.triggerDisplay(static_cast<EInkDisplay::RefreshMode>(mode), turnOffScreen);
}

void HalDisplay::completeDisplay() {
  HalSpiBus::Lock spiLock;
  einkDisplay.completeDisplay();
}

// The async split locks each half separately rather than spanning the pair.
// triggerDisplayAsync() returns while the waveform runs, and that gap is
// deliberately used for CPU/RAM work (see GfxRenderer.h and the inline-AA path
// in EpubReaderActivity) - holding the bus across it would serialize away the
// overlap the split exists to create. It is safe to drop the lock in between:
// during the waveform the controller scans its own RAM and the panel is not
// driving SPI.
void HalDisplay::triggerDisplayAsync(RefreshMode mode, bool turnOffScreen) {
  HalSpiBus::Lock spiLock;

  lastRefreshMode = mode;
  lastDisplayModeByte = refreshModeToByte(mode);

  LOG_DBG("DISP", "#%lu triggerDisplayAsync mode=%s", static_cast<unsigned long>(++panelSeq),
          mode == RefreshMode::FAST_REFRESH ? "FAST" : (mode == RefreshMode::HALF_REFRESH ? "HALF" : "FULL"));
  einkDisplay.triggerDisplayAsync(convertRefreshMode(mode), turnOffScreen);
}

bool HalDisplay::supportsAsyncRefresh() const { return einkDisplay.supportsAsyncRefresh(); }

void HalDisplay::finishDisplayAsync() {
  HalSpiBus::Lock spiLock;
  einkDisplay.finishDisplayAsync();
}

bool HalDisplay::isRefreshPending() const { return einkDisplay.isRefreshPending(); }
bool HalDisplay::isRedRamSynced() const { return einkDisplay.isRedRamSynced(); }

HalDisplay::RefreshMode HalDisplay::getLastRefreshMode() const { return lastRefreshMode; }

uint8_t HalDisplay::getLastDisplayModeByte() const { return lastDisplayModeByte; }

void HalDisplay::copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer) {
  HalSpiBus::Lock spiLock;
  einkDisplay.copyGrayscaleBuffers(lsbBuffer, msbBuffer);
}

void HalDisplay::copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer) {
  HalSpiBus::Lock spiLock;
  einkDisplay.copyGrayscaleLsbBuffers(lsbBuffer);
}

void HalDisplay::copyGrayscaleMsbBuffers(const uint8_t* msbBuffer) {
  HalSpiBus::Lock spiLock;
  einkDisplay.copyGrayscaleMsbBuffers(msbBuffer);
}

void HalDisplay::syncRedRamFromFrameBuffer() {
  // Only a panel that diffs against a host-supplied previous frame has a RED baseline to seed.
  // The SDK no-ops it elsewhere too, but returning here skips the bus lock and the log line.
  if (!fastRefreshNeedsHostFrame()) return;
  HalSpiBus::Lock spiLock;
  einkDisplay.syncRedRamFromFrameBuffer();
  LOG_INF("FBUF", "syncRedRamFromFrameBuffer (hasSecondary=%d redSynced=%s)", einkDisplay.hasSecondaryBuffer() ? 1 : 0,
          redSyncedLabel(fastRefreshNeedsHostFrame(), einkDisplay.isRedRamSynced()));
}

void HalDisplay::cleanupGrayscaleBuffers(const uint8_t* bwBuffer) {
  HalSpiBus::Lock spiLock;
  einkDisplay.cleanupGrayscaleBuffers(bwBuffer);
}

void HalDisplay::cleanupGrayscaleWithPreviousBuffer() {
  HalSpiBus::Lock spiLock;
  einkDisplay.cleanupGrayscaleWithPreviousBuffer();
}

bool HalDisplay::supportsGrayFrame() const { return einkDisplay.supportsGrayFrame(); }

bool HalDisplay::supportsAbsoluteGrayPlanes() const {
  return einkDisplay.grayscaleCapabilities(freeink::GrayscaleMode::Absolute).supported();
}

bool HalDisplay::beginAbsoluteGrayPass(const RefreshMode fallback, const bool turnOffScreen) {
  HalSpiBus::Lock spiLock;
  // The base push happens inside the SDK call, so record the mode the way every
  // other display path does — otherwise the next page summary reports whatever
  // the previous B/W push used. See the note on displayGrayscaleFrame().
  if (!einkDisplay.displayGrayscaleBase(freeink::GrayscaleMode::Absolute, convertRefreshMode(fallback),
                                        turnOffScreen)) {
    return false;
  }
  lastRefreshMode = fallback;
  lastDisplayModeByte = refreshModeToByte(fallback);
  return true;
}

void HalDisplay::triggerGrayscaleFrame(const RefreshMode refreshMode, const bool turnOffScreen) {
  HalSpiBus::Lock spiLock;
  lastRefreshMode = refreshMode;
  lastDisplayModeByte = refreshModeToByte(refreshMode);
  LOG_DBG(
      "DISP", "#%lu triggerGrayscaleFrame mode=%s", static_cast<unsigned long>(++panelSeq),
      refreshMode == RefreshMode::FAST_REFRESH ? "FAST" : (refreshMode == RefreshMode::HALF_REFRESH ? "HALF" : "FULL"));
  einkDisplay.triggerGrayscaleFrame(convertRefreshMode(refreshMode), turnOffScreen);
}

void HalDisplay::displayGrayscaleFrame(const RefreshMode refreshMode, const bool turnOffScreen) {
  HalSpiBus::Lock spiLock;
  einkDisplay.displayGrayscaleFrame(convertRefreshMode(refreshMode), turnOffScreen);
  // Record it like every other display path does. Without this the reader's page
  // summary reports whatever mode the last B/W push used — which on a device log
  // read "refresh=half" for pages the driver had just traced as going out on the
  // fast bank, and cost a round of chasing the wrong thing.
  lastRefreshMode = refreshMode;
  lastDisplayModeByte = refreshModeToByte(refreshMode);
}

uint8_t HalDisplay::getGrayLevels() const { return einkDisplay.grayLevels(); }

uint8_t* HalDisplay::borrowGray8Canvas(uint16_t* stride) {
  HalSpiBus::Lock spiLock;
  return einkDisplay.borrowGray8Canvas(stride);
}

void HalDisplay::displayGray8Canvas(RefreshMode refreshMode, bool turnOffScreen) {
  HalSpiBus::Lock spiLock;
  LOG_DBG("DISP", "#%lu displayGray8Canvas levels=%u", static_cast<unsigned long>(++panelSeq),
          static_cast<unsigned>(einkDisplay.grayLevels()));
  einkDisplay.displayGray8Canvas(convertRefreshMode(refreshMode), turnOffScreen);
  // Record it like every other display path: the driver substitutes a clean-bank
  // refresh, so the page summary must not claim whatever mode the caller asked
  // for. See the note on displayGrayscaleFrame().
  lastRefreshMode = RefreshMode::FULL_REFRESH;
  lastDisplayModeByte = refreshModeToByte(RefreshMode::FULL_REFRESH);
}

void HalDisplay::displayGrayBuffer(bool turnOffScreen) {
  HalSpiBus::Lock spiLock;
  // Give the driver a real BW base when it actually reads one.
  //
  // displayGrayBuffer() hands the driver `frameBuffer`, and the plane dance in
  // GfxRenderer::renderGrayscalePlanesSequential() leaves the LAST PLANE there:
  // it clears to 0x00, renders text-only, copies the plane out, and calls this.
  // Ssd1677Driver::displayGray() opens with `(void)fb` -- the planes are already
  // in controller RAM and the BW page is retained by the panel -- so nothing ever
  // noticed that the buffer holds a plane rather than a page.
  //
  // LgfxEpdDriver USED TO have no controller-side image to fall back on:
  // fillCanvasGray() rebuilt every pixel from the base, mapping a clear base bit to
  // kGrayBlack, so a text-only plane painted the whole background black and left
  // only the AA marks standing -- the inversion first seen on the T5S3.
  //
  // Reseed the write buffer from the on-screen frame first, so the base is the BW
  // page the panel is actually showing. Gated on the controller so the panels that
  // ignore the argument do not pay a full-buffer copy per AA pass.
  //
  // NOTE (Free-Ink/freeink-sdk#47, merged): the driver no longer reads `fb` at all
  // -- displayGray() is now `(void)fb; overlayCanvasGray();`, composing onto LGFX's
  // live canvas. So this reseed no longer serves the purpose it was written for and
  // is a candidate for deletion.
  //
  // Kept for now deliberately, not by oversight. The reseed touches the HOST write
  // framebuffer, which is a different object from the LGFX canvas the overlay
  // composes onto, and whether the plane-restore step in
  // GfxRenderer::renderGrayscalePlanesSequential() leans on it cannot be settled
  // without the panel. The cost is one buffer copy per AA pass on one board; the
  // downside if the reasoning is wrong is the inversion bug coming back. Remove it
  // behind a device test, not behind an argument.
  if (BoardConfig::ACTIVE.displayController == BoardConfig::DisplayController::LgfxEpd) {
    einkDisplay.syncWriteBufferFromActive();
  }
  einkDisplay.displayGrayBuffer(turnOffScreen);
}

void HalDisplay::displayWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool turnOffScreen) {
  HalSpiBus::Lock spiLock;
  LOG_DBG("DISP", "#%lu displayWindow x=%u y=%u w=%u h=%u", static_cast<unsigned long>(++panelSeq), x, y, w, h);
  einkDisplay.displayWindow(x, y, w, h, turnOffScreen);
}

bool HalDisplay::fastRefreshNeedsHostFrame() const { return einkDisplay.fastRefreshNeedsHostFrame(); }

bool HalDisplay::asyncRefreshHoldsFrameBuffer() const { return einkDisplay.asyncRefreshHoldsFrameBuffer(); }

bool HalDisplay::ghostsUnderFastRefresh() const { return einkDisplay.ghostsUnderFastRefresh(); }

void HalDisplay::setFastGrayscaleLut(bool fast) { einkDisplay.setFastGrayscaleLut(fast); }

bool HalDisplay::getFastGrayscaleLut() const { return einkDisplay.getFastGrayscaleLut(); }

uint16_t HalDisplay::getDisplayWidth() const { return einkDisplay.getDisplayWidth(); }

uint16_t HalDisplay::getDisplayHeight() const { return einkDisplay.getDisplayHeight(); }

uint16_t HalDisplay::getDisplayWidthBytes() const { return einkDisplay.getDisplayWidthBytes(); }

uint32_t HalDisplay::getBufferSize() const { return einkDisplay.getBufferSize(); }
