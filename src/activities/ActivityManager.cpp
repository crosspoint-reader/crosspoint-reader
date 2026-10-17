#include "ActivityManager.h"

#include <Arduino.h>
#include <BoardConfig.h>  // FREEINK_CAP_USB_MSC
#include <HalClock.h>
#include <HalPowerManager.h>
#include <Logging.h>
#include <Memory.h>  // makeUniqueNoThrow
#include <esp_heap_caps.h>
#include <esp_system.h>

#include <algorithm>
#include <cstdlib>

#include "CrossPointState.h"
#include "OpdsServerStore.h"
#include "SdCardFontGlobals.h"
#include "SettingsList.h"
#include "SilentRestart.h"
#include "boot_sleep/BootActivity.h"
#include "boot_sleep/SleepActivity.h"
#include "browser/OpdsBookBrowserActivity.h"
#include "components/UITheme.h"
#include "components/themes/ButtonHintStrip.h"
#include "components/themes/ListTouchBand.h"
#include "components/themes/TapTargets.h"
#include "home/FileBrowserActivity.h"
#include "home/GlobalBookmarksActivity.h"
#include "home/HomeActivity.h"
#include "home/HomeMoreActivity.h"
#include "network/CrossPointWebServerActivity.h"
#include "network/SerialTransferActivity.h"
#include "network/UsbDriveActivity.h"
#include "reader/KOReaderSyncActivity.h"
#include "reader/ReaderActivity.h"
#include "settings/ClockSettingsActivity.h"
#include "settings/KOReaderSettingsActivity.h"
#include "settings/OpdsServerListActivity.h"
#include "settings/ReadingStatsActivity.h"
#include "settings/SettingsActivity.h"
#include "settings/SettingsSubmenuActivity.h"
#include "util/FrontlightPanelActivity.h"
#include "util/FullScreenMessageActivity.h"
#include "util/SerialCommandGrammar.h"
#include "weather/WeatherActivity.h"

// Guards ActivityManager's cross-task state (requestedUpdate, waitingTaskHandle),
// which the loop task and the render task both touch.
//
// This used to pass a null mux. That is a latent bug that only
// bites on a dual-core part: on the single-core ESP32-C3 the port ignores the
// mux and just disables interrupts, so a null pointer is harmless — but on the
// dual-core ESP32-S3 the same macro performs a real spinlock_acquire() and
// asserts the lock is non-null. It boot-looped the X4 Pro with
// "assert failed: spinlock_acquire spinlock.h:84 (lock)".
//
// One mux for all four sections is deliberate: they are short, they guard the
// same handful of fields, and sharing one preserves the mutual exclusion the
// nullptr form gave them (a global interrupt disable made every section
// exclusive with every other).
static portMUX_TYPE activityStateMux = portMUX_INITIALIZER_UNLOCKED;

#ifndef DEBUG_MEMORY_CONSUMPTION
#define DEBUG_MEMORY_CONSUMPTION 0
#endif

void ActivityManager::begin() {
  // Create FreeRTOS objects here rather than in the constructor: ActivityManager
  // is a global, and its constructor runs before the scheduler starts. Calling
  // xSemaphoreCreateMutex() that early corrupts the TLSF heap metadata.
  renderingMutex = xSemaphoreCreateMutex();
  assert(renderingMutex != nullptr && "Failed to create rendering mutex");

  // 10 KB: the render task runs the firmware's deepest call chains — page build/parse, CSS
  // resolve, image decode (JPEG/PNG) + dither — and its stack abuts the heap top, so an
  // overflow spills into the heap and surfaces as a corruption assert elsewhere (see the
  // documented hazard in EpubReaderActivity). Field high-water dipped to ~1.7 KB free of the
  // former 8 KB on a parse-on-render-task pass; +2 KB restores a safer margin. renderTaskLoop()
  // also warns (RENDER_STACK_WARN_BYTES) if the live margin ever gets thin again.
  // Pin the render task to CPU 1 on dual-core parts (S3), CPU 0 on single-core
  // (C3, where this is the only choice and the call is equivalent to
  // xTaskCreate). Ported from upstream/develop, whose comment gives the reason:
  // keep long renders and cover decodes off CPU 0's idle watchdog. CPU 0 also
  // runs the Arduino loop task and the system/WiFi tasks, so a multi-second page
  // build or JPEG decode landing there can starve the idle task and trip the
  // watchdog. On the C3 this changes nothing.
#if defined(configNUM_CORES) && configNUM_CORES > 1
  constexpr BaseType_t renderTaskCore = 1;
#else
  constexpr BaseType_t renderTaskCore = 0;
#endif
  xTaskCreatePinnedToCore(&renderTaskTrampoline, "ActivityManagerRender",
                          10240,              // Stack size (bytes)
                          this,               // Parameters
                          1,                  // Priority
                          &renderTaskHandle,  // Task handle
                          renderTaskCore);
  assert(renderTaskHandle != nullptr && "Failed to create render task");
}

#if DEBUG_MEMORY_CONSUMPTION
static void logActivityStackState(const char* stage, Activity* currentActivity, size_t stackSize) {
  const uint32_t freeHeap = esp_get_free_heap_size();
  const uint32_t contigHeap = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
  LOG_DBG("ACT", "%s: current=%s stackSize=%zu free=%lu contig=%lu", stage,
          currentActivity ? currentActivity->getName().c_str() : "<none>", stackSize, freeHeap, contigHeap);
}
#else
static inline void logActivityStackState(const char*, Activity*, size_t) {}
#endif

void ActivityManager::renderTaskTrampoline(void* param) {
  auto* self = static_cast<ActivityManager*>(param);
  self->renderTaskLoop();
}

void ActivityManager::renderTaskLoop() {
#ifdef ENABLE_BOOT_HEAP_DIAGNOSTICS
  // The kernel's CONFIG_FREERTOS_WATCHPOINT_END_OF_STACK is OFF in the precompiled
  // arduino-esp32 IDF libs and can't be flipped without rebuilding IDF. The canary
  // (configCHECK_FOR_STACK_OVERFLOW==2) IS on but only fires at a context switch —
  // too late here, because a deep render excursion spills into the heap and the next
  // synchronous free trips multi_heap poisoning before any yield. Install the
  // end-of-stack hardware watchpoint ourselves on THIS task so an overflow faults at
  // the exact instruction that writes past the stack limit, yielding a backtrace at
  // the offending frame. Single-core C3: one set persists (the kernel does not re-arm
  // watchpoints on context switch when the Kconfig option is off), and the watched
  // address belongs only to this task's stack, so other tasks can't false-trigger it.
  vPortSetStackWatchpoint(pxTaskGetStackStart(nullptr));
#endif
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    // Acquire the lock before reading currentActivity to avoid a TOCTOU race
    // where the main task deletes the activity between the null-check and render().
    RenderLock lock;
    if (currentActivity) {
      HalPowerManager::Lock powerLock;  // Ensure we don't go into low-power mode while rendering
      // Publish "a pass is in flight" from INSIDE the lock, so a transition holding the mutex
      // can read it as proof that no pass can begin behind its back. It stays set across the
      // window where render() drops the mutex (renderContents' pre-waveform unlock), which is
      // exactly the window a plain RenderLock cannot see — see RenderLock(ExclusiveActivityAccess).
      renderPassActive.store(true, std::memory_order_release);
      renderPassesStarted.fetch_add(1, std::memory_order_relaxed);
      // Every touch recorder starts each pass empty, so what they hold afterwards is exactly
      // what THIS frame painted. Clearing on activity transitions alone is not enough: a screen
      // can stop drawing its list without changing activity (WifiSelection swaps the network
      // list for a "no networks" message; FontDownload for a progress screen), and the band
      // recorded before that would keep answering taps against rows nothing paints any more.
      // Cleared here rather than in each conditional screen because "the recorders describe the
      // last frame" is a property of the render pass, not something 26 screens should each
      // remember.
      ListTouchBand::invalidate();
      TapTargets::homeCovers().invalidate();
      TapTargets::homeMenu().invalidate();
      // readerLinks is deliberately NOT cleared here. The reader has render passes that do not
      // put a page on screen -- above all the pre-render, which draws the NEXT page into the
      // frame buffer while the current one is still displayed -- and clearing per pass would
      // make the visible page's links dead until something forced a full render. It is cleared
      // on activity transitions like the rest, and every path that displays a page republishes
      // it (with an empty set when the page has no links), so a displayed page always describes
      // itself.
      currentActivity->render(std::move(lock));
      // Cleared unconditionally on every exit path of render(): the call cannot throw
      // (-fno-exceptions) and every `return` inside it lands here.
      renderPassActive.store(false, std::memory_order_release);
      // Always-on guard (cheap; one read): warn once if the render task's stack high-water gets
      // dangerously thin. The render task's stack abuts the heap top — an overflow spills into
      // the heap and shows up as an unrelated corruption assert. Tracks the running minimum so
      // it only logs when a new low crosses the threshold (no per-pass spam). uxTaskGetStack...
      // returns bytes on the C3 (StackType_t == uint8_t).
      {
        static constexpr unsigned RENDER_STACK_WARN_BYTES = 1536;
        static unsigned lowestRenderStackFree = UINT_MAX;
        const unsigned stackFree = static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr));
        if (stackFree < lowestRenderStackFree) {
          lowestRenderStackFree = stackFree;
          if (stackFree < RENDER_STACK_WARN_BYTES) {
            LOG_ERR("MEM", "Render task stack LOW: %u bytes free (new min) — stack-into-heap overflow risk", stackFree);
          }
        }
      }
#ifdef ENABLE_BOOT_HEAP_DIAGNOSTICS
      // Render runs the deepest call chains in the firmware (page build + image
      // decode + dither) on this task's fixed 8 KB stack, which is heap-allocated
      // at the top of RAM abutting the heap's top block. If a render pass overflows
      // it spills downward into that block, surfacing later as a lazy multi_heap
      // poisoning assert on an unrelated free (e.g. a JPEG cleanup). The high-water
      // mark is the MINIMUM free stack ever seen by this task, so sampling it right
      // after render() returns captures the deepest excursion of the pass just run.
      // bytes (StackType_t == uint8_t on the C3). Approaching 0 ⇒ overflow.
      // Emit the stack high-water on its own line BEFORE the heap walk: if the
      // corruption is a stack-into-heap spill, heap_caps_check_integrity_all() can
      // itself fault while chasing trampled TLSF pointers (see the warning on
      // checkHeapIntegrity in EpubReaderActivity), which would swallow this number.
      const unsigned stackFreeBytes = static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr));
      LOG_INF("MEM", "Render pass done [%s]: stack high-water=%u bytes free (min ever)",
              currentActivity->getName().c_str(), stackFreeBytes);
      if (!heap_caps_check_integrity_all(/*print_errors=*/true)) {
        LOG_ERR("MEM", "HEAP CORRUPT immediately after render pass <-- the render task is the writer");
      }
#endif
    }
    // Notify any task blocked in requestUpdateAndWait() that the render is done.
    TaskHandle_t waiter = nullptr;
    taskENTER_CRITICAL(&activityStateMux);
    waiter = waitingTaskHandle;
    waitingTaskHandle = nullptr;
    taskEXIT_CRITICAL(&activityStateMux);
    if (waiter) {
      xTaskNotify(waiter, 1, eIncrement);
    }
  }
}

void ActivityManager::loop() {
  framebufferPreparedThisTick = false;

  if (currentActivity && currentActivity->requiresExclusiveStorageLoop()) {
    // The raw SD card belongs to a USB host right now. Run the activity and
    // flush its renders, and NOTHING else: no list/hint taps, no minute-tick
    // clock repaint (it reads settings and would race the detached volume), and
    // above all no pending-action processing — a transition would construct the
    // next activity, which reads the filesystem that no longer exists. Such an
    // activity leaves by rebooting, so nothing here needs a transition path.
    currentActivity->loop();
    if (requestedUpdate) {
      taskENTER_CRITICAL(&activityStateMux);
      requestedUpdate = false;
      taskEXIT_CRITICAL(&activityStateMux);
      if (renderTaskHandle) xTaskNotify(renderTaskHandle, 1, eIncrement);
    }
    return;
  }

  // Drain leftover input after an activity transition so that the button press/release
  // used to leave one activity cannot bleed into the next.  We consume events until
  // every button is released and no press/release edges remain.
  if (drainInput) {
    if (mappedInput.wasAnyPressed() || mappedInput.wasAnyReleased() ||
        mappedInput.isPressed(MappedInputManager::Button::Back) ||
        mappedInput.isPressed(MappedInputManager::Button::Confirm) ||
        mappedInput.isPressed(MappedInputManager::Button::Left) ||
        mappedInput.isPressed(MappedInputManager::Button::Right) ||
        mappedInput.isPressed(MappedInputManager::Button::Up) ||
        mappedInput.isPressed(MappedInputManager::Button::Down)) {
      // Still have pending input — skip the activity loop but continue with
      // the rest (pending-action processing, render flushing) so that
      // transitions and screen updates are not delayed.
    } else {
      drainInput = false;
    }
  }

  if (!drainInput && currentActivity) {
    if (mappedInput.wasAnyPressed() || mappedInput.wasAnyReleased()) {
      currentActivity->listTapActivation.reset();
    }
    // Note: do not hold a lock here, the loop() method must be responsible for acquire one if needed
    currentActivity->loop();
    // Swipe before tap: a swipe ends in a release like a tap does, and the SDK reports both for
    // the same contact when the travel sits near the threshold. Resolving the swipe first (it
    // suppresses the contact when it claims one) stops a marginal drag from BOTH moving the
    // selection and paging the list. Same ordering, and the same reason, as GestureEventManager.
#if CP_TOUCH_UI
    // Before the list dispatchers: a top-edge pull is an edge gesture, and a list occupying
    // the whole screen would otherwise read it as a page-down first.
    dispatchLightPanelGesture();
    dispatchListSwipe();
    dispatchListTap();
    dispatchHintStripTap();
#endif
  }

  if (SETTINGS.useClock && HalClock::isSynced()) {
    time_t now = HalClock::now();
    if (now > 0) {
      static time_t lastMinute = -1;
      time_t minute = now / 60;
      if (minute != lastMinute) {
        lastMinute = minute;
        if (!currentActivity || !currentActivity->shouldSkipPeriodicUpdate()) {
          requestUpdate();
        }
      }
    }
  }

  if (pendingAction != PendingAction::None) {
    showBusyIndicator();
  }

  while (pendingAction != PendingAction::None) {
    if (pendingAction == PendingAction::Pop) {
      // Exclusive: this branch destroys currentActivity, so it must also outwait an
      // in-flight render pass, not just the mutex.
      RenderLock lock{RenderLock::ExclusiveActivityAccess{}};

      if (!currentActivity) {
        // Should never happen in practice
        LOG_ERR("ACT", "Pop set but currentActivity is null; ignoring pop request");
        pendingAction = PendingAction::None;
        continue;
      }

      const bool exitingReader = currentActivity->isReaderActivity();
      ActivityResult pendingResult = std::move(currentActivity->result);

      // Destroy the current activity
      exitActivity(lock);
      pendingAction = PendingAction::None;

      if (stackActivities.empty()) {
        LOG_DBG("ACT", "No more activities on stack, returning from child");
        if (exitingReader) {
          unloadSdFontIfLoaded();
        }
        lock.unlock();  // returnFromChild may acquire its own lock via replaceActivity
        returnFromChild();
        continue;  // Will launch the target activity immediately

      } else {
        if (stackActivities.back()->buriedStateReleased) {
          // The activity below dropped its rows for a download; it cannot be drawn again. Every
          // download that releases reboots in its own onExit(), so this is only the net.
          LOG_ERR("ACT", "Resuming an activity whose state was released; rebooting to Settings");
          SETTINGS.saveToFile();
          silentRestartToSettings();
        }
        currentActivity = std::move(stackActivities.back());
        stackActivities.pop_back();
        refreshWifiActivityFlag();
        noteActivityChanged();
        if (exitingReader && !currentActivity->isReaderActivity()) {
          unloadSdFontIfLoaded();
        }
        LOG_DBG("ACT", "Popped from activity stack, new size = %zu", stackActivities.size());
        // Handle result if necessary
        if (currentActivity->resultHandler) {
          LOG_DBG("ACT", "Handling result for popped activity");

          // Move it here to avoid the case where handler calling another startActivityForResult()
          auto handler = std::move(currentActivity->resultHandler);
          currentActivity->resultHandler = nullptr;
          lock.unlock();  // Handler may acquire its own lock
          handler(pendingResult);
        }

        // Arm input drain so the button that triggered the pop doesn't bleed into the
        // restored activity (or into a new activity the handler just pushed).
        drainInput = true;
        buttonEvents.drain();

        // Request an update to ensure the popped activity gets re-rendered
        if (pendingAction == PendingAction::None) {
          requestUpdate();
        }

        // Handler may request another pending action, we will handle it in the next loop iteration
        continue;
      }

    } else if (pendingActivity) {
      // Current activity has requested a new activity to be launched
      // Exclusive: the Replace path destroys currentActivity (and the Push path hands it to the
      // stack while the render task may still hold a pointer to it), so both need the render
      // pass to have finished, not merely the mutex to be free.
      RenderLock lock{RenderLock::ExclusiveActivityAccess{}};

      const bool enteringReader = pendingActivity->isReaderActivity();
      const bool exitingReader = currentActivity && currentActivity->isReaderActivity();
      const bool shouldUnloadSdFont = exitingReader && !enteringReader;

      if (pendingAction == PendingAction::Replace) {
#if DEBUG_MEMORY_CONSUMPTION
        logActivityStackState("replace_before", currentActivity.get(), stackActivities.size());
#endif
        // Destroy the current activity
        exitActivity(lock);
        // Clear the stack
        while (!stackActivities.empty()) {
          stackActivities.back()->onExit();
          stackActivities.pop_back();
        }
        if (shouldUnloadSdFont) {
          unloadSdFontIfLoaded();
        }
#if DEBUG_MEMORY_CONSUMPTION
        logActivityStackState("replace_after_clear", nullptr, stackActivities.size());
#endif
      } else if (pendingAction == PendingAction::Push) {
#if DEBUG_MEMORY_CONSUMPTION
        logActivityStackState("push_before", currentActivity.get(), stackActivities.size());
#endif
        // Move current activity to stack
        stackActivities.push_back(std::move(currentActivity));
#if DEBUG_MEMORY_CONSUMPTION
        LOG_DBG("ACT", "Pushed to activity stack, new size = %zu", stackActivities.size());
        logActivityStackState("push_after", currentActivity.get(), stackActivities.size());
#else
        LOG_DBG("ACT", "Pushed to activity stack, new size = %zu", stackActivities.size());
#endif
      }
      pendingAction = PendingAction::None;
      currentActivity = std::move(pendingActivity);
      refreshWifiActivityFlag();
      noteActivityChanged();

      lock.unlock();  // onEnter may acquire its own lock
      currentActivity->onEnter();

      // Arm input drain so the button that triggered the transition doesn't bleed
      // into the new activity.
      drainInput = true;
      buttonEvents.drain();

      // onEnter may request another pending action, we will handle it in the next loop iteration
      continue;
    }
  }

  if (requestedUpdate) {
    taskENTER_CRITICAL(&activityStateMux);
    requestedUpdate = false;
    taskEXIT_CRITICAL(&activityStateMux);
    // Using direct notification to signal the render task to update
    // Increment counter so multiple rapid calls won't be lost
    if (renderTaskHandle) {
      xTaskNotify(renderTaskHandle, 1, eIncrement);
    }
  }
}

void ActivityManager::exitActivity(const RenderLock& lock) {
  // Note: lock must be held by the caller
  if (currentActivity) {
    currentActivity->onExit();
    currentActivity.reset();
    refreshWifiActivityFlag();
  }
}

void ActivityManager::refreshWifiActivityFlag() {
  // The whole stack, not just the top: a network activity with a prompt or a picker above it still
  // owns the radio -- it brought it up and takes it down in its own onExit().
  bool uses = currentActivity && currentActivity->usesWifi();
  for (const auto& buried : stackActivities) uses = uses || buried->usesWifi();
  activityUsesWifi.store(uses, std::memory_order_relaxed);
}

void ActivityManager::replaceActivity(std::unique_ptr<Activity>&& newActivity) {
  // Note: no lock here, this is usually called by loop() and we may run into deadlock
  if (currentActivity) {
    // Defer launch if we're currently in an activity, to avoid deleting the current activity
    // leading to the "delete this" problem
#if DEBUG_MEMORY_CONSUMPTION
    LOG_DBG("ACT", "replaceActivity requested: current=%s stackSize=%zu", currentActivity->getName().c_str(),
            stackActivities.size());
#endif
    pendingActivity = std::move(newActivity);
    pendingAction = PendingAction::Replace;
  } else {
    // No current activity, safe to launch immediately
    currentActivity = std::move(newActivity);
    refreshWifiActivityFlag();
    noteActivityChanged();
    currentActivity->onEnter();
  }
}

void ActivityManager::goToFileTransfer(const NetworkMode selected) {
  replaceActivity(std::make_unique<CrossPointWebServerActivity>(renderer, mappedInput, selected));
}

void ActivityManager::goToSerialTransfer() {
  replaceActivity(std::make_unique<SerialTransferActivity>(renderer, mappedInput));
}

void ActivityManager::goToUsbDrive() {
#if FREEINK_CAP_USB_MSC
  auto activity = makeUniqueNoThrow<UsbDriveActivity>(renderer, mappedInput);
  if (!activity) {
    LOG_ERR("ACT", "OOM: USB Drive activity");
    return;
  }
  replaceActivity(std::move(activity));
#else
  LOG_ERR("ACT", "USB Drive requested in a build without the USB Drive capability");
#endif
}

void ActivityManager::goToSettings(const int tab) {
  replaceActivity(std::make_unique<SettingsActivity>(renderer, mappedInput, tab));
}

void ActivityManager::goToClockSettings() {
  replaceActivity(std::make_unique<ClockSettingsActivity>(renderer, mappedInput));
}

void ActivityManager::goToKOReaderSettings() {
  replaceActivity(std::make_unique<KOReaderSettingsActivity>(renderer, mappedInput));
}

// The Library is the file browser in one of its four tab modes; every tab has the same views, keys
// and Options.
void ActivityManager::goToLibrary(const LibraryTab tab, std::string path, std::string focusName,
                                  const std::optional<uint32_t> author) {
  auto library = std::make_unique<FileBrowserActivity>(renderer, mappedInput, std::move(path), std::move(focusName),
                                                       FileBrowserActivity::modeFor(tab));
  if (author) library->openAuthorOnEnter(*author);
  replaceActivity(std::move(library));
}

void ActivityManager::goToGlobalBookmarks() { goToGlobalBookmarks({}); }

void ActivityManager::goToGlobalBookmarks(ReturnHint hint) {
  hasReturnHint = false;
  replaceActivity(std::make_unique<GlobalBookmarksActivity>(renderer, mappedInput, std::move(hint)));
}

void ActivityManager::goToBrowser() {
  const auto& servers = OPDS_STORE.getServers();
  // Skip the server picker when there's only one server configured
  if (servers.size() == 1) {
    replaceActivity(
        std::make_unique<OpdsBookBrowserActivity>(renderer, mappedInput, servers[0], OpdsBackTo::FileTransfer));
  } else {
    replaceActivity(std::make_unique<OpdsServerListActivity>(renderer, mappedInput, true, OpdsBackTo::FileTransfer));
  }
}

void ActivityManager::goToBrowserWithSearch(std::string query) {
  const auto& servers = OPDS_STORE.getServers();
  if (servers.size() == 1) {
    replaceActivity(std::make_unique<OpdsBookBrowserActivity>(renderer, mappedInput, servers[0], OpdsBackTo::Home,
                                                              std::move(query)));
  } else {
    replaceActivity(
        std::make_unique<OpdsServerListActivity>(renderer, mappedInput, true, OpdsBackTo::Home, std::move(query)));
  }
}

void ActivityManager::goToReader(std::string path) {
  {
    RenderLock lock;
    ensureSdFontLoadedForPath(path.c_str());
  }
  replaceActivity(std::make_unique<ReaderActivity>(renderer, mappedInput, std::move(path)));
}

void ActivityManager::goToKOReaderSync() {
  const auto& sync = APP_STATE.koReaderSyncSession;
  if (!sync.active || sync.epubPath.empty()) {
    LOG_ERR("ACT", "Cannot launch KOReader sync without an active EPUB handoff");
    goHome();
    return;
  }

  SyncLocalPosition local;
  local.spineIndex = sync.spineIndex;
  local.page = sync.page;
  local.totalPages = sync.totalPagesInSpine;
  local.paragraphIndex = sync.paragraphIndex;
  local.hasParagraphIndex = sync.hasParagraphIndex;
  local.paragraphIndexBefore = sync.paragraphIndexBefore;
  local.visibleOffsetAtPage = sync.visibleOffsetAtPage;
  local.visibleOffsetAtNextPage = sync.visibleOffsetAtNextPage;
  local.hasVisibleOffset = sync.hasVisibleOffset;
  replaceActivity(std::make_unique<KOReaderSyncActivity>(renderer, mappedInput, sync.epubPath, local, sync.intent));
}

void ActivityManager::replaceWithReader(std::string path, ReturnHint hint) {
  returnHint = std::move(hint);
  hasReturnHint = true;
  {
    RenderLock lock;
    ensureSdFontLoadedForPath(path.c_str());
  }
  replaceActivity(std::make_unique<ReaderActivity>(renderer, mappedInput, std::move(path)));
}

void ActivityManager::replaceWithFileBrowser(std::string path, ReturnHint hint, std::string focusName) {
  returnHint = std::move(hint);
  hasReturnHint = true;
  replaceActivity(std::make_unique<FileBrowserActivity>(renderer, mappedInput, std::move(path), std::move(focusName)));
}

void ActivityManager::returnFromChild() {
  if (!hasReturnHint) {
    goHome();
    return;
  }
  ReturnHint hint = std::move(returnHint);
  returnHint = {};
  hasReturnHint = false;

  switch (hint.target) {
    case ReturnTo::Library: {
      std::optional<uint32_t> author;
      if (!hint.selectionContext.empty()) {
        author = static_cast<uint32_t>(std::strtoul(hint.selectionContext.c_str(), nullptr, 10));
      }
      goToLibrary(libraryTabFrom(static_cast<uint8_t>(hint.selectIndex)), std::move(hint.path),
                  std::move(hint.selectName), author);
      break;
    }
    case ReturnTo::AllFiles:
      replaceActivity(std::make_unique<FileBrowserActivity>(renderer, mappedInput, std::move(hint.path),
                                                            std::move(hint.selectName),
                                                            FileBrowserActivity::Mode::AllFiles));
      break;
    case ReturnTo::GlobalBookmarks:
      goToGlobalBookmarks(std::move(hint));
      break;
    case ReturnTo::Home:
    default:
      goHome(std::move(hint.selectName), hint.selectIndex);
      break;
  }
}

void ActivityManager::goToSleep(bool fromTimeout) {
  sleepTransition = true;
  replaceActivity(std::make_unique<SleepActivity>(renderer, mappedInput, fromTimeout));
  loop();  // Important: sleep screen must be rendered immediately, the caller will go to sleep right after this returns
  sleepTransition = false;
}

void ActivityManager::goToBoot() { replaceActivity(std::make_unique<BootActivity>(renderer, mappedInput)); }

void ActivityManager::goToFullScreenMessage(std::string message, EpdFontFamily::Style style) {
  replaceActivity(std::make_unique<FullScreenMessageActivity>(renderer, mappedInput, std::move(message), style));
}

void ActivityManager::goToWeather() { replaceActivity(std::make_unique<WeatherActivity>(renderer, mappedInput)); }

void ActivityManager::goToReadingStats() {
  replaceActivity(std::make_unique<ReadingStatsActivity>(renderer, mappedInput));
}

void ActivityManager::goToHomeMore() { replaceActivity(std::make_unique<HomeMoreActivity>(renderer, mappedInput)); }

void ActivityManager::goToHomeMenuAction(const HomeMenuAction action) {
  switch (action) {
    case HomeMenuAction::Library:
      goToLibrary(libraryTabFrom(APP_STATE.libraryTab));
      break;
    case HomeMenuAction::ReadingStats:
      goToReadingStats();
      break;
    case HomeMenuAction::GlobalBookmarks:
      goToGlobalBookmarks();
      break;
    case HomeMenuAction::FileTransfer:
      goToFileTransfer();
      break;
    case HomeMenuAction::Weather:
      goToWeather();
      break;
    case HomeMenuAction::More:
      goToHomeMore();
      break;
    case HomeMenuAction::Settings:
      goToSettings();
      break;
  }
}

void ActivityManager::goHome(std::string focusBookPath, int focusSelectorIndex) {
  hasReturnHint = false;
  replaceActivity(std::make_unique<HomeActivity>(renderer, mappedInput, std::move(focusBookPath), focusSelectorIndex));
}

void ActivityManager::pushActivity(std::unique_ptr<Activity>&& activity) {
  if (pendingActivity) {
    // Should never happen in practice
    LOG_ERR("ACT", "pendingActivity while pushActivity is not expected");
    pendingActivity.reset();
  }
#if DEBUG_MEMORY_CONSUMPTION
  LOG_DBG("ACT", "pushActivity requested: current=%s stackSize=%zu",
          currentActivity ? currentActivity->getName().c_str() : "<none>", stackActivities.size());
#endif
  pendingActivity = std::move(activity);
  pendingAction = PendingAction::Push;
}

int ActivityManager::releaseBuriedActivityState() {
  int released = 0;
  for (const auto& activity : stackActivities) {
    if (activity->releaseBuriedState()) {
      activity->buriedStateReleased = true;
      ++released;
    }
  }
  return released;
}

void ActivityManager::popActivity() {
  if (pendingActivity) {
    // Should never happen in practice
    LOG_ERR("ACT", "pendingActivity while popActivity is not expected");
    pendingActivity.reset();
  }
  pendingAction = PendingAction::Pop;
}

bool ActivityManager::preventAutoSleep() const { return currentActivity && currentActivity->preventAutoSleep(); }

bool ActivityManager::keepAwake() const { return currentActivity && currentActivity->keepAwake(); }

bool ActivityManager::requiresExclusiveStorageLoop() const {
  return currentActivity && currentActivity->requiresExclusiveStorageLoop();
}

bool ActivityManager::isReaderActivity() const {
  if (currentIsReaderActivity()) return true;
  return std::any_of(stackActivities.begin(), stackActivities.end(),
                     [](const auto& activity) { return activity->isReaderActivity(); });
}

bool ActivityManager::currentIsReaderActivity() const { return currentActivity && currentActivity->isReaderActivity(); }

bool ActivityManager::skipLoopDelay() const { return currentActivity && currentActivity->skipLoopDelay(); }

bool ActivityManager::currentOwnsSerialInput() const { return currentActivity && currentActivity->ownsSerialInput(); }

void ActivityManager::prepareFramebufferForCapture() {
  if (currentActivity) {
    currentActivity->prepareFramebufferForCapture();
  }
}

void ActivityManager::describeState(std::string& out) const {
  out += "activity=";
  out += currentActivityName();
  std::string stack;
  for (size_t i = 0; i < stackActivities.size(); ++i) {
    if (i > 0) stack += ',';
    stack += stackActivities[i]->getName();
  }
  SerialCommandGrammar::appendField(out, "stack", stack);
  if (currentActivity) currentActivity->appendDebugState(out);
}

const char* ActivityManager::currentActivityName() const {
  return currentActivity ? currentActivity->getName().c_str() : "none";
}

const char* ActivityManager::busyReason(const bool includeBackground) {
  if (pendingAction != PendingAction::None) return "transition";
  // A press the loop has not taken in yet, or one the classifier is still deciding about. Not
  // isGestureInFlight(): its emitted-but-unconsumed events can stay queued for a screen's life.
  if (mappedInput.hasPendingInput() || buttonEvents.isMidGesture()) return "input";
  // A frame is queued (requestedUpdate, or a notification the render task has not taken), is
  // being drawn (renderPassActive covers the stretch where render() lets go of the mutex), has a
  // task waiting on it, or is still going onto the panel.
  if (isUpdateSuperseded() || renderPassActive.load(std::memory_order_acquire) || waitingTaskHandle != nullptr ||
      RenderLock::peek() || renderer.isRefreshPending()) {
    return "render";
  }
  // Between taking its notification and taking the mutex the render task shows none of the above
  // for an instant; CMD:IDLE's quiet window (IdleGate) is long enough to see the pass that follows.
#if SERIAL_TEST_API
  if (currentActivity && currentActivity->hasPendingScreenWork()) return "render";
  if (includeBackground && currentActivity && currentActivity->hasPendingBackgroundWork()) return "background";
#else
  (void)includeBackground;
#endif
  return nullptr;
}

bool ActivityManager::currentTakesButtonAction(const CrossPointSettings::BUTTON_ACTION action,
                                               const MappedInputManager::Button button) const {
  if (currentIsReaderActivity()) return true;
  return currentActivity && currentActivity->takesButtonAction(action, button);
}

void ActivityManager::dispatchButtonAction(const CrossPointSettings::BUTTON_ACTION action) {
  // A screen other than the reader opts in per button, which this call does not know, so it is
  // let through here and vetted by the callers: main.cpp hands a reader-scoped button action to
  // such a screen only once currentTakesButtonAction() agreed, and the gesture path offers it none
  // outside the reader. Every other screen keeps the no-op onButtonAction().
  if (currentActivity) {
    currentActivity->onButtonAction(action);
  }
}

bool ActivityManager::handleForcedRefresh(const HalDisplay::RefreshMode mode) {
  return currentActivity && currentActivity->handleForcedRefresh(mode);
}

#if CP_TOUCH_UI

void ActivityManager::dispatchLightPanelGesture() {
  if (!mappedInput.hasTouch()) return;
  // Inert on an unlit board: wasLightPanelGesture() already answers false there, because
  // the top edge is the reader menu on those and the two must not compete.
  if (!mappedInput.wasLightPanelGesture()) return;
  if (currentActivity == nullptr) return;

  // Don't stack a panel on top of itself, or on a screen that owns the whole surface for
  // text entry -- a keyboard's own rows would be behind it and a swipe there is likelier to
  // be a mis-stroke than a request for the light.
  const std::string& name = currentActivity->getName();
  if (name == "FrontlightPanel" || name == "KeyboardEntry") return;

  // A dedicated drawer rather than the Display > Reading light submenu, which this used to
  // borrow. The submenu is a settings LIST, and every control on it is one row deep: the
  // brightness row only opens its slider on a further screen. Worse, the rows handed over were
  // raw settings-list entries that still carried their `submenu` field, so
  // MenuListActivity::onEnter() re-collapsed them into a nested submenu and the pull-down
  // opened a screen titled "Reading light" whose single row was also "Reading light".
  //
  // Through the CURRENT activity's startActivityForResult(), not pushActivity() directly, and
  // that routing is load-bearing rather than stylistic. The drawer paints over only its own band
  // and leaves the rest of the frame standing, which requires the displayed frame to be
  // recoverable -- and syncWriteBufferFromDisplayed() silently does nothing while the secondary
  // framebuffer is lent out (FreeInkDisplay.cpp: `if (frameBuffer && frameBufferActive)`).
  // Background-B holds that borrow across loop ticks for the length of a spine build, and
  // EpubReaderActivity::startActivityForResult() is the hook that hands it back for exactly this
  // case ("B's borrow must not outlive the reader being the top activity"). pushActivity() moves
  // the parent onto the stack without calling anything on it, so it would bypass that and let
  // the drawer composite onto a stale page.
  //
  // The empty handler is deliberate: the pop path already requests a repaint of the revealed
  // activity, and the drawer has no result to report.
  // Put the DISPLAYED frame in the write framebuffer before the drawer composites onto it. Two
  // steps, because two different things can leave it wrong and neither covers the other:
  //
  //  1. syncWriteBufferFromDisplayed() -- displayBuffer() ends in a swap, so on an ordinary screen
  //     the write buffer holds the frame from two refreshes ago while the secondary holds what is
  //     on the panel.
  //  2. prepareFramebufferForCapture() -- step 1 can silently do nothing: its memcpy is gated on
  //     frameBufferActive, which is null while the reader has the secondary lent to a section
  //     build or released for a decode. And in the reader the WRITE buffer may hold the
  //     pre-rendered NEXT page (Background-A draws it there -- renderPageContentOnly, not the
  //     secondary), which step 1 overwrites only when it actually runs. This is the reader's own
  //     hook for exactly that ("redraw the visible page here so the capture matches the
  //     display"), and it re-renders the current page over whatever step 1 left. A no-op on every
  //     activity that has no pre-render.
  //
  // Under a RenderLock and in this order: the screenshot path in main.cpp does the same thing for
  // the same reason, and step 2 must be able to overwrite what step 1 produced.
  {
    RenderLock lock;
    renderer.syncWriteBufferFromDisplayed();
    currentActivity->prepareFramebufferForCapture();
  }
  // The write buffer now holds the DISPLAYED frame. Anything later in this tick that syncs
  // again would undo prepareFramebufferForCapture() above and hand the drawer the wrong page.
  framebufferPreparedThisTick = true;

  mappedInput.suppressTouchContact();
  currentActivity->startActivityForResult(std::make_unique<FrontlightPanelActivity>(renderer, mappedInput),
                                          [](const ActivityResult&) {});
  LOG_DBG("TCH", "Top-edge swipe -> reading light");
}

void ActivityManager::dispatchListTap() {
  if (!mappedInput.hasTouch()) return;

#if defined(BUTTON_TRACE) && BUTTON_TRACE
  // Why a tap on a row did nothing, in one line. Placed before the early-outs so
  // it also reports the case where no band was recorded at all, and it reads the
  // tap with a const peek so it steals nothing from the real path below.
  //
  // What to look for: `first` is the item index of the band's TOP row, so on a
  // scrolled list it must equal the first item actually on screen; `rows` is the
  // y range the band covers, which must contain the tap; `-> item` is what the
  // hit test made of it, -1 being a miss.
  {
    int tx = 0;
    int ty = 0;
    if (mappedInput.wasScreenTapped(tx, ty)) {
      ListTouchBand::Band band;
      if (!ListTouchBand::snapshot(band)) {
        LOG_INF("TCH", "list tap (%d,%d): no band recorded", tx, ty);
      } else {
        const int lastBottom = band.count > 0 ? band.top[band.count - 1] + band.height[band.count - 1] : -1;
        // The selectable mask is 64 bits and printed as two 32-bit halves, high first: the
        // log's printf is the newlib-nano one, which does not carry the `ll` length modifier.
        LOG_INF("TCH", "list tap (%d,%d): band x=%d w=%d first=%d count=%d rows y=%d..%d sel=0x%08lX%08lX -> item %d",
                tx, ty, band.x, band.width, band.firstIndex, band.count, band.count > 0 ? band.top[0] : -1, lastBottom,
                static_cast<unsigned long>(band.selectable >> 32), static_cast<unsigned long>(band.selectable),
                ListTouchBand::hitTestIn(band, tx, ty));
      }
    }
  }
#endif

  // Nothing painted a list on this screen, so there is nothing here a tap could mean. Checked
  // before touching the tap queue: the tap belongs to whoever else wants it.
  if (!ListTouchBand::hasBand()) return;

  // Like the hint strip, this runs AFTER currentActivity->loop(), which makes it a strict
  // fallback: a screen that handles its own taps (Home's covers and menu, RecentBooks' cover
  // grid) has already consumed the tap and this finds nothing.
  int index = -1;
  if (mappedInput.listTouch(index) != MappedInputManager::RowTouch::Tap) return;

  if (currentActivity == nullptr) return;

  // The screen resolves the tap against its current selection; the shared preference may promote
  // a newly selected row to activation. A rejected tap stays consumed because the finger landed
  // on a row this screen painted and nothing else should reinterpret it.
  const auto tapResult = currentActivity->listTapActivation.applyPreference(
      index, currentActivity->selectListRow(index),
      SETTINGS.touchListActivation == CrossPointSettings::TOUCH_LIST_ACTIVATE_IMMEDIATELY);
  switch (tapResult) {
    case ListRowTap::Result::Rejected:
      // Logged, because a silent return here is indistinguishable from a tap that
      // never arrived or a band recorded for the wrong rows — and a screen that
      // draws a list but never implements selectListRow() lands in exactly this
      // branch for every tap, on every page. That is not a hypothetical: it is
      // how EnumSelectionActivity shipped, and the trace above showed the hit
      // test resolving the right row while nothing happened.
      LOG_DBG("TCH", "List tap -> row %d rejected by %s", index, currentActivity->getName().c_str());
      return;

    case ListRowTap::Result::Selected:
      // First tap on a row: move the highlight and stop. This repaint IS the feedback -- the
      // reader sees the selection land on their choice before anything happens, and a mis-tap
      // costs one more tap instead of an action to undo. On e-paper it is the only feedback
      // available until a localised tap-flash exists (P2).
      currentActivity->requestUpdate();
      LOG_DBG("TCH", "List tap -> row %d selected", index);
      return;

    case ListRowTap::Result::Activate:
      // Second tap, on the row already selected. Let the screen's OWN Confirm handler do the
      // rest: every one of these screens already has a Confirm path, often several branches deep
      // (a category row that cycles, an entry that opens, a value that toggles), and a tap must
      // run that exact code rather than a parallel copy of it. Raw rather than logical so the
      // user's button remapping still applies, exactly as the hint strip does it. Both the
      // wasPressed() bitmask consumers and ButtonEventManager's press-type FSM see an ordinary
      // Short press, one tick from now.
      mappedInput.injectRawPress(mappedInput.rawIndex(MappedInputManager::Button::Confirm));
      LOG_DBG("TCH", "List tap -> row %d already selected, Confirm synthesized", index);
      return;
  }
}

// A vertical swipe over a list scrolls it by a screenful.
//
// The gap this closes: a tap reaches only the rows currently painted, because that is all
// ListTouchBand records. Everything below the fold was behind the hint strip's Left/Right boxes
// and nothing else -- workable, but the one obviously missing gesture on a device navigated by
// finger.
//
// Synthesized rather than handled, like every other touch path here. Logical Left/Right are
// already the list PAGE buttons across the firmware (ButtonNavigator::onListPageNav, which
// MenuListActivity's eight subclasses inherit and the browser and chapter selectors implement
// themselves), so injecting one is a screenful in exactly the screens that have one and a step
// in the rest -- and no screen has to learn a new verb.
//
// Direction follows the content, not the finger: dragging the page UP brings the rows BELOW into
// view, which is the next page.
//
// Runs after the activity's own loop(), so a screen handling its own touch has already had its
// turn, and after main.cpp's gesture dispatch, which suppresses the contact when the user has
// BOUND this swipe to something -- so a bound gesture wins and this never doubles up with it.
void ActivityManager::dispatchListSwipe() {
  if (!mappedInput.hasTouch()) return;
  // No list painted on this screen, so a swipe here means nothing. Checked first: the swipe
  // belongs to whoever else wants it.
  if (!ListTouchBand::hasBand()) return;

  const MappedInputManager::SwipeDir dir = mappedInput.wasSwipe();
  if (dir != MappedInputManager::SwipeDir::Up && dir != MappedInputManager::SwipeDir::Down) return;

  const bool forward = dir == MappedInputManager::SwipeDir::Up;
  const auto pageDirection = forward ? Activity::ListPageDirection::Forward : Activity::ListPageDirection::Back;
  mappedInput.suppressTouchContact();
  if (!currentActivity->pageList(pageDirection)) {
    const auto direction = forward ? MappedInputManager::Direction::Right : MappedInputManager::Direction::Left;
    mappedInput.injectRawPress(mappedInput.rawIndex(MappedInputManager::buttonFor(direction)));
  }
  LOG_DBG("TCH", "List swipe %s -> page %s", dir == MappedInputManager::SwipeDir::Up ? "up" : "down",
          dir == MappedInputManager::SwipeDir::Up ? "next" : "prev");
}

// The HAL backdates an injected hold by its own constant, because it cannot see the FSM's
// threshold from lib/. Checked here, where both are in scope, so the two can never drift into a
// "long" press that classifies Short.
static_assert(HalGPIO::INJECTED_LONG_PRESS_MS > ButtonEventManager::LONG_PRESS_MS,
              "an injected long press must outlast the long-press threshold");

void ActivityManager::dispatchHintStripTap() {
  if (!mappedInput.hasTouch()) return;

  // Nothing painted either strip on this screen, so there is nothing here a tap could mean.
  // Checked before touching the tap queue: the tap belongs to whoever else wants it.
  const bool hasBottom = ButtonHintStrip::hasStrip();
  const bool hasSide = ButtonHintStrip::Side::hasStrip();
  if (!hasBottom && !hasSide) return;

  // Deliberately runs AFTER currentActivity->loop(). wasScreenTapped() consumes, so giving
  // the activity first refusal makes the strips a strict fallback: a screen that handles the
  // tap itself (reader page turns, and the list rows of phase 4b) has already claimed it and
  // this call finds nothing. The flip side is that on a hint-drawing screen a tap no one
  // claimed is consumed here even when it misses every box -- which is correct, since by
  // this point nothing else wanted it.
  // Ask for the tap in the PORTRAIT frame, not the live one. Both drawButtonHints() and
  // drawSideButtonHints() force Portrait for the duration of their draw, so that is the frame
  // the recorded boxes live in; resolving the tap the same way makes the hit test correct in
  // all four orientations rather than only while the screen happens to be portrait. The
  // underlying transform has always taken orientation as a parameter -- only GfxRenderer's
  // convenience wrapper pinned it to the live value.
  int x = 0;
  int y = 0;

  // Resolves a point against whichever strips are on screen and returns the raw hardware
  // button index, or -1. ButtonHintStrip::hitTest already returns the raw index directly
  // (mapLabels() emits its four labels in {BTN_BACK, BTN_CONFIRM, BTN_LEFT, BTN_RIGHT} order);
  // the side strip's hitTest instead returns a 0/1 slot (its two boxes are independent rects,
  // not a shared band -- see SideStrip), so that gets turned into BTN_UP/BTN_DOWN here via
  // rawIndex(), the same fixed physical order mapHints() drew the boxes in.
  const auto hitEitherStrip = [&](const int px, const int py) -> int {
    if (hasBottom) {
      const int hit = ButtonHintStrip::hitTest(px, py);
      if (hit >= 0) return hit;
    }
    if (hasSide) {
      const int side = ButtonHintStrip::Side::hitTest(px, py);
      if (side == 0) return mappedInput.rawIndex(MappedInputManager::Button::Up);
      if (side == 1) return mappedInput.rawIndex(MappedInputManager::Button::Down);
    }
    return -1;
  };

  // A long tap on a box is a HOLD of the button it depicts, and it has to be tested before the
  // tap: the long press fires while the finger is still down, and the lift that follows would
  // otherwise also read as a tap and run the short action on top of the long one.
  //
  // Without this a whole class of actions is unreachable by touch, and on X4 Pro unreachable
  // full stop -- that board has no Back or Confirm pin and its capacitive home key emits press
  // and release in one pass, so nothing on it can produce a hold. The file browser's context
  // menu, the recents view toggle, remove-book and book-info all sit on one.
  //
  // peek + suppress rather than the consuming wasScreenLongPress(): the contact is only claimed
  // once the hold is known to be over a box, so a long press anywhere else still degrades into
  // the tap it would have been.
  if (mappedInput.peekScreenLongPressIn(touchtransform::Portrait, x, y)) {
    const int held = hitEitherStrip(x, y);
    if (held >= 0) {
      mappedInput.suppressTouchContact();
      mappedInput.injectRawPress(static_cast<uint8_t>(held), /*longPress=*/true);
      LOG_DBG("TCH", "Hint strip long tap at (%d,%d) -> raw button %d held", x, y, held);
      return;
    }
  }

  if (!mappedInput.wasScreenTappedIn(touchtransform::Portrait, x, y)) return;

  const int hint = hitEitherStrip(x, y);
  if (hint < 0) return;

  // Dated when the finger left the glass, not now: a refresh can hold the loop up between two
  // quick taps, and the list's double-tap reads the press times.
  mappedInput.injectRawPress(static_cast<uint8_t>(hint), /*longPress=*/false, mappedInput.lastTouchEventAtMs());
  LOG_DBG("TCH", "Hint strip tap at (%d,%d) -> raw button %d", x, y, hint);
}

#endif  // CP_TOUCH_UI

// A queued transition runs on this task and can take a second or more: tearing the current
// activity down, writing progress, building the next screen. Nothing reaches the panel while
// that happens, so a press -- button or touch alike -- looks ignored, and people press again.
//
// The refresh is fired ASYNCHRONOUSLY wherever the driver supports it, which is every board we
// ship except the T5S3 (LgfxEpdDriver is the only one of ours with supportsAsyncDisplay() ==
// false). The waveform then runs WHILE the transition does its work instead of before it, and
// nothing has to join it explicitly: the next screen's displayBuffer() opens with
// syncPendingAsync(). On T5S3 this is a blocking FAST refresh and the cost is real -- which is
// what the timing line at the end is for.
void ActivityManager::showBusyIndicator() {
  if (!SETTINGS.showBusyIndicator) return;
  // The Pop branch above logs and clears pendingAction when currentActivity is null WITHOUT
  // requesting a render, so an indicator painted for it would sit on the panel with nothing
  // scheduled to cover it.
  if (!currentActivity) return;
  // Checked on BOTH ends: leaving a lightweight overlay is as uninteresting as entering one, and
  // the compositing activities break outright if an indicator repaints before them. On a Pop
  // there is no pendingActivity, which is exactly the drawer-exit case the source check covers.
  if (currentActivity->suppressesBusyIndicator()) return;
  if (pendingActivity && pendingActivity->suppressesBusyIndicator()) return;
  // A HALF/FULL clean is armed for the screen that is about to be drawn. Shipping an indicator
  // would CONSUME it (every ship goes through consumeRefreshOverride): the indicator pays for the
  // clean, and the destination silently loses the one it was scheduled -- ghosting debt that does
  // not surface for many page turns. Measured on X4 Pro: the promoted HALF cost a 112 ms push and
  // a 1269 ms drain. Skip instead; the transition is about to repaint anyway.
  if (renderer.hasRefreshOverridePending()) return;
  // Only where the panel can ship it cheaply, judged by what a FAST refresh actually COSTS on
  // the controller in front of us rather than by which board this is. An X4 Pro can come up on
  // SSD1677, UC8179 or UC8279 and they do not agree, so the board name is not the question.
  //
  // An absolute threshold, not a ratio: the indicator buys the user roughly one waveform of
  // earlier feedback and costs the destination screen roughly one waveform of delay (its
  // displayBuffer() opens with syncPendingAsync() and drains ours). Those cancel, so what decides
  // whether it is worth having is the magnitude of that one waveform, nothing else.
  //
  // Device-measured FAST durations, excluding refreshes that already carried a drain:
  //   X3 ~435 ms, X4 Pro ~792 ms -- reported working as expected
  //   T5S3 ~1329 ms              -- reported "more annoying than helpful"
  // 1000 ms sits between them with real margin on both sides. Re-check it if a new panel lands
  // near the line rather than moving it on a hunch.
  //
  // Zero means never measured and nothing persisted, which only happens before this panel's first
  // FAST. Allow it: one indicator is a cheap price for learning the number.
  constexpr uint16_t kMaxFastRefreshMs = 1000;
  const uint16_t fastRefreshMs = renderer.getLastFastRefreshMs();
  if (fastRefreshMs > kMaxFastRefreshMs) return;
  // Keep the persisted copy current in memory; the next ordinary settings save carries it to NVS,
  // so this costs no extra write.
  if (fastRefreshMs != 0) SETTINGS.measuredFastRefreshMs = fastRefreshMs;

  RenderLock lock;

  // drawBusyIndicator() overlays the frame on screen, which needs syncWriteBufferFromDisplayed()
  // to be able to find one. Two cases where that is not true, and neither covers the other:
  //
  //  - Something earlier in this tick may have prepared the write buffer already
  //    (dispatchLightPanelGesture does). Syncing again would undo its
  //    prepareFramebufferForCapture() -- see the pitfall in docs/activity-manager.md -- so tell
  //    the theme not to.
  //  - While the secondary is LENT OUT the sync is gated on frameBufferActive and silently does
  //    nothing, leaving no way to get a correct frame. Skip rather than ship a stale panel.
  //    hasSecondaryBuffer() cannot tell a borrow from a release (a release seeds the write buffer
  //    first and would be safe), so this is deliberately conservative.
  const bool alreadyPrepared = framebufferPreparedThisTick;
  if (!alreadyPrepared && !renderer.hasSecondaryBuffer()) return;

  const unsigned long start = millis();
  // Through the theme rather than drawn here: GUI owns every other piece of chrome, and
  // drawPopup()'s frame style, sync rule and ship contract are exactly the ones this wants.
  // Async so the waveform overlaps the transition's own work -- the next screen's displayBuffer()
  // opens with syncPendingAsync() and joins it, so nothing here owes an explicit finish.
  GUI.drawBusyIndicator(renderer, /*overlayDisplayedFrame=*/!alreadyPrepared, PopupShip::Async);

  // The number this feature lives or dies on: what the acknowledgement ADDS to the transition it
  // is acknowledging.
  LOG_DBG("ACT", "busy indicator (%s) returned in %lu ms", renderer.supportsAsyncRefresh() ? "async" : "blocking",
          static_cast<unsigned long>(millis() - start));
}

void ActivityManager::requestUpdate(bool immediate) {
  if (immediate) {
    if (renderTaskHandle) {
      xTaskNotify(renderTaskHandle, 1, eIncrement);
    }
  } else {
    // Deferring the update until current loop is finished
    // This is to avoid multiple updates being requested in the same loop
    taskENTER_CRITICAL(&activityStateMux);
    requestedUpdate = true;
    taskEXIT_CRITICAL(&activityStateMux);
  }
}
void ActivityManager::requestUpdateAndWait() {
  if (!renderTaskHandle) {
    return;
  }

  // Atomic section to perform checks
  taskENTER_CRITICAL(&activityStateMux);
  auto currTaskHandler = xTaskGetCurrentTaskHandle();
  auto mutexHolder = xSemaphoreGetMutexHolder(renderingMutex);
  bool isRenderTask = (currTaskHandler == renderTaskHandle);
  bool alreadyWaiting = (waitingTaskHandle != nullptr);
  bool holdingRenderLock = (mutexHolder == currTaskHandler);
  if (!alreadyWaiting && !isRenderTask && !holdingRenderLock) {
    waitingTaskHandle = currTaskHandler;
  }
  taskEXIT_CRITICAL(&activityStateMux);

  // Render task cannot call requestUpdateAndWait() or it will cause a deadlock
  assert(!isRenderTask && "Render task cannot call requestUpdateAndWait()");

  // There should never be the case where 2 tasks are waiting for a render at the same time
  assert(!alreadyWaiting && "Already waiting for a render to complete");

  // Cannot call while holding RenderLock or it will cause a deadlock
  assert(!holdingRenderLock && "Cannot call requestUpdateAndWait() while holding RenderLock");

  xTaskNotify(renderTaskHandle, 1, eIncrement);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

bool ActivityManager::isUpdateSuperseded() const {
  // Requested on the loop task but not yet converted into a task notification (that happens
  // at the end of ActivityManager::loop()).
  if (requestedUpdate) {
    return true;
  }
  if (!renderTaskHandle) {
    return false;
  }
  // renderTaskLoop() takes its wake-up with ulTaskNotifyTake(pdTRUE, ...), which zeroes the
  // notification value before render() is entered. Anything counted here was therefore posted
  // by a requestUpdate() that landed *during* the current render. Clearing no bits (mask 0)
  // makes this a pure read.
  return ulTaskNotifyValueClear(renderTaskHandle, 0) > 0;
}

// RenderLock

RenderLock::RenderLock(const Mode mode) {
  // A zero timeout makes the take a pure attempt. portMAX_DELAY blocks until it succeeds (the
  // kernel is built with INCLUDE_vTaskSuspend), so Blocking always ends up locked.
  isLocked = xSemaphoreTake(activityManager.renderingMutex, mode == Mode::Try ? 0 : portMAX_DELAY) == pdTRUE;
}

RenderLock::RenderLock([[maybe_unused]] Activity&) {
  xSemaphoreTake(activityManager.renderingMutex, portMAX_DELAY);
  isLocked = true;
}

RenderLock::RenderLock(ExclusiveActivityAccess) {
  // This waits for the render task to leave render(). Called FROM the render task it would wait
  // on itself forever, so state the invariant rather than leaving a future misuse to present as
  // an unexplained freeze — which is the exact class of bug this constructor exists to remove.
  assert(xTaskGetCurrentTaskHandle() != activityManager.renderTaskHandle &&
         "RenderLock(ExclusiveActivityAccess) must not be taken on the render task");

  // See the header for why the mutex alone is not enough. Take it, and only keep it if no
  // render pass is in flight; otherwise hand it straight back so the render task can finish.
  constexpr TickType_t RETRY_DELAY_TICKS = 1;
  constexpr unsigned long SLOW_WAIT_WARN_MS = 5000;
  const unsigned long start = millis();
  bool warned = false;
  bool waited = false;
  while (true) {
    xSemaphoreTake(activityManager.renderingMutex, portMAX_DELAY);
    if (!activityManager.renderPassActive.load(std::memory_order_acquire)) {
      isLocked = true;
      // Only ever logged when we actually had to wait, so it is silent on the common path and
      // one line per genuinely-overlapping transition otherwise. That line IS the evidence this
      // guard exists for: it marks a transition that arrived while the render task was inside
      // render() — the interleaving that used to run the activity's destructor (or race
      // endBackgroundBorrow()) out from under a pass still using the object. Compiled out at
      // LOG_LEVEL=0, so it costs nothing in a release build.
      if (waited) {
        LOG_DBG("ACT", "Transition waited %lums for an in-flight render pass (activity teardown deferred)",
                millis() - start);
      }
      return;
    }
    waited = true;
    // The render task is in its mid-pass unlocked window and wants the mutex back. Give it up
    // and yield: a plain retry at equal priority could re-win the mutex before the render task
    // is scheduled, so the vTaskDelay is load-bearing, not a politeness.
    xSemaphoreGive(activityManager.renderingMutex);
    vTaskDelay(RETRY_DELAY_TICKS);
    // A pass that never ends means a wedged panel or a stuck build slice, not lock contention.
    // Say so once rather than looking like an unexplained freeze — this wait blocks activity
    // transitions, so it is the first thing a "device hung on Back" report would land on.
    if (!warned && millis() - start > SLOW_WAIT_WARN_MS) {
      warned = true;
      LOG_ERR("ACT", "Activity transition waiting >%lums for the in-flight render pass to finish", SLOW_WAIT_WARN_MS);
    }
  }
}

RenderLock::~RenderLock() {
  if (isLocked) {
    xSemaphoreGive(activityManager.renderingMutex);
    isLocked = false;
  }
}

void RenderLock::unlock() {
  if (isLocked) {
    xSemaphoreGive(activityManager.renderingMutex);
    isLocked = false;
  }
}

/**
 *
 * Checks if renderingMutex is busy.
 *
 * @return true if renderingMutex is busy, otherwise false.
 *
 * Asks the mutex for its holder rather than peeking it as a queue. xQueuePeek() is not defined
 * for mutex-type queues: it reinterprets the holder/recursion union as queue pointers, and on a
 * free mutex it also pops a task off xTasksWaitingToReceive — waking a waiter that then finds
 * the mutex still unavailable. It happened to be harmless for a plain (non-recursive) mutex,
 * but it is unsupported API use in the reader's hottest polling path, and the holder query
 * answers exactly the same question legally.
 */
bool RenderLock::peek() { return xSemaphoreGetMutexHolder(activityManager.renderingMutex) != nullptr; };

bool RenderLock::heldByCurrentTask() {
  return xSemaphoreGetMutexHolder(activityManager.renderingMutex) == xTaskGetCurrentTaskHandle();
}
