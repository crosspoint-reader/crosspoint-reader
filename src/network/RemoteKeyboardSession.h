#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

class RemoteKeyboardSession {
 public:
  enum class SubmitResult { Submitted, InvalidSession, TextTooLong };

  struct Snapshot {
    bool active = false;
    uint32_t id = 0;
    std::string title;
    std::string text;
    size_t maxLength = 0;
    bool isPassword = false;
    std::string claimedBy;
    unsigned long lastClaimAt = 0;
  };

  static RemoteKeyboardSession& getInstance();

  uint32_t begin(std::string title, std::string initialText, size_t maxLength, bool isPassword);
  void cancel(uint32_t id);

  Snapshot snapshot() const;
  bool isActive(uint32_t id) const;
  bool claim(uint32_t id, std::string clientId);
  SubmitResult submit(uint32_t id, const std::string& text);
  bool takeSubmitted(uint32_t id, std::string& outText);
  bool hasRecentClaim(uint32_t id, unsigned long freshnessMs = 2500) const;

 private:
  void clearLocked();

  mutable std::mutex mutex;
  uint32_t nextId = 1;
  uint32_t activeId = 0;
  std::string title;
  std::string text;
  size_t maxLength = 0;
  bool password = false;
  std::string claimedBy;
  unsigned long lastClaimAt = 0;
  bool completed = false;
  std::string submittedText;
};

#define REMOTE_KEYBOARD_SESSION RemoteKeyboardSession::getInstance()
