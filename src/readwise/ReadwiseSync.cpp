#include "ReadwiseSync.h"

#include <Arduino.h>
#include <Epub.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <PersistableStore.h>
#include <SecureHttpClient.h>
#include <StreamingJsonParser.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "EpubBuilder.h"
#include "JsonStringStreamer.h"
#include "ReadwiseStore.h"
#include "XhtmlSanitizer.h"
#include "util/BookCacheUtils.h"
#include "util/OpdsFilename.h"
#include "util/PluginHttp.h"
#include "util/TaskWatchdog.h"

namespace {

// Overridable for tests (the simulator points it at a local mock server).
#ifndef READWISE_API_BASE
#define READWISE_API_BASE "https://readwise.io/api"
#endif
constexpr char API[] = READWISE_API_BASE;
constexpr char INDEX_PATH[] = "/.crosspoint/readwise-index.json";

// Readwise: list 20/min, update 50/min per token. Stay just under both.
constexpr uint32_t LIST_INTERVAL_MS = 3200;
constexpr uint32_t UPDATE_INTERVAL_MS = 1300;
constexpr uint32_t DEFAULT_RETRY_AFTER_S = 30;
constexpr uint8_t MAX_RETRIES = 4;
// A queue longer than this many pages per location is not fully listed, so
// local cleanup is skipped for that sync (nothing can be judged "gone").
constexpr uint8_t MAX_LIST_PAGES = 20;
// Articles with less visible text than this are skipped (videos, image posts).
constexpr size_t MIN_TEXT_BYTES = 200;
// Progress changes smaller than this are not sent.
constexpr float PROGRESS_STEP = 0.02f;

bool wantedCategory(const std::string& c) { return c == "article" || c == "email" || c == "rss" || c == "pdf"; }

bool timeReached(uint32_t at) { return static_cast<int32_t>(millis() - at) >= 0; }

// --- list response parsing (metadata only; html is never requested here) ---

struct ListParse {
  std::vector<ReadwiseSync::RemoteDoc>* out = nullptr;
  std::string cursor;
  int depth = 0;
  char key[24] = {};
  int keyDepth = -1;
  int resultsDepth = -1;  // depth of the "results" array, -1 when outside
  bool inDoc = false;
  ReadwiseSync::RemoteDoc doc;

  bool docLevel() const { return inDoc && depth == resultsDepth + 1 && keyDepth == depth; }
  bool is(const char* k) const { return std::strcmp(key, k) == 0; }
};

JsonCallbacks listCallbacks(ListParse& p) {
  JsonCallbacks cb{};
  cb.ctx = &p;
  cb.onKey = [](void* ctx, const char* k, size_t len) {
    auto& s = *static_cast<ListParse*>(ctx);
    const size_t n = std::min(len, sizeof(s.key) - 1);
    std::memcpy(s.key, k, n);
    s.key[n] = '\0';
    s.keyDepth = s.depth;
  };
  cb.onString = [](void* ctx, const char* v, size_t len) {
    auto& s = *static_cast<ListParse*>(ctx);
    if (s.depth == 1 && s.keyDepth == 1 && s.is("nextPageCursor")) {
      s.cursor.assign(v, len);
      return;
    }
    if (!s.docLevel()) return;
    if (s.is("id")) {
      s.doc.id.assign(v, len);
    } else if (s.is("title")) {
      s.doc.title.assign(v, len);
    } else if (s.is("author")) {
      s.doc.author.assign(v, len);
    } else if (s.is("site_name")) {
      s.doc.siteName.assign(v, len);
    } else if (s.is("source_url")) {
      s.doc.sourceUrl.assign(v, len);
    } else if (s.is("category")) {
      s.doc.category.assign(v, len);
    } else if (s.is("parent_id")) {
      s.doc.isChild = len > 0;
    }
  };
  cb.onNumber = [](void* ctx, const char* v, size_t len) {
    auto& s = *static_cast<ListParse*>(ctx);
    if (!s.docLevel() || !s.is("reading_progress")) return;
    char buf[24];
    const size_t n = std::min(len, sizeof(buf) - 1);
    std::memcpy(buf, v, n);
    buf[n] = '\0';
    s.doc.progress = std::clamp(static_cast<float>(std::atof(buf)), 0.0f, 1.0f);
  };
  cb.onBool = [](void*, bool) {};
  cb.onNull = [](void*) {};
  cb.onObjectStart = [](void* ctx) {
    auto& s = *static_cast<ListParse*>(ctx);
    ++s.depth;
    if (s.resultsDepth >= 0 && s.depth == s.resultsDepth + 1) {
      s.inDoc = true;
      s.doc = ReadwiseSync::RemoteDoc{};
    }
  };
  cb.onObjectEnd = [](void* ctx) {
    auto& s = *static_cast<ListParse*>(ctx);
    if (s.inDoc && s.depth == s.resultsDepth + 1) {
      s.inDoc = false;
      if (!s.doc.id.empty()) s.out->push_back(std::move(s.doc));
    }
    --s.depth;
  };
  cb.onArrayStart = [](void* ctx) {
    auto& s = *static_cast<ListParse*>(ctx);
    ++s.depth;
    if (s.resultsDepth < 0 && s.depth == 2 && s.keyDepth == 1 && s.is("results")) s.resultsDepth = s.depth;
  };
  cb.onArrayEnd = [](void* ctx) {
    auto& s = *static_cast<ListParse*>(ctx);
    if (s.depth == s.resultsDepth) s.resultsDepth = -1;
    --s.depth;
  };
  return cb;
}

// Reading position from the reader's progress.bin: <spine u16><page u16><pages u16>...
// "Finished" follows the reader's own end_percent == 100 rule (see
// docs/plugin-events.md): the last page is showing, or the reader paged past
// it (the article EPUB has one spine item, so spine >= 1).
bool readLocalProgress(const std::string& path, float& fraction, bool& finished) {
  auto epub = makeUniqueNoThrow<Epub>(path, "/.crosspoint");
  if (!epub) return false;
  HalFile file;
  if (!Storage.openFileForRead("RWS", epub->getCachePath() + "/progress.bin", file)) return false;
  uint8_t data[10] = {};
  const int size = file.read(data, sizeof(data));
  file.close();
  if (size < 4) return false;
  const int spine = data[0] | (data[1] << 8);
  const int page = data[2] | (data[3] << 8);
  const int total = size >= 6 ? data[4] | (data[5] << 8) : 0;
  finished = spine >= 1 || (total > 0 && page != 0xFFFF && page + 1 >= total);
  if (finished) {
    fraction = 1.0f;
  } else if (total > 0 && page != 0xFFFF) {
    fraction = std::clamp(static_cast<float>(page) / static_cast<float>(total), 0.0f, 1.0f);
  } else {
    fraction = 0;
  }
  return true;
}

bool headerEquals(const std::string& a, const char* b) {
  if (a.size() != std::strlen(b)) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
  }
  return true;
}

}  // namespace

ReadwiseSync::ReadwiseSync() = default;
ReadwiseSync::~ReadwiseSync() = default;

bool ReadwiseSync::begin() {
  token_ = READWISE_STORE.getToken();
  folder_ = READWISE_STORE.getFolder();
  maxArticles_ = READWISE_STORE.getMaxArticles();
  archiveFinished_ = READWISE_STORE.getArchiveFinished();
  removeArchived_ = READWISE_STORE.getRemoveArchived();
  locations_.clear();
  switch (READWISE_STORE.getSources()) {
    case ReadwiseSources::InboxLaterShortlist:
      locations_.push_back("new");
      [[fallthrough]];
    case ReadwiseSources::LaterShortlist:
      locations_.push_back("later");
      locations_.push_back("shortlist");
      break;
    default:
      locations_.push_back("later");
      break;
  }
  if (token_.empty()) {
    fail("No Readwise access token set");
    return false;
  }
  loadIndex();
  session_ = makeUniqueNoThrow<freeink::SecureHttpClient>();
  if (!session_) {
    fail("Out of memory");
    return false;
  }
  phase_ = Phase::Auth;
  return true;
}

int ReadwiseSync::itemCount() const {
  switch (phase_) {
    case Phase::Progress:
      return static_cast<int>(index_.size());
    case Phase::Download:
      return static_cast<int>(toDownload_.size());
    default:
      return 0;
  }
}

uint32_t ReadwiseSync::waitRemainingMs() const {
  const uint32_t at = (phase_ == Phase::Progress) ? nextUpdateAt_ : nextListAt_;
  if (phase_ != Phase::Auth && phase_ != Phase::List && phase_ != Phase::Progress && phase_ != Phase::Download) {
    return 0;
  }
  const int32_t left = static_cast<int32_t>(at - millis());
  return left > 0 ? static_cast<uint32_t>(left) : 0;
}

void ReadwiseSync::fail(const std::string& message) {
  LOG_ERR("RWS", "%s", message.c_str());
  error_ = message;
  phase_ = Phase::Failed;
}

bool ReadwiseSync::ready(const Limit limit) const {
  return timeReached(limit == Limit::List ? nextListAt_ : nextUpdateAt_);
}

void ReadwiseSync::spent(const Limit limit) {
  if (limit == Limit::List) {
    nextListAt_ = millis() + LIST_INTERVAL_MS;
  } else {
    nextUpdateAt_ = millis() + UPDATE_INTERVAL_MS;
  }
}

void ReadwiseSync::backOff(const Limit limit, uint32_t seconds) {
  if (seconds == 0) seconds = DEFAULT_RETRY_AFTER_S;
  seconds = std::min<uint32_t>(seconds, 120);
  (limit == Limit::List ? nextListAt_ : nextUpdateAt_) = millis() + seconds * 1000;
}

// After a 429: wait and retry the same item, up to MAX_RETRIES times.
bool ReadwiseSync::retryOr(const Limit limit, const uint32_t retryAfter, const char* what) {
  if (retries_ < MAX_RETRIES) {
    ++retries_;
    LOG_INF("RWS", "Rate limited (%s); retry %u in %us", what, retries_, retryAfter);
    backOff(limit, retryAfter);
    return true;
  }
  return false;
}

int ReadwiseSync::api(const char* method, const std::string& url, const std::string& body, uint32_t* retryAfter,
                      const std::function<bool(const uint8_t*, size_t)>& onBody) {
  if (retryAfter) *retryAfter = 0;
  pluginhttp::Headers headers{{"Authorization", "Token " + token_}, {"Accept", "application/json"}};
  if (!body.empty()) headers.emplace_back("Content-Type", "application/json");

  int status = -1;
  bool complete = false;
  const int result = pluginhttp::requestStream(
      session_.get(), url, method, body, headers,
      [&](int s, const pluginhttp::Headers& responseHeaders, long) {
        status = s;
        for (const auto& h : responseHeaders) {
          if (retryAfter && headerEquals(h.first, "Retry-After"))
            *retryAfter = std::strtoul(h.second.c_str(), nullptr, 10);
        }
        return true;
      },
      [&](const uint8_t* data, size_t len) {
        resetTaskWatchdogIfSubscribed();
        if (status < 200 || status >= 300 || !onBody) return true;  // drain error bodies
        return onBody(data, len);
      },
      complete);
  if (result < 0) return -1;
  if (status >= 200 && status < 300 && !complete) return -1;
  return status;
}

void ReadwiseSync::step() {
  switch (phase_) {
    case Phase::Auth:
      stepAuth();
      break;
    case Phase::List:
      stepList();
      break;
    case Phase::Progress:
      stepProgress();
      break;
    case Phase::Cleanup:
      stepCleanup();
      break;
    case Phase::Download:
      stepDownload();
      break;
    case Phase::Done:
    case Phase::Failed:
      break;
  }
}

void ReadwiseSync::stepAuth() {
  if (!ready(Limit::List)) return;
  currentTitle_.clear();
  uint32_t retryAfter = 0;
  const int status = api("GET", std::string(API) + "/v2/auth/", "", &retryAfter, nullptr);
  spent(Limit::List);
  if (status == 204 || status == 200) {
    retries_ = 0;
    phase_ = Phase::List;
    return;
  }
  if (status == 429 && retryOr(Limit::List, retryAfter, "auth")) return;
  if (status == 401 || status == 403) {
    fail("Readwise rejected the access token");
  } else if (status < 0) {
    fail("Could not reach readwise.io");
  } else {
    fail("Readwise error " + std::to_string(status));
  }
}

void ReadwiseSync::stepList() {
  if (!ready(Limit::List)) return;
  if (pageCursor_.empty()) listPages_ = 0;

  const char* location = locations_[locationIndex_];
  std::string url = std::string(API) + "/v3/list/?location=" + location;
  if (!pageCursor_.empty()) url += "&pageCursor=" + pluginhttp::urlEncodeQuery(pageCursor_);

  std::vector<RemoteDoc> page;
  ListParse parse;
  parse.out = &page;
  StreamingJsonParser json(listCallbacks(parse));
  uint32_t retryAfter = 0;
  const int status = api("GET", url, "", &retryAfter, [&](const uint8_t* d, size_t n) {
    json.feed(reinterpret_cast<const char*>(d), n);
    return !json.hasError();
  });
  spent(Limit::List);

  if (status == 429 && retryOr(Limit::List, retryAfter, "list")) return;
  if (status != 200 || json.hasError()) {
    fail(status < 0 ? "Could not reach readwise.io" : "Could not list documents (" + std::to_string(status) + ")");
    return;
  }
  retries_ = 0;

  for (auto& doc : page) {
    if (doc.isChild || !wantedCategory(doc.category)) continue;
    if (findRemote(doc.id)) continue;  // same doc listed twice across pages
    // Memory stays bounded on long queues: past the download window only
    // documents already on the device are kept (for progress and cleanup).
    if (remote_.size() >= maxArticles_ && !findEntry(doc.id)) {
      ++beyondWindow_;
      continue;
    }
    remote_.push_back(std::move(doc));
  }
  LOG_INF("RWS", "Listed %s: %u kept, %u beyond the newest %u", location, static_cast<unsigned>(remote_.size()),
          static_cast<unsigned>(beyondWindow_), maxArticles_);

  ++listPages_;
  if (!parse.cursor.empty() && listPages_ < MAX_LIST_PAGES) {
    pageCursor_ = parse.cursor;
    return;
  }
  if (!parse.cursor.empty()) {
    LOG_INF("RWS", "Queue longer than %u pages; skipping local cleanup this sync", MAX_LIST_PAGES);
    listTruncated_ = true;
  }
  pageCursor_.clear();
  if (++locationIndex_ < locations_.size()) return;

  // Newest first within each location. Everything not yet on the device is
  // inside the window by construction (see above).
  toDownload_.clear();
  for (size_t i = 0; i < remote_.size(); ++i) {
    if (!findEntry(remote_[i].id)) toDownload_.push_back(i);
  }
  cursor_ = 0;
  phase_ = Phase::Progress;
}

void ReadwiseSync::stepProgress() {
  while (cursor_ < index_.size()) {
    IndexEntry& entry = index_[cursor_];
    const RemoteDoc* remote = findRemote(entry.id);
    if (entry.file.empty() || !remote) {
      ++cursor_;
      continue;
    }
    if (!Storage.exists(entry.file.c_str())) {
      // Deleted on the device: remember, so it is not downloaded again.
      LOG_INF("RWS", "Deleted locally: %s", entry.file.c_str());
      entry.file.clear();
      saveIndex();
      ++cursor_;
      continue;
    }
    float fraction = 0;
    bool finished = false;
    if (!readLocalProgress(entry.file, fraction, finished) || fraction <= 0) {
      ++cursor_;
      continue;
    }

    const bool archive = finished && archiveFinished_;
    const bool push = fraction >= remote->progress + PROGRESS_STEP && fraction >= entry.pushed + PROGRESS_STEP;
    if (!archive && !push) {
      ++cursor_;
      continue;
    }
    if (!ready(Limit::Update)) return;
    currentTitle_ = remote->title;

    char body[64];
    if (archive) {
      std::snprintf(body, sizeof(body), "{\"location\":\"archive\",\"reading_progress\":1}");
    } else {
      std::snprintf(body, sizeof(body), "{\"reading_progress\":%.3f}", static_cast<double>(fraction));
    }
    uint32_t retryAfter = 0;
    const int status = api("PATCH", std::string(API) + "/v3/update/" + entry.id + "/", body, &retryAfter, nullptr);
    spent(Limit::Update);
    if (status == 429 && retryOr(Limit::Update, retryAfter, "update")) return;
    retries_ = 0;
    if (status < 200 || status >= 300) {
      LOG_ERR("RWS", "Update %s failed: %d", entry.id.c_str(), status);
      ++stats_.failed;
      ++cursor_;
      return;
    }

    if (archive) {
      LOG_INF("RWS", "Archived %s", entry.id.c_str());
      removeBookFile(entry.file);
      const std::string id = entry.id;
      index_.erase(index_.begin() + static_cast<long>(cursor_));
      // Drop it from the queue view too, so it is neither re-downloaded
      // nor counted as "removed" by cleanup.
      const auto it = std::find_if(remote_.begin(), remote_.end(), [&](const RemoteDoc& d) { return d.id == id; });
      if (it != remote_.end()) {
        const size_t removedAt = static_cast<size_t>(it - remote_.begin());
        remote_.erase(it);
        toDownload_.erase(std::remove(toDownload_.begin(), toDownload_.end(), removedAt), toDownload_.end());
        for (auto& i : toDownload_) {
          if (i > removedAt) --i;
        }
      }
      ++stats_.archived;
    } else {
      entry.pushed = fraction;
      ++stats_.progress;
      ++cursor_;
    }
    saveIndex();
    return;
  }
  cursor_ = 0;
  phase_ = Phase::Cleanup;
}

void ReadwiseSync::stepCleanup() {
  currentTitle_.clear();
  cursor_ = 0;
  // An incomplete listing cannot tell "gone" from "not listed".
  if (listTruncated_) {
    phase_ = toDownload_.empty() ? Phase::Done : Phase::Download;
    return;
  }
  bool changed = false;
  for (size_t i = 0; i < index_.size();) {
    if (findRemote(index_[i].id)) {
      ++i;
      continue;
    }
    // Gone from the queue (archived or deleted in Readwise).
    if (removeArchived_ && !index_[i].file.empty() && Storage.exists(index_[i].file.c_str())) {
      LOG_INF("RWS", "Removing %s", index_[i].file.c_str());
      removeBookFile(index_[i].file);
      ++stats_.removed;
    }
    index_.erase(index_.begin() + static_cast<long>(i));
    changed = true;
  }
  if (changed) saveIndex();
  phase_ = toDownload_.empty() ? Phase::Done : Phase::Download;
}

void ReadwiseSync::stepDownload() {
  if (cursor_ >= toDownload_.size()) {
    currentTitle_.clear();
    phase_ = Phase::Done;
    return;
  }
  if (!ready(Limit::List)) return;
  const RemoteDoc& doc = remote_[toDownload_[cursor_]];
  currentTitle_ = doc.title;

  uint32_t retryAfter = 0;
  bool transportFailed = false;
  const bool ok = downloadArticle(doc, &retryAfter, &transportFailed);
  spent(Limit::List);
  if (!ok && retryAfter != UINT32_MAX && retryOr(Limit::List, retryAfter, "download")) return;
  retries_ = 0;
  if (!ok) {
    ++stats_.failed;
    if (transportFailed && stats_.downloaded == 0 && stats_.failed >= 3) {
      fail("Downloads keep failing; check Wi-Fi");
      return;
    }
  }
  ++cursor_;
}

// Returns true when the article was saved or deliberately skipped. On false,
// *retryAfter is the server's Retry-After for a 429, or UINT32_MAX when
// retrying would not help.
bool ReadwiseSync::downloadArticle(const RemoteDoc& doc, uint32_t* retryAfter, bool* transportFailed) {
  const std::string bodyTmp = folder_ + "/.rw-body.tmp";
  const std::string epubTmp = folder_ + "/.rw-epub.tmp";
  if (!Storage.ensureDirectoryExists(folder_.c_str())) {
    *retryAfter = UINT32_MAX;
    return false;
  }

  HalFile body;
  if (!Storage.openFileForWrite("RWS", bodyTmp, body)) {
    *retryAfter = UINT32_MAX;
    return false;
  }
  bool writeOk = true;
  uint32_t bodySize = 0;
  readwise::LanguageGuesser lang;
  readwise::XhtmlSanitizer sanitizer([&](const char* d, size_t n) {
    if (body.write(d, n) != n) {
      writeOk = false;
      return false;
    }
    bodySize += static_cast<uint32_t>(n);
    lang.feed(d, n);
    return true;
  });
  readwise::JsonStringStreamer html("html_content", [&](const char* d, size_t n) { return sanitizer.feed(d, n); });

  const std::string url =
      std::string(API) + "/v3/list/?id=" + pluginhttp::urlEncodeQuery(doc.id) + "&withHtmlContent=true";
  const int status = api("GET", url, "", retryAfter,
                         [&](const uint8_t* d, size_t n) { return html.feed(reinterpret_cast<const char*>(d), n); });
  sanitizer.finish();
  body.close();

  if (status == 429) {
    Storage.remove(bodyTmp.c_str());
    return false;  // *retryAfter holds the server's wait
  }
  *retryAfter = UINT32_MAX;
  if (status != 200 || !writeOk || (html.found() && !html.complete())) {
    LOG_ERR("RWS", "Download %s failed: status=%d write=%d", doc.id.c_str(), status, writeOk);
    *transportFailed = status < 0;
    Storage.remove(bodyTmp.c_str());
    return false;
  }

  if (!html.found() || sanitizer.textBytes() < MIN_TEXT_BYTES) {
    LOG_INF("RWS", "No readable text in %s (%s); skipping", doc.id.c_str(), doc.category.c_str());
    Storage.remove(bodyTmp.c_str());
    index_.push_back(IndexEntry{doc.id, "", 0});
    saveIndex();
    ++stats_.skipped;
    return true;
  }

  readwise::ArticleMeta meta;
  meta.id = doc.id;
  meta.title = doc.title.empty() ? "Untitled" : doc.title;
  meta.author = doc.author;
  meta.siteName = doc.siteName;
  meta.sourceUrl = doc.sourceUrl;
  meta.language = lang.result();

  bool built = false;
  {
    HalFile out;
    if (Storage.openFileForWrite("RWS", epubTmp, out)) {
      readwise::EpubBuilder builder([&](const uint8_t* d, size_t n) { return out.write(d, n) == n; });
      built = builder.writeArticle(meta, bodySize, [&](const readwise::EpubBuilder::Write& write) {
        HalFile in;
        if (!Storage.openFileForRead("RWS", bodyTmp, in)) return false;
        uint8_t buf[512];
        for (;;) {
          const int r = in.read(buf, sizeof(buf));
          if (r < 0) return false;
          if (r == 0) return true;
          if (!write(buf, static_cast<size_t>(r))) return false;
          resetTaskWatchdogIfSubscribed();
        }
      });
      out.flush();
      built = out.close() && built;
    }
  }
  Storage.remove(bodyTmp.c_str());
  if (!built) {
    LOG_ERR("RWS", "Could not write EPUB for %s", doc.id.c_str());
    Storage.remove(epubTmp.c_str());
    return false;
  }

  const std::string path = uniquePath(meta.title);
  if (!Storage.rename(epubTmp.c_str(), path.c_str())) {
    Storage.remove(epubTmp.c_str());
    return false;
  }
  clearBookCache(path);  // a stale cache could exist for a reused filename
  LOG_INF("RWS", "Saved %s (%u bytes text, %s)", path.c_str(), static_cast<unsigned>(sanitizer.textBytes()),
          meta.language.c_str());
  index_.push_back(IndexEntry{doc.id, path, doc.progress});
  saveIndex();
  ++stats_.downloaded;
  return true;
}

std::string ReadwiseSync::uniquePath(const std::string& title) const {
  const std::string name = opdsBookFilename("", title, OpdsFilenameFormat::TitleOnly);
  std::string path = folder_ + "/" + name;
  if (!Storage.exists(path.c_str())) return path;
  const auto dot = name.rfind('.');
  const std::string stem = dot == std::string::npos ? name : name.substr(0, dot);
  const std::string ext = dot == std::string::npos ? "" : name.substr(dot);
  for (int n = 2; n < 1000; ++n) {
    path = folder_ + "/" + stem + " (" + std::to_string(n) + ")" + ext;
    if (!Storage.exists(path.c_str())) return path;
  }
  return path;
}

ReadwiseSync::IndexEntry* ReadwiseSync::findEntry(const std::string& id) {
  for (auto& e : index_) {
    if (e.id == id) return &e;
  }
  return nullptr;
}

const ReadwiseSync::RemoteDoc* ReadwiseSync::findRemote(const std::string& id) const {
  for (const auto& d : remote_) {
    if (d.id == id) return &d;
  }
  return nullptr;
}

bool ReadwiseSync::loadIndex() {
  index_.clear();
  JsonDocument doc;
  if (!PersistableStoreBase::readDocFromFile(INDEX_PATH, doc)) return false;
  for (JsonVariantConst e : doc["docs"].as<JsonArrayConst>()) {
    const char* id = e["id"] | "";
    if (!*id) continue;
    index_.push_back(IndexEntry{id, e["f"] | "", e["p"] | 0.0f});
  }
  return true;
}

bool ReadwiseSync::saveIndex() const {
  JsonDocument doc;
  doc["v"] = 1;
  JsonArray docs = doc["docs"].to<JsonArray>();
  for (const auto& e : index_) {
    JsonObject o = docs.add<JsonObject>();
    o["id"] = e.id.c_str();
    if (!e.file.empty()) o["f"] = e.file.c_str();
    if (e.pushed > 0) o["p"] = e.pushed;
  }
  return PersistableStoreBase::writeDocToFile(INDEX_PATH, doc);
}
