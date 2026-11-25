#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace freeink {
class SecureHttpClient;
}

/**
 * Mirrors a Readwise Reader queue onto the SD card as one EPUB per article,
 * and reports reading back.
 *
 * Each sync:
 *   1. checks the token (GET /api/v2/auth/)
 *   2. lists the selected locations (Later / Shortlist / Inbox), keeping
 *      articles, emails, RSS items and PDFs (newest first, capped)
 *   3. for articles already on the device: pushes reading progress, and
 *      archives the ones read to the last page (optional)
 *   4. removes local copies of articles no longer in the queue (optional)
 *   5. downloads new articles (html_content -> sanitized XHTML -> EPUB)
 *
 * Driven by step(): each call does at most one network request, so the
 * caller can repaint between calls. step() returns immediately while it
 * waits out Readwise's rate limit (20 list requests/minute).
 *
 * A local index (/.crosspoint/readwise-index.json) maps document ids to the
 * files this feature created; files it did not create are never touched.
 * Articles deleted on the device are not downloaded again while they stay
 * in the queue.
 */
class ReadwiseSync {
 public:
  enum class Phase : uint8_t { Auth, List, Progress, Cleanup, Download, Done, Failed };

  struct Stats {
    uint16_t downloaded = 0;
    uint16_t skipped = 0;   // no readable text (video, image-only, ...)
    uint16_t archived = 0;  // finished on device -> archived in Readwise
    uint16_t removed = 0;   // left the queue -> removed locally
    uint16_t progress = 0;  // progress updates sent
    uint16_t failed = 0;
  };

  ReadwiseSync();
  ~ReadwiseSync();

  // Loads settings and the index. False (with error()) when there is no token.
  bool begin();
  void step();

  Phase phase() const { return phase_; }
  bool finished() const { return phase_ == Phase::Done || phase_ == Phase::Failed; }
  const std::string& error() const { return error_; }
  const Stats& stats() const { return stats_; }
  // Item being worked on in the current phase, for the progress screen.
  const std::string& currentTitle() const { return currentTitle_; }
  int itemIndex() const { return static_cast<int>(cursor_); }
  int itemCount() const;
  // Milliseconds left on a rate-limit wait (0 when not waiting).
  uint32_t waitRemainingMs() const;

  struct RemoteDoc {
    std::string id;
    std::string title;
    std::string author;
    std::string siteName;
    std::string sourceUrl;
    std::string category;
    float progress = 0;
    bool isChild = false;  // highlight/note (has parent_id)
  };

  struct IndexEntry {
    std::string id;
    std::string file;  // empty: skipped (no text) or deleted on the device
    float pushed = 0;  // last progress sent
  };

 private:
  enum class Limit : uint8_t { List, Update };

  bool ready(Limit limit) const;
  void spent(Limit limit);
  void backOff(Limit limit, uint32_t seconds);
  bool retryOr(Limit limit, uint32_t retryAfter, const char* what);

  int api(const char* method, const std::string& url, const std::string& body, uint32_t* retryAfter,
          const std::function<bool(const uint8_t*, size_t)>& onBody);
  void fail(const std::string& message);

  void stepAuth();
  void stepList();
  void stepProgress();
  void stepCleanup();
  void stepDownload();

  bool downloadArticle(const RemoteDoc& doc, uint32_t* retryAfter, bool* transportFailed);
  std::string uniquePath(const std::string& title) const;

  bool loadIndex();
  bool saveIndex() const;
  IndexEntry* findEntry(const std::string& id);
  const RemoteDoc* findRemote(const std::string& id) const;

  Phase phase_ = Phase::Auth;
  std::string error_;
  Stats stats_;
  std::string currentTitle_;

  std::string token_;
  std::string folder_;
  std::vector<const char*> locations_;
  size_t locationIndex_ = 0;
  std::string pageCursor_;
  uint8_t listPages_ = 0;
  uint32_t beyondWindow_ = 0;
  bool listTruncated_ = false;
  uint8_t maxArticles_ = 50;
  bool archiveFinished_ = true;
  bool removeArchived_ = true;

  std::vector<RemoteDoc> remote_;
  std::vector<IndexEntry> index_;
  std::vector<size_t> toDownload_;  // indices into remote_
  size_t cursor_ = 0;
  uint8_t retries_ = 0;

  uint32_t nextListAt_ = 0;
  uint32_t nextUpdateAt_ = 0;

  std::unique_ptr<freeink::SecureHttpClient> session_;
};
