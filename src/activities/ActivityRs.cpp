#include "ActivityRs.h"

#include <Logging.h>

// Global pointers for Rust FFI to access renderer, input, and activity.
//
// They name whichever Rust activity is currently running. bindGlobals() sets
// them on entry and again at the top of every loop() and render(), because
// binding once is not enough in two different ways:
//
//  - render() runs on the render task while loop() runs on the main task, so
//    binding inside a single callback leaves them null for the other (that is
//    what made input dead from Rust's loop_());
//  - activities form a stack. A Rust screen opened over another Rust screen
//    clears these on its way out, and ActivityManager resumes the one beneath
//    without calling onEnter() again, so it would otherwise run with nulls.
GfxRenderer* g_rustRendererPtr = nullptr;
MappedInputManager* g_rustInputPtr = nullptr;
ActivityRs* g_rustActivityPtr = nullptr;

// Forward declarations for Rust FFI
extern "C" {
void rust_activity_on_enter(void* activity);
void rust_activity_loop(void* activity);
void rust_activity_on_exit(void* activity);
void rust_activity_render(void* activity, const void* renderer);
uint8_t rust_activity_home_gesture(void* activity);
void destroy_rust_activity(void* activity);
}

ActivityRs::ActivityRs(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput, CreateFn create_fn_arg)
    : Activity(name, renderer, mappedInput), create_fn(create_fn_arg) {
  // Create the Rust activity object
  rust_activity = create_fn();
  if (!rust_activity) {
    LOG_ERR("ACTRS", "Failed to create Rust activity: %s", name);
  }
}

ActivityRs::~ActivityRs() {
  if (rust_activity) {
    destroy_rust_activity(rust_activity);
    rust_activity = nullptr;
  }
}

void ActivityRs::bindGlobals() {
  // Three stores, cheap enough to repeat every frame; see the note above the
  // globals for why once is not enough.
  g_rustRendererPtr = &renderer;
  g_rustInputPtr = &mappedInput;
  g_rustActivityPtr = this;
}

void ActivityRs::onEnter() {
  // Bind before any Rust code runs so both the main task (loop) and the
  // render task (render) can reach renderer/input/activity.
  bindGlobals();

  // Rust allocates from this same heap, so its cost only shows up here - the
  // build-time size report measures static sections, where Rust is ~0 bytes.
  heapOnEnter = ESP.getFreeHeap();

  Activity::onEnter();

  if (rust_activity) {
    rust_activity_on_enter(rust_activity);
  }

  // Paint on entry. ActivityManager's Push branch does not request an update
  // (only its Pop branch does), so without this a Rust screen appears only
  // when the screen it replaced happened to have a repaint pending - which is
  // why the frontlight panel came up on some top-edge pulls and not others.
  // The C++ activities each do this themselves; doing it here covers every
  // Rust screen ever written.
  requestUpdate();
}

void ActivityRs::onExit() {
  if (rust_activity) {
    rust_activity_on_exit(rust_activity);
  }
  Activity::onExit();

  // A screen that does not return to its entry figure is leaking. Largest
  // block matters as much as the total: fragmentation, not exhaustion, is
  // what makes the next allocation fail on a device with no MMU.
  const uint32_t heapOnExit = ESP.getFreeHeap();
  LOG_INF("ACTRS", "%s heap: %u -> %u (%+d), largest block %u", name.c_str(), heapOnEnter, heapOnExit,
          static_cast<int32_t>(heapOnExit) - static_cast<int32_t>(heapOnEnter), ESP.getMaxAllocHeap());

  // Only unbind what we bound: a pushed activity must not clear its parent's.
  if (g_rustActivityPtr == this) {
    g_rustRendererPtr = nullptr;
    g_rustInputPtr = nullptr;
    g_rustActivityPtr = nullptr;
  }
}

bool ActivityRs::handleHomeGesture() { return rust_activity && rust_activity_home_gesture(rust_activity) != 0; }

void ActivityRs::loop() {
  bindGlobals();
  Activity::loop();

  if (rust_activity) {
    rust_activity_loop(rust_activity);
  }
}

void ActivityRs::render(RenderLock&& lock) {
  bindGlobals();

  // The Rust activity clears and paints the framebuffer via the FFI stubs.
  if (rust_activity) {
    rust_activity_render(rust_activity, &renderer);
  }

  // Push the painted framebuffer to the panel, as every C++ activity does.
  renderer.displayBuffer();

  // Lock is held for the whole paint; released when it goes out of scope.
  (void)lock;
}
