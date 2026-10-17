#pragma once
#include <Logging.h>

#include <cassert>
#include <memory>
#include <string>
#include <utility>

#include "ActivityManager.h"  // for using the ActivityManager singleton
#include "ActivityResult.h"
#include "ButtonEventManager.h"
#include "GfxRenderer.h"
#include "ListRowTap.h"
#include "MappedInputManager.h"
#include "RenderLock.h"

class Activity {
  friend class ActivityManager;

 protected:
  std::string name;
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;
  ButtonEventManager& buttonEvents;

  ActivityResultHandler resultHandler;
  ActivityResult result;
  ListRowTap::ActivationState listTapActivation;

 private:
  // Set by ActivityManager when releaseBuriedState() dropped something: this activity can not be
  // drawn again, so the manager reboots to Settings rather than resume it.
  bool buriedStateReleased = false;

 public:
  enum class ListPageDirection : uint8_t { Back, Forward };

  explicit Activity(std::string name, GfxRenderer& renderer, MappedInputManager& mappedInput)
      : name(std::move(name)), renderer(renderer), mappedInput(mappedInput), buttonEvents(globalButtonEvents()) {}
  virtual ~Activity() = default;
  const std::string& getName() const { return name; }
  virtual void onEnter();
  virtual void onExit();
  virtual void loop() {}

  virtual void render(RenderLock&&) {}

  // If immediate is true, the update will be triggered immediately.
  // Otherwise, it will be deferred until the end of the current loop iteration.
  virtual void requestUpdate(bool immediate = false);

  // Request an immediate render and block until it completes.
  virtual void requestUpdateAndWait();

  // True when another render has already been queued while this one is still running.
  // See ActivityManager::isUpdateSuperseded() for the contract — advisory only, and
  // meant to be read from render() as late as possible.
  bool isUpdateSuperseded() const;

  virtual bool skipLoopDelay() { return false; }
  // preventAutoSleep() means "busy": it holds off the sleep timeout AND keeps the CPU at full
  // speed, because main.cpp treats it as user activity. keepAwake() is the idle counterpart:
  // the device must not time out to sleep, but nothing is running between events, so the idle
  // governor may still downclock and light-sleep (e.g. reader auto page turn, issue #293).
  virtual bool preventAutoSleep() { return false; }
  virtual bool keepAwake() { return false; }
  // True while the activity has handed the raw SD card to something outside the
  // firmware (USB Drive). The filesystem is unmounted for the duration, so both
  // main.cpp's loop and ActivityManager's stand down: no screenshots, no sleep,
  // no button shortcuts, no navigation. Such an activity leaves by rebooting,
  // never by transitioning — a transition would let filesystem users back in
  // while the host still owns the sectors.
  virtual bool requiresExclusiveStorageLoop() const { return false; }
  virtual bool isReaderActivity() const { return false; }
  virtual bool usesWifi() const { return false; }

  // What a tap on the row at `index` should do, moving this screen's selection if it lands on a
  // new row. See ListRowTap.h for the rule; most implementations are one call to
  // ListRowTap::apply().
  //
  // `Selected` means the highlight moved; the dispatcher applies the user's tap preference and
  // either repaints or arms the row for a second tap. `Activate` means the tapped row was already
  // selected; it still requires a preceding tap in two-step mode because keyboard focus is not
  // touch confirmation. Activation synthesizes Confirm so a tap runs the SAME handler the button
  // does rather than a second copy that can drift.
  //
  // Screens whose list is not a plain recorded row band -- Home's covers and menu, RecentBooks'
  // cover grid -- consume the tap in their own loop() instead and leave this alone. They follow
  // the same two-step rule there.
  virtual ListRowTap::Result selectListRow(int /*index*/) { return ListRowTap::Result::Rejected; }

  // Handle a touch page gesture without translating it into a physical button. Activities whose
  // Left/Right buttons mean something other than list paging override this boundary.
  virtual bool pageList(ListPageDirection /*direction*/) { return false; }

  // Return true while this activity owns the raw serial input stream (e.g. the
  // USB serial file-transfer activity reading a binary protocol). When true,
  // main.cpp's line-based `CMD:` handler skips reading logSerial so it can't
  // steal bytes from the activity's byte stream. Default: false.
  virtual bool ownsSerialInput() const { return false; }

  // Called before something captures the raw frame buffer (e.g. a screenshot) outside the
  // normal render flow. An activity that may leave content other than what is on screen in
  // the frame buffer (e.g. the reader's pre-rendered next page) must redraw the visible page
  // here so the capture matches the display. Default is a no-op.
  virtual void prepareFramebufferForCapture() {}

  // Appends what a test script needs to know about this activity's state to `out`, as
  // space-separated key=value pairs, for the serial CMD:STATE. Called under the render lock.
  // Write each pair with SerialCommandGrammar::appendField(), which quotes a value with spaces.
  // Default: nothing beyond the activity's name, which the caller already writes.
  virtual void appendDebugState(std::string& /*out*/) const {}

  // True while work the screen does on its own, without input, is running or about to start: a
  // section build, a cover decode. The serial CMD:IDLE waits for it to end. Called from the loop
  // task WITHOUT the render lock; an activity that must lock to answer should Try and report busy
  // when it cannot. Not skipLoopDelay(): the transfer screens hold that for their whole life.
  virtual bool hasPendingBackgroundWork() { return false; }

  // Called while this activity is buried on the stack, before a download that needs the heap in
  // one piece (releaseMemoryForDownload()). Drop what the activity built only to draw itself, such
  // as row lists and labels; the object must stay valid to destroy. Return true if anything was
  // dropped: the activity can then not be shown again, so ActivityManager reboots to Settings
  // rather than resume it. Every caller of the release reboots on exit anyway; that is the net.
  virtual bool releaseBuriedState() { return false; }

  // True when transitions INTO or OUT OF this activity must not paint a busy indicator. Checked
  // on both ends, because the reasons run both directions:
  //
  //  - Sleep and the frontlight drawer paint by COMPOSITING onto whatever the write framebuffer
  //    already holds. An indicator syncs the displayed frame into that buffer and swaps, so it
  //    changes the very thing they are about to composite onto -- the sleep cover then appears
  //    over the previous screen plus the indicator.
  //  - Boot already shows a splash, so marking the hand-off to Home paints over it for nothing.
  //  - Leaving a lightweight overlay is fast by construction; there is no wait to acknowledge.
  virtual bool suppressesBusyIndicator() const { return false; }

  // Return true to suppress the minute-tick requestUpdate() from ActivityManager when nothing
  // status-bar-relevant has changed since the last render. Skipping avoids a no-op page render
  // followed by a no-diff e-ink refresh, which on X3 panels accumulates visible speckle.
  virtual bool shouldSkipPeriodicUpdate() const { return false; }

  // Called by ActivityManager when a globally-configured button action targets the
  // current activity. Override in reader activities to handle reader-specific actions.
  // Non-reader activities can ignore this (default is no-op).
  virtual void onButtonAction(CrossPointSettings::BUTTON_ACTION) {}

  // Whether a reader-scoped `action` configured on `button` comes here through onButtonAction()
  // although this is not the reader. Off that screen the press normally falls through raw, so the
  // screen's own use of the button wins (see main.cpp) -- which leaves a button it has no use for
  // doing nothing. A paginated screen opened from the reader opts in for the page turns on such a
  // button, so a Power button set to turn pages turns its pages too.
  virtual bool takesButtonAction(CrossPointSettings::BUTTON_ACTION, MappedInputManager::Button) const { return false; }

  // A manual force refresh (a button or gesture bound to Force Refresh) while this activity is on
  // top. Return true when the activity repaints itself in `mode`; false lets main.cpp re-flush the
  // displayed frame as it stands. Override when that frame is more than the B/W buffer -- an
  // anti-aliased or grayscale page, whose gray planes a raw flush would strip -- or when the write
  // buffer may hold something other than the screen, such as the reader's pre-rendered next page.
  // Ported from crosspoint-reader PR #2683 ("fix: restore antialiasing after manual refresh",
  // @thiagokokada): the hook and the raw-flush fallback are theirs; the mode parameter (for the
  // separate HALF and FAST actions) and the per-reader overrides are ours.
  virtual bool handleForcedRefresh(HalDisplay::RefreshMode /*mode*/) { return false; }

  // Start a new activity without destroying the current one
  // Note: requestUpdate() will be invoked automatically once resultHandler finishes
  //
  // Virtual because a suspended activity keeps whatever it was holding: the stack keeps this
  // object alive but stops calling loop(), so any background work it owns freezes in place
  // without releasing its resources. An activity that lends out a shared resource must hand it
  // back here — see EpubReaderActivity, whose look-ahead build borrows the display's secondary
  // framebuffer and silently degraded every refresh the child activity drew.
  virtual void startActivityForResult(std::unique_ptr<Activity>&& activity, ActivityResultHandler resultHandler);

  // Set the result to be passed back to the previous activity when this activity finishes
  void setResult(ActivityResult&& result);

  // Finish this activity and return to the previous one on the stack (if any)
  void finish();

  // Convenience method to facilitate API transition to ActivityManager
  // TODO: remove this in near future
  void onGoHome();
};
