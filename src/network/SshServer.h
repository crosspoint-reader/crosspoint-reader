#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdint>
#include <string>

// libssh opaque handle types (identical to the typedefs in <libssh/libssh.h>;
// repeated here so the rest of the firmware never includes libssh headers).
typedef struct ssh_bind_struct* ssh_bind;
typedef struct ssh_session_struct* ssh_session;
typedef struct ssh_channel_struct* ssh_channel;

/**
 * SshServer provides remote access to the SD card over SSH (port 22).
 *
 * - Password auth (single user, password supplied by the caller per session)
 * - Interactive shell with basic file commands (ls, cat, rm, mv, mkdir, ...)
 * - File transfer via scp (upload: `scp book.epub crosspoint@<ip>:/`,
 *   download: `scp crosspoint@<ip>:/book.epub .`)
 * - Persistent ed25519 host key stored on the SD card
 *
 * All session handling runs on a dedicated FreeRTOS task; the UI polls
 * getStatus() for progress display. One client is served at a time.
 */
class SshServer {
 public:
  struct TransferStatus {
    bool clientConnected = false;
    bool inProgress = false;
    size_t received = 0;
    size_t total = 0;
    std::string filename;
    std::string lastCompleteName;
    unsigned long lastCompleteAt = 0;
  };

  SshServer();
  ~SshServer();

  // Start listening (call after WiFi is up). The password is copied.
  bool begin(const char* password);

  // Stop the server task and free all libssh resources.
  void stop();

  bool isRunning() const { return running.load(); }

  TransferStatus getStatus() const;

  static constexpr uint16_t PORT = 22;
  static constexpr const char* USERNAME = "crosspoint";

 private:
  static void taskTrampoline(void* param);
  void serverTaskLoop();
  void handleClient();

  bool authenticate(ssh_session session);
  ssh_channel openChannel(ssh_session session);
  void serveChannel(ssh_session session, ssh_channel channel);

  void runShell(ssh_channel channel);
  // Returns false when the client asked to exit.
  bool executeCommand(ssh_channel channel, char* line, bool interactive);

  void commandLs(ssh_channel channel, const char* path);
  void commandCat(ssh_channel channel, const char* path);

  void scpSink(ssh_channel channel, const char* target);
  void scpSource(ssh_channel channel, const char* path);

  bool loadOrCreateHostKey(std::string& b64Key);

  void channelPrintf(ssh_channel channel, const char* fmt, ...);

  void setClientConnected(bool connected);
  void setTransferProgress(const char* filename, size_t received, size_t total);
  void setTransferComplete(const char* filename);

  ssh_bind sshbind = nullptr;
  int listenFd = -1;

  TaskHandle_t taskHandle = nullptr;
  std::atomic<bool> running{false};
  std::atomic<bool> stopRequested{false};

  char password[48] = {0};

  TransferStatus status;
  mutable SemaphoreHandle_t statusMutex = nullptr;
};
