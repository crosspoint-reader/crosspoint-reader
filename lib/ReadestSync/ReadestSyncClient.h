#pragma once
#include <string>

// Reuse progress types from KOReader sync (same protocol)
#include "KOReaderSyncClient.h"

/**
 * HTTP client for Readest sync, using the KOReader sync protocol.
 *
 * Readest does not host a sync server. Users must deploy their own
 * KOReader-compatible server (e.g. koreader-sync-server via Docker).
 *
 * Endpoints and authentication are identical to KOReaderSyncClient,
 * but credentials come from ReadestCredentialStore.
 */
class ReadestSyncClient {
 public:
  using Error = KOReaderSyncClient::Error;

  static Error authenticate();
  static Error getProgress(const std::string& documentHash, KOReaderProgress& outProgress);
  static Error updateProgress(const KOReaderProgress& progress);

  static const char* errorString(Error error) { return KOReaderSyncClient::errorString(error); }
};
