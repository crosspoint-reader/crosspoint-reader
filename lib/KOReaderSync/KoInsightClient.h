#pragma once

#include <memory>
#include <string>

class Epub;

/**
 * KoInsight HTTP API (same routes as koinsight.koplugin upload.lua).
 * CrossPoint has no KOReader statistics DB; we send the same JSON shape with data
 * derived from recent books, per-book progress.bin, and the current reading position.
 */
class KoInsightClient {
 public:
  /**
   * If a KoInsight base URL is configured, POST /api/plugin/device and /api/plugin/import.
   * Failures are logged only; KOReader sync continues regardless.
   */
  static void pushReadingSnapshotIfConfigured(const std::shared_ptr<Epub>& epub, const std::string& epubPath,
                                              int currentSpineIndex, int currentPage, int chapterTotalPages);
};
