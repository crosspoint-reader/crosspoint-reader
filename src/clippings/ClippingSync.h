#pragma once

#include <string>

class SyncRequest;

namespace clippingSync {
// Syncs one book's clippings; syncEngine calls it when Sync Clippings is enabled.
// Returns false when the server lacks clipping sync or a step fails.
bool run(SyncRequest& sync, const std::string& bookPath, const std::string& documentHash);
}  // namespace clippingSync
