#pragma once

#include <cstddef>

#include "BookFusionSyncClient.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Browse and download books from the user's BookFusion library.
 *
 * Shows the user's library 20 books at a time (paginated).
 * Selecting a book fetches a pre-signed download URL, streams the EPUB
 * to the SD card, and writes a BookFusion sidecar via BookFusionBookIdStore
 * so that progress sync works immediately after download.
 *
 * Requires a linked BookFusion account (token in BF_TOKEN_STORE).
 */
class BookFusionBrowserActivity final : public Activity {
 public:
  explicit BookFusionBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("BookFusionBrowser", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }

 private:
  enum State { CATEGORY_SELECTION, WIFI_SELECTION, LOADING, BROWSING, DOWNLOADING, DOWNLOAD_COMPLETE, ERROR };

  State state = CATEGORY_SELECTION;
  ButtonNavigator buttonNavigator;

  BookFusionSearchResult searchResult;  // Current page of 20 books (~2.5 KB on heap)
  int selectedIndex = 0;
  int currentPage = 1;

  // Category menu: which item is highlighted, and which one we're browsing.
  int selectedCategory = 0;
  int currentCategory = 0;

  // Pre-signed S3 URLs from BookFusion have grown to ~1450+ chars; size for
  // headroom. strlcpy() in BookFusionSyncClient::getDownloadUrl silently
  // truncates if this is too small, which produces a bad URL and a failed
  // download with no obvious error.
  char downloadUrl[2048] = {};
  char downloadTitle[64] = {};
  size_t downloadProgress = 0;
  size_t downloadTotal = 0;

  char errorMsg[128] = {};

  void onWifiSelectionComplete(bool success);
  void handleCategorySelection();
  void loadPage(int page);
  void startDownload(int bookIndex);
};
