#include "WriterInput.h"

#ifdef SIMULATOR
#include "WriterSimInput.h"
#endif

void WriterInput::setActive(bool active) {
#ifdef SIMULATOR
  WriterSimInput::setActive(active);
#else
  (void)active;
#endif
}

bool WriterInput::readAvailable(std::string& out) {
#ifdef SIMULATOR
  return WriterSimInput::readAvailable(out);
#else
  out.clear();
  return false;
#endif
}
