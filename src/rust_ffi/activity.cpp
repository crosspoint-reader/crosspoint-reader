// The activity a Rust screen is running inside, and the panic hook.

#include <Logging.h>

#include <cstdlib>

#include "activities/ActivityRs.h"
#include "internal.h"

extern "C" {

const uint8_t* cpp_activity_title() {
  return reinterpret_cast<const uint8_t*>(g_rustActivityPtr ? g_rustActivityPtr->getTitle() : "");
}

void cpp_activity_finish() {
  if (g_rustActivityPtr) g_rustActivityPtr->finish();
}

void cpp_activity_request_update() {
  if (g_rustActivityPtr) g_rustActivityPtr->requestUpdate();
}

// Rust's panic handler lands here on the ESP32 targets. Nothing can be
// recovered: the framework builds with panic = "abort".
void cpp_panic(const uint8_t* message) {
  LOG_ERR("RUST", "%s", message ? rust_ffi::asText(message) : "panic");
  abort();
}

// The lifecycle entry points (rust_activity_on_enter and friends) are defined
// on the Rust side, not here.
}
