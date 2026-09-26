#pragma once

#include <mutex>

// The socket of the HTTP upload being read, shared between the main task, which
// reads the upload, and another task that cancels it. Cancelling shuts the
// socket's read side, so WebServer sees the client gone at its next read and
// aborts the upload. The main task retracts the socket before WebServer closes
// it; the lock keeps a cancel from shutting a number the closed socket has
// already handed to another connection.
class UploadCancel {
 public:
  using Shut = void (*)(int fd);

  // An upload starts reading from `fd`; after a cancel it is shut at once.
  void note(const int fd, const Shut shut) {
    std::lock_guard<std::mutex> lock(mutex);
    socket = fd;
    if (cancelled && socket >= 0) shut(socket);
  }

  void retract() {
    std::lock_guard<std::mutex> lock(mutex);
    socket = -1;
  }

  // Shuts the upload being read, and every later one as it starts.
  void cancel(const Shut shut) {
    std::lock_guard<std::mutex> lock(mutex);
    cancelled = true;
    if (socket >= 0) shut(socket);
  }

 private:
  std::mutex mutex;
  int socket = -1;
  bool cancelled = false;
};
