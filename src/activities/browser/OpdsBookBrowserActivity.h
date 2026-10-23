#pragma once
#include <LcpLicense.h>
#include <OpdsClient.h>
#include <OpdsEntry.h>
#include <OpdsPublicationDoc.h>

#include <string>
#include <utility>
#include <vector>

#include "OpdsServerStore.h"
#include "activities/CatalogActivity.h"
#include "network/OpdsHttpTransport.h"

struct DetailsSpec;

/**
 * Activity for browsing and downloading books from an OPDS server.
 * Supports navigation through catalog hierarchy and downloading EPUBs.
 */
class OpdsBookBrowserActivity final : public CatalogActivity {
 public:
  explicit OpdsBookBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, OpdsServer server);

  void onEnter() override;
  void onExit() override;

 private:
  // Subclass actions after the base's ACTION_SEARCH/ACTION_CANCEL.
  static constexpr freeink::ui::ActionId ACTION_PAGE = ACTION_USER + 2;

  std::vector<OpdsEntry> entries;
  // Row buffer, built whenever entries changes (fetchFeed()/releaseEntries())
  // so buildBrowsingScreen() reuses it on every repaint instead of rebuilding
  // a ListItem vector per render.
  std::vector<freeink::ui::ListItem> rowItems;
  void rebuildRowItems();
  std::vector<std::string> navigationHistory;
  std::string currentPath;
  // The active search query. While browsing its results the header shows it
  // (quoted, library-view convention) instead of the server name, and
  // reopening search pre-fills the keyboard with it.
  std::string searchQuery;
  // Quoted form of searchQuery for the header; derived by setSearchQuery().
  std::string headerSearchTitle;
  // searchQuery per navigationHistory entry, pushed/popped in lockstep, so
  // Back restores the search term (or its absence) of the feed it returns to.
  std::vector<std::string> searchQueryHistory;
  // Raw pagination hrefs of the current feed: following them keeps the
  // search-term header (page 2 of results is still the same search).
  std::string pageNextHref;
  std::string pagePrevHref;
  std::string pageFirstHref;
  std::string pageLastHref;
  bool isPaginationHref(const std::string& href) const {
    return (!href.empty()) &&
           (href == pageNextHref || href == pagePrevHref || href == pageFirstHref || href == pageLastHref);
  }
  // Title of the current feed (shown in the header when no search is active).
  std::string feedTitle;
  // Feed-reported pagination ("Page N of M" between the arrows); 0 = unknown.
  int pageCurrent = 0;
  int pageTotal = 0;
  void setSearchQuery(const std::string& query);
  // The book the open details page acquires, and its cover art on an SD temp
  // (removed when the page closes).
  OpdsEntry detailBook;
  std::string detailCoverPath;
  // Raw search URL template ({searchTerms} or RFC 6570 {?query} style),
  // either inlined in the feed or fetched from an OpenSearch description.
  std::string searchTemplate;
  // OpenSearch description document URL (OPDS 1.x feeds that don't inline a
  // template); fetched lazily on first search.
  std::string searchDescriptionUrl;
  // Base URL the template is relative to: the OpenSearch description URL, or
  // empty when the template came from the feed itself (resolve against feed).
  std::string searchTemplateBase;

  OpdsServer server;  // Copied at construction — safe even if the store changes during browsing

  // All OPDS network orchestration (feed fetch, 401 auth, token refresh,
  // indirect acquisition, OpenSearch, publication docs) lives in the SDK
  // client; this activity only builds UI and drives file I/O. The transport
  // must outlive the client, so it is declared first.
  OpdsHttpTransport opdsTransport;
  freeink::opds::OpdsClient opdsClient{opdsTransport};

  // --- CatalogActivity / UiListActivity contract -----------------------------
  int listCount() const override { return state == State::BROWSING ? static_cast<int>(entries.size()) : 0; }
  bool hasSearch() const override { return !searchTemplate.empty() || !searchDescriptionUrl.empty(); }
  std::string searchPrefill() const override { return searchQuery; }
  void activateIndex(int index) override;
  void buildScreen(UiScreen& screen) override;
  void drawFooter() override;
  void startBrowse() override;
  void downloadFinished(bool cancelled) override;
  void performSearch(const std::string& query) override;
  void onBackButton() override;
  // Pagination side buttons; everything else
  // falls through to the shared catalog input handling.
  bool handleCustomInput() override;

  // Bottom pagination tab bar (arrow icons); drawn only when the feed
  // advertises next/previous/first/last links.
  void buildPaginationBar(UiScreen& screen);
  void followPageLink(const std::string& href);
  void buildBrowsingScreen(UiScreen& screen);
  static void onPageEvent(const freeink::ui::ActionEvent& event, void* user);

  // LCP acquisition: fetch the license, fulfill through the LCP service,
  // then prompt for the passphrase and store the device-wrapped book key.
  void downloadLcpBook(const OpdsEntry& book);
  enum class LcpUnlock { Ok, WrongPassphrase, Failed };
  // Content key wrapped to this device's enrolled X25519 key (base64 fields).
  struct LcpWrappedKey {
    std::string epk, iv, ct, tag;
  };
  LcpUnlock requestLcpUnlock(const std::string& userKeyHex);
  // Saved per-provider hash first; the keyboard only on a miss or a 403.
  void startLcpUnlock();
  void promptLcpPassphrase(bool retry);
  // Decodes the /unlock response ({content_key: b64, expires: epoch}).
  static bool parseUnlockResponse(const std::string& response, LcpWrappedKey* wrapped, int64_t* expiresAt);
  std::string downloadDestination(const OpdsEntry& book) const;
  bool verifyAndRegisterEpub(const std::string& filename);
  // Fulfilled LCP book awaiting its passphrase (path + parsed license).
  freeink::content::LcpLicense pendingLcpLicense;
  // Raw license JSON, kept verbatim for the /unlock request body.
  std::string pendingLcpLicenseText;
  std::string pendingLcpPath;

  void fetchFeed(const std::string& path);
  void releaseEntries();
  void navigateToEntry(const OpdsEntry& entry);
  void navigateBack();
  void downloadBook(const OpdsEntry& book);
  void openPublicationDetail(const OpdsEntry& entry);
  static void fillDetailInfo(const OpdsPublication& publication, DetailsSpec& spec);
  // Download the publication's cover art to an SD temp for the details page.
  void loadDetailCover(const std::string& docUrl, const std::string& coverHref);
  // Label for the details-page acquire button: Buy for a purchase, Place Hold
  // for a borrowable title with no copies available, otherwise Borrow (library
  // loan) or Download (direct file).
  const char* acquireLabel(const OpdsAvailability& av) const;
  const char* browserTitle() const;
  bool ensureSearchTemplate();
  // Client status hook: reflect the login phase in the status line.
  static void onClientStatus(void* ctx, freeink::opds::ClientPhase phase);
  // Persist the client's current token state (or clear it) for this server.
  // Returns false (and logs) if the SD write failed, so callers can detect it.
  bool persistTokens();
  // Token-store key: URL plus username, so two accounts on the same server
  // keep separate tokens. \x1f (unit separator) can't appear in either field.
  std::string tokenKey() const { return server.url + '\x1f' + server.username; }
};
