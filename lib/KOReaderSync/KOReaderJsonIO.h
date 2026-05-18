#pragma once

class KOReaderCredentialStore;

namespace KOReaderJsonIO {
bool save(const KOReaderCredentialStore& store, const char* path);
bool load(KOReaderCredentialStore& store, const char* json, bool* needsResave);
}  // namespace KOReaderJsonIO
