#include "network/RemoteKeyboardSession.h"

#include <Arduino.h>

RemoteKeyboardSession& RemoteKeyboardSession::getInstance() {
  static RemoteKeyboardSession instance;
  return instance;
}

uint32_t RemoteKeyboardSession::begin(std::string newTitle, std::string initialText, const size_t newMaxLength,
                                      const bool isPassword) {
  std::lock_guard<std::mutex> lock(mutex);
  activeId = nextId++;
  if (nextId == 0) {
    nextId = 1;
  }
  title = std::move(newTitle);
  text = std::move(initialText);
  maxLength = newMaxLength;
  password = isPassword;
  claimedBy.clear();
  lastClaimAt = 0;
  completed = false;
  submittedText.clear();
  return activeId;
}

void RemoteKeyboardSession::cancel(const uint32_t id) {
  std::lock_guard<std::mutex> lock(mutex);
  if (activeId == id) {
    clearLocked();
  }
}

RemoteKeyboardSession::Snapshot RemoteKeyboardSession::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex);
  Snapshot snapshot;
  snapshot.active = activeId != 0;
  snapshot.id = activeId;
  snapshot.title = title;
  snapshot.text = text;
  snapshot.maxLength = maxLength;
  snapshot.isPassword = password;
  if (activeId != 0 && (millis() - lastClaimAt) <= 2500) {
    snapshot.claimedBy = claimedBy;
    snapshot.lastClaimAt = lastClaimAt;
  }
  return snapshot;
}

bool RemoteKeyboardSession::isActive(const uint32_t id) const {
  std::lock_guard<std::mutex> lock(mutex);
  return activeId != 0 && activeId == id;
}

bool RemoteKeyboardSession::claim(const uint32_t id, std::string clientId) {
  std::lock_guard<std::mutex> lock(mutex);
  if (activeId == 0 || activeId != id) {
    return false;
  }
  claimedBy = std::move(clientId);
  lastClaimAt = millis();
  return true;
}

RemoteKeyboardSession::SubmitResult RemoteKeyboardSession::submit(const uint32_t id, const std::string& newText) {
  std::lock_guard<std::mutex> lock(mutex);
  if (activeId == 0 || activeId != id) {
    return SubmitResult::InvalidSession;
  }
  if (maxLength > 0 && newText.length() > maxLength) {
    return SubmitResult::TextTooLong;
  }
  text = newText;
  submittedText = newText;
  completed = true;
  return SubmitResult::Submitted;
}

bool RemoteKeyboardSession::takeSubmitted(const uint32_t id, std::string& outText) {
  std::lock_guard<std::mutex> lock(mutex);
  if (activeId == 0 || activeId != id || !completed) {
    return false;
  }
  outText = submittedText;
  clearLocked();
  return true;
}

bool RemoteKeyboardSession::hasRecentClaim(const uint32_t id, const unsigned long freshnessMs) const {
  std::lock_guard<std::mutex> lock(mutex);
  return activeId != 0 && activeId == id && !claimedBy.empty() && (millis() - lastClaimAt) <= freshnessMs;
}

void RemoteKeyboardSession::clearLocked() {
  activeId = 0;
  title.clear();
  text.clear();
  maxLength = 0;
  password = false;
  claimedBy.clear();
  lastClaimAt = 0;
  completed = false;
  submittedText.clear();
}
