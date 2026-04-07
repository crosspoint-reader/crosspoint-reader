# 青空文庫デバイス側 実装プラン

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** CrossPoint Readerデバイスから青空文庫の書籍を検索・ダウンロードできるAozoraActivityを実装する

**Architecture:** 単一Activity＋ステートマシン方式。FontDownloadActivityのパターンを踏襲し、WiFi接続→API呼び出し→リスト表示→ダウンロードの流れを実装。ダウンロード済みEPUBは `/Aozora/` に保存し、JSONインデックスで管理。

**Tech Stack:** C++20, Arduino-ESP32, ArduinoJson, HttpDownloader（既存）, UITheme（既存）

**Spec:** `docs/superpowers/specs/2026-04-07-aozora-bunko-download-design.md`

**前提:** Vercel API（`aozora-epub-api`）がデプロイ済みであること

---

## ファイル構成

```
src/activities/settings/
├── AozoraActivity.h              ← Activity定義、State enum、メンバ変数
└── AozoraActivity.cpp            ← 全状態の実装（WiFi、検索、ダウンロード）

src/
├── AozoraIndexManager.h          ← /Aozora/.aozora_index.json の読み書き
├── AozoraIndexManager.cpp
└── SettingsList.h                ← 修正: SettingAction::AozoraBunko追加

src/activities/settings/
└── SettingsActivity.cpp          ← 修正: AozoraActivity起動追加

lib/I18n/translations/
├── japanese.yaml                 ← 修正: 青空文庫関連キー追加
└── english.yaml                  ← 修正: 同上
```

---

### Task 1: i18n翻訳キー追加

**Files:**
- Modify: `lib/I18n/translations/japanese.yaml`
- Modify: `lib/I18n/translations/english.yaml`

- [ ] **Step 1: japanese.yamlに青空文庫関連キーを追加**

ファイル末尾に以下を追加:

```yaml
STR_AOZORA_BUNKO: "青空文庫"
STR_SEARCH_BY_AUTHOR: "作家から探す"
STR_SEARCH_BY_TITLE: "作品名から探す"
STR_SEARCH_BY_GENRE: "ジャンルから探す"
STR_NEWEST_WORKS: "新着作品"
STR_DOWNLOADED_BOOKS: "ダウンロード済み"
STR_ALREADY_DOWNLOADED: "ダウンロード済み"
STR_DOWNLOAD_CONFIRM: "ダウンロードしますか？"
STR_DELETE_CONFIRM: "削除しますか？"
STR_DOWNLOADING_BOOK: "ダウンロード中..."
STR_CONNECTION_FAILED: "サーバーに接続できません"
STR_DOWNLOAD_COMPLETE: "ダウンロード完了"
STR_DOWNLOAD_FAILED: "ダウンロードに失敗しました"
STR_DELETE_COMPLETE: "削除しました"
STR_LOADING_AUTHORS: "作家一覧を読み込み中..."
STR_LOADING_WORKS: "作品一覧を読み込み中..."
STR_NO_RESULTS: "該当する作品がありません"
STR_KANA_ROW_A: "あ行"
STR_KANA_ROW_KA: "か行"
STR_KANA_ROW_SA: "さ行"
STR_KANA_ROW_TA: "た行"
STR_KANA_ROW_NA: "な行"
STR_KANA_ROW_HA: "は行"
STR_KANA_ROW_MA: "ま行"
STR_KANA_ROW_YA: "や行"
STR_KANA_ROW_RA: "ら行"
STR_KANA_ROW_WA: "わ行"
STR_GENRE_NOVEL: "小説"
STR_GENRE_POETRY: "詩歌"
STR_GENRE_ESSAY: "随筆・エッセイ"
STR_GENRE_DRAMA: "戯曲"
STR_GENRE_FAIRY_TALE: "童話"
STR_GENRE_OTHER: "その他"
```

- [ ] **Step 2: english.yamlに対応する英語キーを追加**

```yaml
STR_AOZORA_BUNKO: "Aozora Bunko"
STR_SEARCH_BY_AUTHOR: "Search by Author"
STR_SEARCH_BY_TITLE: "Search by Title"
STR_SEARCH_BY_GENRE: "Search by Genre"
STR_NEWEST_WORKS: "Newest Works"
STR_DOWNLOADED_BOOKS: "Downloaded"
STR_ALREADY_DOWNLOADED: "Already Downloaded"
STR_DOWNLOAD_CONFIRM: "Download this book?"
STR_DELETE_CONFIRM: "Delete this book?"
STR_DOWNLOADING_BOOK: "Downloading..."
STR_CONNECTION_FAILED: "Cannot connect to server"
STR_DOWNLOAD_COMPLETE: "Download complete"
STR_DOWNLOAD_FAILED: "Download failed"
STR_DELETE_COMPLETE: "Deleted"
STR_LOADING_AUTHORS: "Loading authors..."
STR_LOADING_WORKS: "Loading works..."
STR_NO_RESULTS: "No results found"
STR_KANA_ROW_A: "A row"
STR_KANA_ROW_KA: "Ka row"
STR_KANA_ROW_SA: "Sa row"
STR_KANA_ROW_TA: "Ta row"
STR_KANA_ROW_NA: "Na row"
STR_KANA_ROW_HA: "Ha row"
STR_KANA_ROW_MA: "Ma row"
STR_KANA_ROW_YA: "Ya row"
STR_KANA_ROW_RA: "Ra row"
STR_KANA_ROW_WA: "Wa row"
STR_GENRE_NOVEL: "Novel"
STR_GENRE_POETRY: "Poetry"
STR_GENRE_ESSAY: "Essay"
STR_GENRE_DRAMA: "Drama"
STR_GENRE_FAIRY_TALE: "Fairy Tale"
STR_GENRE_OTHER: "Other"
```

- [ ] **Step 3: i18nヘッダー再生成**

```bash
python scripts/gen_i18n.py lib/I18n/translations lib/I18n/
```

- [ ] **Step 4: ビルド確認**

```bash
pio run
```

Expected: SUCCESS

- [ ] **Step 5: コミット**

```bash
git add lib/I18n/translations/japanese.yaml lib/I18n/translations/english.yaml
git commit -m "✨ 青空文庫機能のi18n翻訳キーを追加（日本語・英語）"
```

---

### Task 2: SettingAction追加とメニュー統合

**Files:**
- Modify: `src/SettingsList.h` — `SettingAction` enumに `AozoraBunko` 追加、systemSettingsにACTION追加
- Modify: `src/activities/settings/SettingsActivity.cpp` — `toggleCurrentSetting()`にcase追加

- [ ] **Step 1: SettingsList.hにSettingAction追加**

`SettingAction` enumに追加:
```cpp
SettingAction::AozoraBunko,
```

`getSettingsList()` 内のsystemSettingsセクションで、`DownloadFonts` の近くに追加:
```cpp
systemSettings.push_back(SettingInfo::Action(StrId::STR_AOZORA_BUNKO, SettingAction::AozoraBunko));
```

- [ ] **Step 2: SettingsActivity.cppにcase追加**

`toggleCurrentSetting()` 内のswitch文に追加:
```cpp
case SettingAction::AozoraBunko:
  startActivityForResult(
      std::make_unique<AozoraActivity>(renderer, mappedInput),
      [this](const ActivityResult&) {
        SETTINGS.saveToFile();
      });
  break;
```

ファイル先頭にinclude追加:
```cpp
#include "AozoraActivity.h"
```

- [ ] **Step 3: ビルド確認**

```bash
pio run
```

Expected: FAIL（AozoraActivity未定義）— これは次のTaskで解消

- [ ] **Step 4: コミット**

```bash
git add src/SettingsList.h src/activities/settings/SettingsActivity.cpp
git commit -m "✨ 設定メニューに青空文庫ACTION項目を追加"
```

---

### Task 3: AozoraIndexManager（ダウンロード管理）

**Files:**
- Create: `src/AozoraIndexManager.h`
- Create: `src/AozoraIndexManager.cpp`

- [ ] **Step 1: AozoraIndexManager.h作成**

```cpp
#pragma once

#include <string>
#include <vector>
#include <HalStorage.h>

struct AozoraBookEntry {
  int workId;
  char title[64];
  char author[32];
  char filename[80];
};

class AozoraIndexManager {
 public:
  static constexpr const char* AOZORA_DIR = "/Aozora";
  static constexpr const char* INDEX_PATH = "/Aozora/.aozora_index.json";

  /** インデックスをSDカードから読み込み、存在しないファイルをパージする */
  bool loadAndPurge();

  /** 指定work_idがダウンロード済みか */
  bool isDownloaded(int workId) const;

  /** ダウンロード完了時にエントリ追加して保存 */
  bool addEntry(int workId, const char* title, const char* author, const char* filename);

  /** エントリ削除（ファイルも削除）して保存 */
  bool removeEntry(int workId);

  /** ダウンロード済みの一覧を返す */
  const std::vector<AozoraBookEntry>& entries() const { return entries_; }

  /** work_idからファイル名を生成（FAT32安全） */
  static std::string makeFilename(int workId, const char* title);

  /** /Aozora/ディレクトリを作成（存在しなければ） */
  static bool ensureDirectory();

 private:
  std::vector<AozoraBookEntry> entries_;

  bool saveIndex() const;
};
```

- [ ] **Step 2: AozoraIndexManager.cpp作成**

```cpp
#include "AozoraIndexManager.h"
#include <ArduinoJson.h>
#include <Logging.h>

bool AozoraIndexManager::loadAndPurge() {
  entries_.clear();

  FsFile file;
  if (!Storage.openFileForRead("AOZORA", INDEX_PATH, file)) {
    LOG_DBG("AOZORA", "No index file, starting fresh");
    return true;  // ファイルなし = 空インデックス、正常
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, file);
  file.close();

  if (err) {
    LOG_ERR("AOZORA", "Index parse error: %s", err.c_str());
    return true;  // 壊れたインデックスは無視して空で続行
  }

  JsonArray arr = doc.as<JsonArray>();
  entries_.reserve(arr.size());

  bool needsSave = false;
  for (JsonObject obj : arr) {
    AozoraBookEntry entry;
    entry.workId = obj["work_id"] | 0;
    snprintf(entry.title, sizeof(entry.title), "%s", (const char*)(obj["title"] | ""));
    snprintf(entry.author, sizeof(entry.author), "%s", (const char*)(obj["author"] | ""));
    snprintf(entry.filename, sizeof(entry.filename), "%s", (const char*)(obj["filename"] | ""));

    // ファイル存在チェック
    char fullPath[128];
    snprintf(fullPath, sizeof(fullPath), "%s/%s", AOZORA_DIR, entry.filename);

    if (Storage.exists(fullPath)) {
      entries_.push_back(entry);
    } else {
      LOG_DBG("AOZORA", "Purging missing file: %s", entry.filename);
      needsSave = true;
    }
  }

  if (needsSave) {
    saveIndex();
  }

  return true;
}

bool AozoraIndexManager::isDownloaded(int workId) const {
  for (const auto& e : entries_) {
    if (e.workId == workId) return true;
  }
  return false;
}

bool AozoraIndexManager::addEntry(int workId, const char* title, const char* author, const char* filename) {
  if (isDownloaded(workId)) return true;  // 既に登録済み

  AozoraBookEntry entry;
  entry.workId = workId;
  snprintf(entry.title, sizeof(entry.title), "%s", title);
  snprintf(entry.author, sizeof(entry.author), "%s", author);
  snprintf(entry.filename, sizeof(entry.filename), "%s", filename);
  entries_.push_back(entry);

  return saveIndex();
}

bool AozoraIndexManager::removeEntry(int workId) {
  for (auto it = entries_.begin(); it != entries_.end(); ++it) {
    if (it->workId == workId) {
      // SDカードのファイルも削除
      char fullPath[128];
      snprintf(fullPath, sizeof(fullPath), "%s/%s", AOZORA_DIR, it->filename);
      Storage.remove(fullPath);

      entries_.erase(it);
      return saveIndex();
    }
  }
  return false;
}

bool AozoraIndexManager::saveIndex() const {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();

  for (const auto& e : entries_) {
    JsonObject obj = arr.add<JsonObject>();
    obj["work_id"] = e.workId;
    obj["title"] = e.title;
    obj["author"] = e.author;
    obj["filename"] = e.filename;
  }

  FsFile file;
  if (!Storage.openFileForWrite("AOZORA", INDEX_PATH, file)) {
    LOG_ERR("AOZORA", "Failed to open index for write");
    return false;
  }

  serializeJson(doc, file);
  file.close();
  return true;
}

std::string AozoraIndexManager::makeFilename(int workId, const char* title) {
  // FAT32安全なファイル名生成
  char safeName[64];
  int pos = 0;
  for (int i = 0; title[i] && pos < 50; i++) {
    char c = title[i];
    // FAT32禁止文字をスキップ
    if (c == '<' || c == '>' || c == ':' || c == '"' ||
        c == '/' || c == '\\' || c == '|' || c == '?' || c == '*') {
      safeName[pos++] = '_';
    } else {
      // UTF-8マルチバイト文字はそのまま通す
      safeName[pos++] = c;
    }
  }
  safeName[pos] = '\0';

  char result[80];
  snprintf(result, sizeof(result), "%d_%s.epub", workId, safeName);
  return std::string(result);
}

bool AozoraIndexManager::ensureDirectory() {
  if (Storage.exists(AOZORA_DIR)) return true;
  return Storage.mkdir(AOZORA_DIR);
}
```

- [ ] **Step 3: ビルド確認**

```bash
pio run
```

Expected: SUCCESS（AozoraIndexManagerは独立してビルド可能）

- [ ] **Step 4: コミット**

```bash
git add src/AozoraIndexManager.h src/AozoraIndexManager.cpp
git commit -m "✨ AozoraIndexManager追加（ダウンロード済みインデックスの読み書き・パージ）"
```

---

### Task 4: AozoraActivity ヘッダー定義

**Files:**
- Create: `src/activities/settings/AozoraActivity.h`

- [ ] **Step 1: AozoraActivity.h作成**

```cpp
#pragma once

#include <string>
#include <vector>
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"
#include "network/HttpDownloader.h"
#include "AozoraIndexManager.h"

class AozoraActivity final : public Activity {
 public:
  AozoraActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override;

 private:
  enum class State {
    WIFI_SELECTION,
    TOP_MENU,
    KANA_SELECT,
    GENRE_SELECT,
    AUTHOR_LIST,
    WORK_LIST,
    WORK_DETAIL,
    DOWNLOADING,
    DOWNLOADED_LIST,
    LOADING,
    ERROR,
  };

  enum class SearchMode {
    AUTHOR,
    TITLE,
  };

  struct AuthorEntry {
    int id;
    char name[32];
    char kana[48];
    int workCount;
  };

  struct WorkEntry {
    int id;
    char title[64];
    char kana[48];
    char ndc[8];
  };

  // 50音行の定義
  struct KanaRow {
    StrId labelId;
    const char* apiPrefix;  // API送信用カタカナ行頭文字
  };

  static constexpr const char* API_BASE_URL = "https://aozora-epub-api.vercel.app";

  // NDCジャンル定義
  struct GenreEntry {
    StrId labelId;
    const char* ndc;
  };

  State state_ = State::WIFI_SELECTION;
  SearchMode searchMode_ = SearchMode::AUTHOR;
  int selectedIndex_ = 0;
  std::string errorMessage_;

  // ステート履歴スタック（「戻る」用）
  std::vector<State> stateStack_;

  // API結果バッファ（状態遷移時にclear + 再利用）
  std::vector<AuthorEntry> authors_;
  std::vector<WorkEntry> works_;

  // 選択中の情報
  int selectedAuthorId_ = 0;
  int selectedWorkId_ = 0;
  char selectedWorkTitle_[64] = {};
  char selectedWorkAuthor_[32] = {};

  // ダウンロード進捗
  size_t downloadProgress_ = 0;
  size_t downloadTotal_ = 0;

  // UI
  ButtonNavigator buttonNavigator_;

  // ダウンロード管理
  AozoraIndexManager indexManager_;

  // State遷移
  void pushState(State newState);
  void popState();

  // WiFi
  void onWifiSelectionComplete(bool success);

  // API呼び出し
  bool fetchAuthors(const char* kanaPrefix);
  bool fetchWorks(const char* queryParam);
  bool downloadBook(int workId, const char* title, const char* author);

  // JSON解析
  bool parseAuthorsJson(const std::string& json);
  bool parseWorksJson(const std::string& json);

  // 描画ヘルパー
  void renderTopMenu(int pageWidth, int pageHeight);
  void renderKanaSelect(int pageWidth, int pageHeight);
  void renderGenreSelect(int pageWidth, int pageHeight);
  void renderAuthorList(int pageWidth, int pageHeight);
  void renderWorkList(int pageWidth, int pageHeight);
  void renderWorkDetail(int pageWidth, int pageHeight);
  void renderDownloading(int pageWidth, int pageHeight);
  void renderDownloadedList(int pageWidth, int pageHeight);
  void renderLoading(int pageWidth, int pageHeight);
  void renderError(int pageWidth, int pageHeight);

  // 入力ハンドラ
  void handleTopMenuInput();
  void handleKanaSelectInput();
  void handleGenreSelectInput();
  void handleAuthorListInput();
  void handleWorkListInput();
  void handleWorkDetailInput();
  void handleDownloadedListInput();
  void handleErrorInput();
};
```

- [ ] **Step 2: ビルド確認（ヘッダーのみ、cppはまだ）**

この時点ではcppがないのでリンクエラーになるが、ヘッダーの文法エラーがないことを確認:
```bash
pio run 2>&1 | head -20
```

Expected: ヘッダーの構文エラーなし（リンクエラーは許容）

- [ ] **Step 3: コミット**

```bash
git add src/activities/settings/AozoraActivity.h
git commit -m "✨ AozoraActivity ヘッダー定義（State enum、メンバ変数、メソッドシグネチャ）"
```

---

### Task 5: AozoraActivity基本実装（WiFi接続・TOP_MENU・状態遷移）

**Files:**
- Create: `src/activities/settings/AozoraActivity.cpp`

- [ ] **Step 1: AozoraActivity.cpp基本部分を作成**

```cpp
#include "AozoraActivity.h"
#include <WiFi.h>
#include <ArduinoJson.h>
#include <Logging.h>
#include <I18n.h>
#include <HalStorage.h>
#include "activities/network/WifiSelectionActivity.h"
#include "fontIds.h"

// 50音行定義
static constexpr AozoraActivity::KanaRow KANA_ROWS[] = {
    {StrId::STR_KANA_ROW_A, "ア"},
    {StrId::STR_KANA_ROW_KA, "カ"},
    {StrId::STR_KANA_ROW_SA, "サ"},
    {StrId::STR_KANA_ROW_TA, "タ"},
    {StrId::STR_KANA_ROW_NA, "ナ"},
    {StrId::STR_KANA_ROW_HA, "ハ"},
    {StrId::STR_KANA_ROW_MA, "マ"},
    {StrId::STR_KANA_ROW_YA, "ヤ"},
    {StrId::STR_KANA_ROW_RA, "ラ"},
    {StrId::STR_KANA_ROW_WA, "ワ"},
};
static constexpr int KANA_ROW_COUNT = sizeof(KANA_ROWS) / sizeof(KANA_ROWS[0]);

// ジャンル定義
static constexpr AozoraActivity::GenreEntry GENRES[] = {
    {StrId::STR_GENRE_NOVEL, "913"},
    {StrId::STR_GENRE_POETRY, "911"},
    {StrId::STR_GENRE_ESSAY, "914"},
    {StrId::STR_GENRE_DRAMA, "912"},
    {StrId::STR_GENRE_FAIRY_TALE, "388"},
};
static constexpr int GENRE_COUNT = sizeof(GENRES) / sizeof(GENRES[0]);

// トップメニュー項目数
static constexpr int TOP_MENU_COUNT = 5;

AozoraActivity::AozoraActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Aozora", renderer, mappedInput) {}

void AozoraActivity::onEnter() {
  Activity::onEnter();

  // ダウンロード済みインデックスの読み込み・パージ
  AozoraIndexManager::ensureDirectory();
  indexManager_.loadAndPurge();

  // WiFi接続開始
  WiFi.mode(WIFI_STA);
  startActivityForResult(
      std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
      [this](const ActivityResult& result) {
        onWifiSelectionComplete(!result.isCancelled);
      });
}

void AozoraActivity::onExit() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Activity::onExit();
}

void AozoraActivity::onWifiSelectionComplete(bool success) {
  if (!success) {
    finish();
    return;
  }

  // TLSバッファ用メモリ確保のためフォントキャッシュ解放
  // （FontDownloadActivityと同パターン）
  // FontManager::getInstance() のキャッシュ解放はオプション

  {
    RenderLock lock(*this);
    state_ = State::TOP_MENU;
    selectedIndex_ = 0;
    stateStack_.clear();
  }
  requestUpdateAndWait();
}

bool AozoraActivity::preventAutoSleep() {
  return state_ == State::LOADING || state_ == State::DOWNLOADING;
}

// --- State遷移 ---

void AozoraActivity::pushState(State newState) {
  stateStack_.push_back(state_);
  state_ = newState;
  selectedIndex_ = 0;
}

void AozoraActivity::popState() {
  if (stateStack_.empty()) {
    finish();
    return;
  }
  state_ = stateStack_.back();
  stateStack_.pop_back();
  selectedIndex_ = 0;
}

// --- loop() ---

void AozoraActivity::loop() {
  if (state_ == State::WIFI_SELECTION || state_ == State::LOADING ||
      state_ == State::DOWNLOADING) {
    return;  // これらの状態では入力を受け付けない
  }

  mappedInput.update();

  switch (state_) {
    case State::TOP_MENU:
      handleTopMenuInput();
      break;
    case State::KANA_SELECT:
      handleKanaSelectInput();
      break;
    case State::GENRE_SELECT:
      handleGenreSelectInput();
      break;
    case State::AUTHOR_LIST:
      handleAuthorListInput();
      break;
    case State::WORK_LIST:
      handleWorkListInput();
      break;
    case State::WORK_DETAIL:
      handleWorkDetailInput();
      break;
    case State::DOWNLOADED_LIST:
      handleDownloadedListInput();
      break;
    case State::ERROR:
      handleErrorInput();
      break;
    default:
      break;
  }
}

// --- render() ---

void AozoraActivity::render(RenderLock&&) {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  renderer.clearScreen();

  GUI.drawHeader(renderer,
                 Rect{0, GUI.getMetrics().topPadding, pageWidth, GUI.getMetrics().headerHeight},
                 tr(STR_AOZORA_BUNKO));

  switch (state_) {
    case State::TOP_MENU:
      renderTopMenu(pageWidth, pageHeight);
      break;
    case State::KANA_SELECT:
      renderKanaSelect(pageWidth, pageHeight);
      break;
    case State::GENRE_SELECT:
      renderGenreSelect(pageWidth, pageHeight);
      break;
    case State::AUTHOR_LIST:
      renderAuthorList(pageWidth, pageHeight);
      break;
    case State::WORK_LIST:
      renderWorkList(pageWidth, pageHeight);
      break;
    case State::WORK_DETAIL:
      renderWorkDetail(pageWidth, pageHeight);
      break;
    case State::DOWNLOADING:
      renderDownloading(pageWidth, pageHeight);
      break;
    case State::DOWNLOADED_LIST:
      renderDownloadedList(pageWidth, pageHeight);
      break;
    case State::LOADING:
      renderLoading(pageWidth, pageHeight);
      break;
    case State::ERROR:
      renderError(pageWidth, pageHeight);
      break;
    default:
      break;
  }

  renderer.displayBuffer();
}
```

- [ ] **Step 2: ビルド確認**

```bash
pio run
```

Expected: リンクエラー（入力ハンドラ・描画ヘルパーが未定義）は許容。構文エラーがないことを確認。

- [ ] **Step 3: コミット**

```bash
git add src/activities/settings/AozoraActivity.cpp
git commit -m "✨ AozoraActivity基本実装（WiFi接続、State遷移、loop/renderスケルトン）"
```

---

### Task 6: TOP_MENU・KANA_SELECT・GENRE_SELECTの入力＆描画

**Files:**
- Modify: `src/activities/settings/AozoraActivity.cpp`

- [ ] **Step 1: トップメニュー入力ハンドラ追加**

```cpp
void AozoraActivity::handleTopMenuInput() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  buttonNavigator_.onNextRelease([this] {
    if (selectedIndex_ < TOP_MENU_COUNT - 1) {
      selectedIndex_++;
      requestUpdate();
    }
  });

  buttonNavigator_.onPreviousRelease([this] {
    if (selectedIndex_ > 0) {
      selectedIndex_--;
      requestUpdate();
    }
  });

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    RenderLock lock(*this);
    switch (selectedIndex_) {
      case 0:  // 作家から探す
        searchMode_ = SearchMode::AUTHOR;
        pushState(State::KANA_SELECT);
        break;
      case 1:  // 作品名から探す
        searchMode_ = SearchMode::TITLE;
        pushState(State::KANA_SELECT);
        break;
      case 2:  // ジャンルから探す
        pushState(State::GENRE_SELECT);
        break;
      case 3:  // 新着作品
        pushState(State::LOADING);
        break;
      case 4:  // ダウンロード済み
        pushState(State::DOWNLOADED_LIST);
        break;
    }
  }

  // 新着の場合はすぐにAPI呼び出し
  if (state_ == State::LOADING && stateStack_.back() == State::TOP_MENU) {
    requestUpdateAndWait();
    if (fetchWorks("sort=newest&limit=50")) {
      RenderLock lock(*this);
      state_ = State::WORK_LIST;
    } else {
      RenderLock lock(*this);
      state_ = State::ERROR;
    }
    requestUpdate();
  }
}
```

- [ ] **Step 2: 50音選択入力ハンドラ追加**

```cpp
void AozoraActivity::handleKanaSelectInput() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    RenderLock lock(*this);
    popState();
    requestUpdate();
    return;
  }

  buttonNavigator_.onNextRelease([this] {
    if (selectedIndex_ < KANA_ROW_COUNT - 1) {
      selectedIndex_++;
      requestUpdate();
    }
  });

  buttonNavigator_.onPreviousRelease([this] {
    if (selectedIndex_ > 0) {
      selectedIndex_--;
      requestUpdate();
    }
  });

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    const char* prefix = KANA_ROWS[selectedIndex_].apiPrefix;

    {
      RenderLock lock(*this);
      pushState(State::LOADING);
    }
    requestUpdateAndWait();

    bool success;
    if (searchMode_ == SearchMode::AUTHOR) {
      char param[64];
      snprintf(param, sizeof(param), "kana_prefix=%s", prefix);
      success = fetchAuthors(prefix);
      if (success) {
        RenderLock lock(*this);
        state_ = State::AUTHOR_LIST;
      }
    } else {
      char param[64];
      snprintf(param, sizeof(param), "kana_prefix=%s", prefix);
      success = fetchWorks(param);
      if (success) {
        RenderLock lock(*this);
        state_ = State::WORK_LIST;
      }
    }

    if (!success) {
      RenderLock lock(*this);
      state_ = State::ERROR;
    }
    requestUpdate();
  }
}
```

- [ ] **Step 3: ジャンル選択入力ハンドラ追加**

```cpp
void AozoraActivity::handleGenreSelectInput() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    RenderLock lock(*this);
    popState();
    requestUpdate();
    return;
  }

  buttonNavigator_.onNextRelease([this] {
    if (selectedIndex_ < GENRE_COUNT - 1) {
      selectedIndex_++;
      requestUpdate();
    }
  });

  buttonNavigator_.onPreviousRelease([this] {
    if (selectedIndex_ > 0) {
      selectedIndex_--;
      requestUpdate();
    }
  });

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    const char* ndc = GENRES[selectedIndex_].ndc;

    {
      RenderLock lock(*this);
      pushState(State::LOADING);
    }
    requestUpdateAndWait();

    char param[32];
    snprintf(param, sizeof(param), "ndc=%s", ndc);
    bool success = fetchWorks(param);

    if (success) {
      RenderLock lock(*this);
      state_ = State::WORK_LIST;
    } else {
      RenderLock lock(*this);
      state_ = State::ERROR;
    }
    requestUpdate();
  }
}
```

- [ ] **Step 4: 描画ヘルパー追加（TOP_MENU, KANA_SELECT, GENRE_SELECT）**

```cpp
void AozoraActivity::renderTopMenu(int pageWidth, int pageHeight) {
  const auto& m = GUI.getMetrics();
  const Rect contentRect = {m.contentSidePadding,
                            m.topPadding + m.headerHeight + m.verticalSpacing,
                            pageWidth - m.contentSidePadding * 2,
                            pageHeight - m.topPadding - m.headerHeight - m.verticalSpacing - m.buttonHintsHeight};

  const StrId labels[TOP_MENU_COUNT] = {
      StrId::STR_SEARCH_BY_AUTHOR,
      StrId::STR_SEARCH_BY_TITLE,
      StrId::STR_SEARCH_BY_GENRE,
      StrId::STR_NEWEST_WORKS,
      StrId::STR_DOWNLOADED_BOOKS,
  };

  GUI.drawList(renderer, contentRect, TOP_MENU_COUNT, selectedIndex_,
               [&labels](int index) -> std::string { return tr(labels[index]); },
               nullptr, nullptr, nullptr, true, nullptr);
}

void AozoraActivity::renderKanaSelect(int pageWidth, int pageHeight) {
  const auto& m = GUI.getMetrics();
  const Rect contentRect = {m.contentSidePadding,
                            m.topPadding + m.headerHeight + m.verticalSpacing,
                            pageWidth - m.contentSidePadding * 2,
                            pageHeight - m.topPadding - m.headerHeight - m.verticalSpacing - m.buttonHintsHeight};

  GUI.drawList(renderer, contentRect, KANA_ROW_COUNT, selectedIndex_,
               [](int index) -> std::string { return tr(KANA_ROWS[index].labelId); },
               nullptr, nullptr, nullptr, true, nullptr);
}

void AozoraActivity::renderGenreSelect(int pageWidth, int pageHeight) {
  const auto& m = GUI.getMetrics();
  const Rect contentRect = {m.contentSidePadding,
                            m.topPadding + m.headerHeight + m.verticalSpacing,
                            pageWidth - m.contentSidePadding * 2,
                            pageHeight - m.topPadding - m.headerHeight - m.verticalSpacing - m.buttonHintsHeight};

  GUI.drawList(renderer, contentRect, GENRE_COUNT, selectedIndex_,
               [](int index) -> std::string { return tr(GENRES[index].labelId); },
               nullptr, nullptr, nullptr, true, nullptr);
}

void AozoraActivity::renderLoading(int pageWidth, int pageHeight) {
  const int centerY = pageHeight / 2;
  renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_LOADING_WORKS));
}

void AozoraActivity::renderError(int pageWidth, int pageHeight) {
  const int centerY = pageHeight / 2;
  renderer.drawCenteredText(UI_10_FONT_ID, centerY,
                            errorMessage_.empty() ? tr(STR_CONNECTION_FAILED) : errorMessage_.c_str());
}

void AozoraActivity::handleErrorInput() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    RenderLock lock(*this);
    popState();
    requestUpdate();
  }
}
```

- [ ] **Step 5: ビルド確認**

```bash
pio run
```

Expected: リンクエラー（AUTHOR_LIST等の未実装ハンドラ）は許容。追加分のコンパイルが通ること。

- [ ] **Step 6: コミット**

```bash
git add src/activities/settings/AozoraActivity.cpp
git commit -m "✨ TOP_MENU・KANA_SELECT・GENRE_SELECTの入力＆描画を実装"
```

---

### Task 7: API通信＆JSONパース

**Files:**
- Modify: `src/activities/settings/AozoraActivity.cpp`

- [ ] **Step 1: fetchAuthors実装**

```cpp
bool AozoraActivity::fetchAuthors(const char* kanaPrefix) {
  char url[256];
  snprintf(url, sizeof(url), "%s/api/authors?kana_prefix=%s", API_BASE_URL, kanaPrefix);

  std::string jsonContent;
  if (!HttpDownloader::fetchUrl(std::string(url), jsonContent)) {
    LOG_ERR("AOZORA", "fetchAuthors failed: http=%d", HttpDownloader::lastHttpCode);
    errorMessage_ = tr(STR_CONNECTION_FAILED);
    return false;
  }

  return parseAuthorsJson(jsonContent);
}

bool AozoraActivity::parseAuthorsJson(const std::string& json) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    LOG_ERR("AOZORA", "JSON parse error: %s", err.c_str());
    return false;
  }

  // エラーレスポンスチェック
  if (doc["error"].is<const char*>()) {
    errorMessage_ = doc["message"] | tr(STR_CONNECTION_FAILED);
    return false;
  }

  authors_.clear();
  JsonArray arr = doc["authors"].as<JsonArray>();
  authors_.reserve(arr.size());

  for (JsonObject obj : arr) {
    AuthorEntry entry;
    entry.id = obj["id"] | 0;
    snprintf(entry.name, sizeof(entry.name), "%s", (const char*)(obj["name"] | ""));
    snprintf(entry.kana, sizeof(entry.kana), "%s", (const char*)(obj["kana"] | ""));
    entry.workCount = obj["work_count"] | 0;
    authors_.push_back(entry);
  }

  return true;
}
```

- [ ] **Step 2: fetchWorks実装**

```cpp
bool AozoraActivity::fetchWorks(const char* queryParam) {
  char url[256];
  snprintf(url, sizeof(url), "%s/api/works?%s", API_BASE_URL, queryParam);

  std::string jsonContent;
  if (!HttpDownloader::fetchUrl(std::string(url), jsonContent)) {
    LOG_ERR("AOZORA", "fetchWorks failed: http=%d", HttpDownloader::lastHttpCode);
    errorMessage_ = tr(STR_CONNECTION_FAILED);
    return false;
  }

  return parseWorksJson(jsonContent);
}

bool AozoraActivity::parseWorksJson(const std::string& json) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    LOG_ERR("AOZORA", "JSON parse error: %s", err.c_str());
    return false;
  }

  if (doc["error"].is<const char*>()) {
    errorMessage_ = doc["message"] | tr(STR_CONNECTION_FAILED);
    return false;
  }

  works_.clear();
  JsonArray arr = doc["works"].as<JsonArray>();
  works_.reserve(arr.size());

  for (JsonObject obj : arr) {
    WorkEntry entry;
    entry.id = obj["id"] | 0;
    snprintf(entry.title, sizeof(entry.title), "%s", (const char*)(obj["title"] | ""));
    snprintf(entry.kana, sizeof(entry.kana), "%s", (const char*)(obj["kana"] | ""));
    snprintf(entry.ndc, sizeof(entry.ndc), "%s", (const char*)(obj["ndc"] | ""));
    works_.push_back(entry);
  }

  return true;
}
```

- [ ] **Step 3: downloadBook実装**

```cpp
bool AozoraActivity::downloadBook(int workId, const char* title, const char* author) {
  AozoraIndexManager::ensureDirectory();

  std::string filename = AozoraIndexManager::makeFilename(workId, title);
  char destPath[128];
  snprintf(destPath, sizeof(destPath), "%s/%s", AozoraIndexManager::AOZORA_DIR, filename.c_str());

  char url[256];
  snprintf(url, sizeof(url), "%s/api/convert?work_id=%d", API_BASE_URL, workId);

  auto result = HttpDownloader::downloadToFile(
      std::string(url), std::string(destPath),
      [this](size_t downloaded, size_t total) {
        downloadProgress_ = downloaded;
        downloadTotal_ = total;
        requestUpdate(true);
      });

  if (result != HttpDownloader::OK) {
    LOG_ERR("AOZORA", "Download failed: %d (http=%d)", result, HttpDownloader::lastHttpCode);
    // 不完全ファイル削除
    Storage.remove(destPath);
    return false;
  }

  // インデックスに追加
  return indexManager_.addEntry(workId, title, author, filename.c_str());
}
```

- [ ] **Step 4: コミット**

```bash
git add src/activities/settings/AozoraActivity.cpp
git commit -m "✨ API通信（fetchAuthors/fetchWorks/downloadBook）とJSONパースを実装"
```

---

### Task 8: AUTHOR_LIST・WORK_LIST・WORK_DETAIL の入力＆描画

**Files:**
- Modify: `src/activities/settings/AozoraActivity.cpp`

- [ ] **Step 1: AUTHOR_LISTの入力＆描画**

```cpp
void AozoraActivity::handleAuthorListInput() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    RenderLock lock(*this);
    popState();
    requestUpdate();
    return;
  }

  const int count = static_cast<int>(authors_.size());

  buttonNavigator_.onNextRelease([this, count] {
    if (selectedIndex_ < count - 1) {
      selectedIndex_++;
      requestUpdate();
    }
  });

  buttonNavigator_.onPreviousRelease([this] {
    if (selectedIndex_ > 0) {
      selectedIndex_--;
      requestUpdate();
    }
  });

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) && count > 0) {
    selectedAuthorId_ = authors_[selectedIndex_].id;
    snprintf(selectedWorkAuthor_, sizeof(selectedWorkAuthor_), "%s", authors_[selectedIndex_].name);

    {
      RenderLock lock(*this);
      pushState(State::LOADING);
    }
    requestUpdateAndWait();

    char param[32];
    snprintf(param, sizeof(param), "author_id=%d", selectedAuthorId_);
    bool success = fetchWorks(param);

    if (success) {
      RenderLock lock(*this);
      state_ = State::WORK_LIST;
    } else {
      RenderLock lock(*this);
      state_ = State::ERROR;
    }
    requestUpdate();
  }
}

void AozoraActivity::renderAuthorList(int pageWidth, int pageHeight) {
  const auto& m = GUI.getMetrics();
  const Rect contentRect = {m.contentSidePadding,
                            m.topPadding + m.headerHeight + m.verticalSpacing,
                            pageWidth - m.contentSidePadding * 2,
                            pageHeight - m.topPadding - m.headerHeight - m.verticalSpacing - m.buttonHintsHeight};

  if (authors_.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_NO_RESULTS));
    return;
  }

  GUI.drawList(renderer, contentRect, static_cast<int>(authors_.size()), selectedIndex_,
               [this](int index) -> std::string { return std::string(authors_[index].name); },
               nullptr, nullptr,
               [this](int index) -> std::string {
                 char buf[32];
                 snprintf(buf, sizeof(buf), "%d works", authors_[index].workCount);
                 return std::string(buf);
               },
               true, nullptr);
}
```

- [ ] **Step 2: WORK_LISTの入力＆描画**

```cpp
void AozoraActivity::handleWorkListInput() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    RenderLock lock(*this);
    popState();
    requestUpdate();
    return;
  }

  const int count = static_cast<int>(works_.size());

  buttonNavigator_.onNextRelease([this, count] {
    if (selectedIndex_ < count - 1) {
      selectedIndex_++;
      requestUpdate();
    }
  });

  buttonNavigator_.onPreviousRelease([this] {
    if (selectedIndex_ > 0) {
      selectedIndex_--;
      requestUpdate();
    }
  });

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) && count > 0) {
    selectedWorkId_ = works_[selectedIndex_].id;
    snprintf(selectedWorkTitle_, sizeof(selectedWorkTitle_), "%s", works_[selectedIndex_].title);

    {
      RenderLock lock(*this);
      pushState(State::WORK_DETAIL);
    }
    requestUpdate();
  }
}

void AozoraActivity::renderWorkList(int pageWidth, int pageHeight) {
  const auto& m = GUI.getMetrics();
  const Rect contentRect = {m.contentSidePadding,
                            m.topPadding + m.headerHeight + m.verticalSpacing,
                            pageWidth - m.contentSidePadding * 2,
                            pageHeight - m.topPadding - m.headerHeight - m.verticalSpacing - m.buttonHintsHeight};

  if (works_.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_NO_RESULTS));
    return;
  }

  GUI.drawList(renderer, contentRect, static_cast<int>(works_.size()), selectedIndex_,
               [this](int index) -> std::string { return std::string(works_[index].title); },
               nullptr, nullptr, nullptr, true,
               [this](int index) -> bool { return indexManager_.isDownloaded(works_[index].id); });
}
```

- [ ] **Step 3: WORK_DETAILの入力＆描画**

```cpp
void AozoraActivity::handleWorkDetailInput() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    RenderLock lock(*this);
    popState();
    requestUpdate();
    return;
  }

  bool downloaded = indexManager_.isDownloaded(selectedWorkId_);

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (downloaded) {
      // 削除確認 → 削除実行
      if (indexManager_.removeEntry(selectedWorkId_)) {
        LOG_DBG("AOZORA", "Deleted work %d", selectedWorkId_);
      }
      requestUpdate();
    } else {
      // ダウンロード開始
      {
        RenderLock lock(*this);
        state_ = State::DOWNLOADING;
        downloadProgress_ = 0;
        downloadTotal_ = 0;
      }
      requestUpdateAndWait();

      bool success = downloadBook(selectedWorkId_, selectedWorkTitle_, selectedWorkAuthor_);

      {
        RenderLock lock(*this);
        if (success) {
          state_ = State::WORK_DETAIL;  // 済みマーク表示に戻る
        } else {
          errorMessage_ = tr(STR_DOWNLOAD_FAILED);
          state_ = State::ERROR;
        }
      }
      requestUpdate();
    }
  }
}

void AozoraActivity::renderWorkDetail(int pageWidth, int pageHeight) {
  const auto& m = GUI.getMetrics();
  const int startY = m.topPadding + m.headerHeight + m.verticalSpacing * 2;

  // タイトル表示
  renderer.drawText(UI_12_FONT_ID, m.contentSidePadding, startY, selectedWorkTitle_, true);

  // 作家名表示
  renderer.drawText(UI_10_FONT_ID, m.contentSidePadding, startY + 40, selectedWorkAuthor_, true);

  // ダウンロード状態表示
  bool downloaded = indexManager_.isDownloaded(selectedWorkId_);
  const int buttonY = startY + 100;

  if (downloaded) {
    renderer.drawText(UI_10_FONT_ID, m.contentSidePadding, buttonY, tr(STR_ALREADY_DOWNLOADED), true);
    // Confirm = 削除
    GUI.drawButtonHints(renderer, pageWidth, pageHeight, tr(STR_BACK), tr(STR_DELETE_CONFIRM));
  } else {
    // Confirm = ダウンロード
    GUI.drawButtonHints(renderer, pageWidth, pageHeight, tr(STR_BACK), tr(STR_DOWNLOAD_CONFIRM));
  }
}

void AozoraActivity::renderDownloading(int pageWidth, int pageHeight) {
  const auto& m = GUI.getMetrics();
  const int centerY = pageHeight / 2;

  renderer.drawCenteredText(UI_10_FONT_ID, centerY - 20, tr(STR_DOWNLOADING_BOOK));

  if (downloadTotal_ > 0) {
    const Rect progressRect = {m.contentSidePadding, centerY + 10,
                                pageWidth - m.contentSidePadding * 2, 20};
    int progress = static_cast<int>((downloadProgress_ * 100) / downloadTotal_);
    GUI.drawProgressBar(renderer, progressRect, progress, 100);
  }
}
```

- [ ] **Step 4: ビルド確認**

```bash
pio run
```

Expected: リンクエラー（DOWNLOADED_LIST未実装）は許容。追加コードのコンパイル通過を確認。

- [ ] **Step 5: コミット**

```bash
git add src/activities/settings/AozoraActivity.cpp
git commit -m "✨ AUTHOR_LIST・WORK_LIST・WORK_DETAILの入力＆描画を実装"
```

---

### Task 9: DOWNLOADED_LIST（ダウンロード済み管理）

**Files:**
- Modify: `src/activities/settings/AozoraActivity.cpp`

- [ ] **Step 1: DOWNLOADED_LISTの入力＆描画**

```cpp
void AozoraActivity::handleDownloadedListInput() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    RenderLock lock(*this);
    popState();
    requestUpdate();
    return;
  }

  const auto& entries = indexManager_.entries();
  const int count = static_cast<int>(entries.size());

  buttonNavigator_.onNextRelease([this, count] {
    if (selectedIndex_ < count - 1) {
      selectedIndex_++;
      requestUpdate();
    }
  });

  buttonNavigator_.onPreviousRelease([this] {
    if (selectedIndex_ > 0) {
      selectedIndex_--;
      requestUpdate();
    }
  });

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) && count > 0) {
    selectedWorkId_ = entries[selectedIndex_].workId;
    snprintf(selectedWorkTitle_, sizeof(selectedWorkTitle_), "%s", entries[selectedIndex_].title);
    snprintf(selectedWorkAuthor_, sizeof(selectedWorkAuthor_), "%s", entries[selectedIndex_].author);

    {
      RenderLock lock(*this);
      pushState(State::WORK_DETAIL);
    }
    requestUpdate();
  }
}

void AozoraActivity::renderDownloadedList(int pageWidth, int pageHeight) {
  const auto& m = GUI.getMetrics();
  const Rect contentRect = {m.contentSidePadding,
                            m.topPadding + m.headerHeight + m.verticalSpacing,
                            pageWidth - m.contentSidePadding * 2,
                            pageHeight - m.topPadding - m.headerHeight - m.verticalSpacing - m.buttonHintsHeight};

  const auto& entries = indexManager_.entries();

  if (entries.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_NO_RESULTS));
    return;
  }

  GUI.drawList(renderer, contentRect, static_cast<int>(entries.size()), selectedIndex_,
               [&entries](int index) -> std::string { return std::string(entries[index].title); },
               nullptr, nullptr,
               [&entries](int index) -> std::string { return std::string(entries[index].author); },
               true, nullptr);
}
```

- [ ] **Step 2: フルビルド確認**

```bash
pio run
```

Expected: SUCCESS（全メソッド実装完了）

- [ ] **Step 3: コミット**

```bash
git add src/activities/settings/AozoraActivity.cpp
git commit -m "✨ DOWNLOADED_LIST（ダウンロード済み一覧）の入力＆描画を実装"
```

---

### Task 10: 全体ビルド検証・最終調整

- [ ] **Step 1: クリーンビルド**

```bash
pio run -t clean && pio run
```

Expected: SUCCESS, 0 errors, 0 warnings

- [ ] **Step 2: git diff確認**

```bash
git diff
git status
```

意図しない変更がないことを確認。

- [ ] **Step 3: 変更ファイル一覧の最終確認**

```
新規:
  src/activities/settings/AozoraActivity.h
  src/activities/settings/AozoraActivity.cpp
  src/AozoraIndexManager.h
  src/AozoraIndexManager.cpp

修正:
  src/SettingsList.h (SettingAction追加)
  src/activities/settings/SettingsActivity.cpp (case追加)
  lib/I18n/translations/japanese.yaml (翻訳キー追加)
  lib/I18n/translations/english.yaml (翻訳キー追加)
```

- [ ] **Step 4: 実機テスト指示**

以下を実機で確認:
1. Settings → 青空文庫 が表示されること
2. WiFi接続 → トップメニュー表示
3. 作家から探す → あ行 → 芥川龍之介 → 羅生門 → ダウンロード
4. `/Aozora/` にEPUBが保存されること
5. HomeActivityからダウンロードしたEPUBが開けること
6. ダウンロード済み一覧から削除できること
7. 手動でSDからファイル削除後、インデックスパージが動作すること
