// Stub implementation — full background sync is in feat/rss-feed-sync.
// This file exists so feat/pulsr-theme compiles standalone.
#include "RssFeedSync.h"

namespace RssFeedSync {

void startSync() {}
void suppressSync(unsigned long) {}
State getState() { return State::IDLE; }
const char* getStatusLabel() { return "FEED"; }
bool isFeedActive() { return false; }
bool isSyncing() { return false; }
void getProgress(int& current, int& total) { current = 0; total = 0; }

}  // namespace RssFeedSync
