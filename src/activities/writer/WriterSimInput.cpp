#include "WriterSimInput.h"

#ifdef SIMULATOR
#include <SimTextInput.h>
#endif

void WriterSimInput::setActive(bool active) {
#ifdef SIMULATOR
  SimTextInput::setCaptureEnabled(active);
#else
  (void)active;
#endif
}

bool WriterSimInput::readAvailable(std::string& out) {
#ifdef SIMULATOR
  return SimTextInput::readAvailable(out);
#else
  out.clear();
  return false;
#endif
}
