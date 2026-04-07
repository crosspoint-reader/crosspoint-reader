#pragma once

#include <ArduinoJson.h>

#include <string>
#include <vector>

#include "AozoraIndexManager.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class AozoraActivity : public Activity {
 public:
  explicit AozoraActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state_ == LOADING || state_ == DOWNLOADING; }
  bool skipLoopDelay() override { return true; }

 private:
  enum State {
    WIFI_SELECTION,
    TOP_MENU,
    KANA_SELECT,
    KANA_CHAR_SELECT,
    GENRE_SELECT,
    AUTHOR_LIST,
    WORK_LIST,
    WORK_DETAIL,
    DOWNLOADING,
    DOWNLOADED_LIST,
    LOADING,
    ERROR,
  };

  enum SearchMode { SEARCH_AUTHOR, SEARCH_TITLE };

  struct AuthorEntry {
    int id;
    char name[48];
    char kana[48];
    int workCount;
  };

  struct WorkEntry {
    int id;
    char title[80];
    char kana[48];
    char ndc[8];
    char author[48];
  };

  State state_ = WIFI_SELECTION;
  SearchMode searchMode_ = SEARCH_AUTHOR;
  ButtonNavigator buttonNavigator_;
  int selectedIndex_ = 0;
  std::string errorMessage_;

  // State history stack for Back navigation
  std::vector<State> stateStack_;
  std::vector<int> selectedIndexStack_;

  // API result buffers
  std::vector<AuthorEntry> authors_;
  std::vector<WorkEntry> works_;

  // Works pagination
  int worksTotal_ = 0;
  int worksOffset_ = 0;
  static constexpr int WORKS_PAGE_SIZE = 30;
  char lastWorksQuery_[64] = {};  // 再取得用にクエリを保持

  // Selected kana row index (for KANA_CHAR_SELECT)
  int selectedKanaRowIndex_ = 0;

  // 最後に使った作家検索の50音行（再取得用）
  char lastAuthorsKanaPrefix_[8] = {};

  // Selected item info (carried across states)
  int selectedAuthorId_ = 0;
  char selectedAuthorName_[48] = {};
  int selectedWorkId_ = 0;
  char selectedWorkTitle_[80] = {};
  char selectedWorkAuthor_[48] = {};

  // Download progress
  size_t downloadProgress_ = 0;
  size_t downloadTotal_ = 0;

  // Download index manager
  AozoraIndexManager indexManager_;

  static constexpr const char* API_BASE = "https://aozora-epub-api.vercel.app";

  // State navigation
  void pushState(State newState);
  void popState();

  // WiFi
  void onWifiSelectionComplete(bool success);

  // API calls (blocking -- call from correct state)
  bool fetchAuthors(const char* kanaPrefix);
  bool fetchWorks(const char* queryParam);
  bool downloadBook();

  // JSON parsing
  bool parseAuthorsJson(JsonDocument& doc);
  bool parseWorksJson(JsonDocument& doc);
};
