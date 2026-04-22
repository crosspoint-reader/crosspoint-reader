#include "WriterInput.h"

#ifdef SIMULATOR
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

bool WriterInput::readAvailable(std::string& out) {
  out.clear();

#ifdef SIMULATOR
  static bool stdinConfigured = false;
  if (!stdinConfigured) {
    const int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags >= 0) {
      fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    }
    stdinConfigured = true;
  }

  char buffer[128];
  bool readAny = false;

  while (true) {
    const ssize_t bytesRead = read(STDIN_FILENO, buffer, sizeof(buffer));
    if (bytesRead > 0) {
      out.append(buffer, static_cast<size_t>(bytesRead));
      readAny = true;
      if (static_cast<size_t>(bytesRead) < sizeof(buffer)) {
        break;
      }
    } else {
      if (bytesRead < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        break;
      }
      break;
    }
  }

  return readAny;
#else
  return false;
#endif
}
