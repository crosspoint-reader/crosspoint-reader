#pragma once

#include <string>

class SyncRequest;

namespace bookmarkSync {
// Merges one book's bookmarks with the server: uploads pending deletions and
// new or renamed bookmarks, then applies the server's list to the bookmark file.
// The caller has confirmed that the server offers bookmark sync.
// Returns false when a step fails; local changes stay pending for the next sync.
bool run(SyncRequest& sync, const std::string& bookPath, const std::string& documentHash);
}  // namespace bookmarkSync
