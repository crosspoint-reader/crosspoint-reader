# 青空文庫 お気に入り作家管理機能 実装計画

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 青空文庫の作家をお気に入り登録し、TOP_MENUから素早くアクセスできる機能を実装する

**Architecture:** `FavoriteAuthorsManager` クラスがSDカード上のJSONファイルでお気に入りデータを永続化。`AozoraActivity` に `FAVORITE_AUTHORS` と `AUTHOR_ACTION` の2状態を追加し、AUTHOR_LISTのConfirmをアクションメニュー経由に変更する。

**Tech Stack:** C++20 / Arduino-ESP32 / ArduinoJson / PlatformIO

---

## ファイル構成

| ファイル | 操作 | 責務 |
|---------|------|------|
| `src/FavoriteAuthorsManager.h` | 新規 | お気に入り作家の構造体・管理クラス定義 |
| `src/FavoriteAuthorsManager.cpp` | 新規 | JSON永続化・ソート・CRUD実装 |
| `src/activities/settings/AozoraActivity.h` | 変更 | 新状態・メンバー追加 |
| `src/activities/settings/AozoraActivity.cpp` | 変更 | 状態遷移・描画・入力処理の追加 |
| `lib/I18n/translations/japanese.yaml` | 変更 | 日本語翻訳キー追加 |
| `lib/I18n/translations/english.yaml` | 変更 | 英語翻訳キー追加 |

---

### Task 1: i18n 翻訳キーの追加

**Files:**
- Modify: `lib/I18n/translations/japanese.yaml:389` (STR_DOWNLOADED_BOOKS の後に追加)
- Modify: `lib/I18n/translations/english.yaml:396` (同上)

- [ ] **Step 1: japanese.yaml にキーを追加**

`lib/I18n/translations/japanese.yaml` の `STR_GENRE_FAIRY_TALE` 行の直前（`STR_NO_RESULTS` の後あたりの青空文庫セクション末尾）に以下を追加:

```yaml
STR_FAVORITE_AUTHORS: "お気に入り作家"
STR_ADD_TO_FAVORITES: "お気に入りに追加"
STR_REMOVE_FROM_FAVORITES: "お気に入りから削除"
STR_VIEW_WORKS: "作品を見る"
STR_NO_FAVORITE_AUTHORS: "お気に入り作家がありません"
```

挿入位置: `STR_NO_RESULTS` 行（397行目）の直後。

- [ ] **Step 2: english.yaml にキーを追加**

`lib/I18n/translations/english.yaml` の対応する位置に以下を追加:

```yaml
STR_FAVORITE_AUTHORS: "Favorite Authors"
STR_ADD_TO_FAVORITES: "Add to Favorites"
STR_REMOVE_FROM_FAVORITES: "Remove from Favorites"
STR_VIEW_WORKS: "View Works"
STR_NO_FAVORITE_AUTHORS: "No Favorite Authors"
```

- [ ] **Step 3: i18nヘッダーを再生成**

```bash
cd /Users/zrn_ns/projects/crosspoint-reader
python3 scripts/gen_i18n.py lib/I18n/translations lib/I18n/
```

Expected: `I18nKeys.h`, `I18nStrings.h`, `I18nStrings.cpp` が再生成され、新キーが含まれる。

- [ ] **Step 4: 生成結果を確認**

```bash
grep -n "STR_FAVORITE_AUTHORS\|STR_VIEW_WORKS\|STR_NO_FAVORITE_AUTHORS" lib/I18n/I18nKeys.h
```

Expected: 5つのキーが `StrId` enum に含まれている。

- [ ] **Step 5: コミット**

```bash
git add lib/I18n/translations/japanese.yaml lib/I18n/translations/english.yaml
git commit -m "✨ お気に入り作家機能のi18n翻訳キーを追加（Issue #34）"
```

---

### Task 2: FavoriteAuthorsManager の実装

**Files:**
- Create: `src/FavoriteAuthorsManager.h`
- Create: `src/FavoriteAuthorsManager.cpp`

- [ ] **Step 1: ヘッダーファイルを作成**

`src/FavoriteAuthorsManager.h` を作成:

```cpp
#pragma once

#include <vector>

struct FavoriteAuthor {
  int authorId;
  char name[48];
  char kana[48];
};

class FavoriteAuthorsManager {
 public:
  static constexpr const char* FAVORITES_PATH = "/Aozora/.favorite_authors.json";

  bool load();
  bool save() const;
  void addAuthor(int id, const char* name, const char* kana);
  void removeAuthor(int id);
  bool isFavorited(int id) const;
  const std::vector<FavoriteAuthor>& entries() const { return entries_; }

 private:
  std::vector<FavoriteAuthor> entries_;
  void sortEntries();
};
```

- [ ] **Step 2: 実装ファイルを作成**

`src/FavoriteAuthorsManager.cpp` を作成:

```cpp
#include "FavoriteAuthorsManager.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

bool FavoriteAuthorsManager::load() {
  entries_.clear();

  FsFile file;
  if (!Storage.openFileForRead("FAVAUTH", FAVORITES_PATH, file)) {
    LOG_DBG("FAVAUTH", "No favorites file, starting fresh");
    return true;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, file);
  file.close();

  if (err) {
    LOG_ERR("FAVAUTH", "Favorites parse error: %s", err.c_str());
    return true;
  }

  JsonArray arr = doc.as<JsonArray>();
  entries_.reserve(arr.size());

  for (JsonObject obj : arr) {
    FavoriteAuthor entry;
    entry.authorId = obj["author_id"] | 0;
    snprintf(entry.name, sizeof(entry.name), "%s", (const char*)(obj["name"] | ""));
    snprintf(entry.kana, sizeof(entry.kana), "%s", (const char*)(obj["kana"] | ""));
    entries_.push_back(entry);
  }

  sortEntries();
  LOG_DBG("FAVAUTH", "Loaded %zu favorites", entries_.size());
  return true;
}

bool FavoriteAuthorsManager::save() const {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();

  for (const auto& e : entries_) {
    JsonObject obj = arr.add<JsonObject>();
    obj["author_id"] = e.authorId;
    obj["name"] = e.name;
    obj["kana"] = e.kana;
  }

  FsFile file;
  if (!Storage.openFileForWrite("FAVAUTH", FAVORITES_PATH, file)) {
    LOG_ERR("FAVAUTH", "Failed to open favorites for write");
    return false;
  }

  serializeJson(doc, file);
  file.close();
  return true;
}

void FavoriteAuthorsManager::addAuthor(int id, const char* name, const char* kana) {
  if (isFavorited(id)) return;

  FavoriteAuthor entry;
  entry.authorId = id;
  snprintf(entry.name, sizeof(entry.name), "%s", name);
  snprintf(entry.kana, sizeof(entry.kana), "%s", kana);
  entries_.push_back(entry);
  sortEntries();
  save();
}

void FavoriteAuthorsManager::removeAuthor(int id) {
  for (auto it = entries_.begin(); it != entries_.end(); ++it) {
    if (it->authorId == id) {
      entries_.erase(it);
      save();
      return;
    }
  }
}

bool FavoriteAuthorsManager::isFavorited(int id) const {
  for (const auto& e : entries_) {
    if (e.authorId == id) return true;
  }
  return false;
}

void FavoriteAuthorsManager::sortEntries() {
  std::sort(entries_.begin(), entries_.end(), [](const FavoriteAuthor& a, const FavoriteAuthor& b) {
    return strcmp(a.kana, b.kana) < 0;
  });
}
```

- [ ] **Step 3: ビルド確認**

```bash
cd /Users/zrn_ns/projects/crosspoint-reader && pio run 2>&1 | tail -5
```

Expected: BUILD SUCCESS（FavoriteAuthorsManager は AozoraActivity からまだ参照されていないが、コンパイル単位として問題ないことを確認）

- [ ] **Step 4: コミット**

```bash
git add src/FavoriteAuthorsManager.h src/FavoriteAuthorsManager.cpp
git commit -m "✨ FavoriteAuthorsManager クラスを追加（Issue #34）"
```

---

### Task 3: AozoraActivity に新状態と FavoriteAuthorsManager を追加

**Files:**
- Modify: `src/activities/settings/AozoraActivity.h:24-37` (State enum)
- Modify: `src/activities/settings/AozoraActivity.h:94` (メンバー追加)

- [ ] **Step 1: State enum に FAVORITE_AUTHORS と AUTHOR_ACTION を追加**

`src/activities/settings/AozoraActivity.h` の `State` enum を変更。`DOWNLOADED_LIST` の後に2つの状態を追加:

```cpp
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
    FAVORITE_AUTHORS,
    AUTHOR_ACTION,
    LOADING,
    ERROR,
  };
```

- [ ] **Step 2: FavoriteAuthorsManager メンバーを追加**

`src/activities/settings/AozoraActivity.h` に以下を追加:

1. ヘッダーインクルード（ファイル先頭のインクルード群に追加）:
```cpp
#include "FavoriteAuthorsManager.h"
```

2. private メンバー（`AozoraIndexManager indexManager_;` の後に追加）:
```cpp
  FavoriteAuthorsManager favoritesManager_;

  // AUTHOR_ACTION state
  int actionMenuIndex_ = 0;
  State actionReturnState_ = AUTHOR_LIST;  // AUTHOR_ACTION完了後の戻り先
```

- [ ] **Step 3: ビルド確認**

```bash
cd /Users/zrn_ns/projects/crosspoint-reader && pio run 2>&1 | tail -5
```

Expected: BUILD SUCCESS

- [ ] **Step 4: コミット**

```bash
git add src/activities/settings/AozoraActivity.h
git commit -m "✨ AozoraActivity に FAVORITE_AUTHORS/AUTHOR_ACTION 状態を追加（Issue #34）"
```

---

### Task 4: お気に入り読み込みと TOP_MENU の変更

**Files:**
- Modify: `src/activities/settings/AozoraActivity.cpp:63` (TOP_MENU_COUNT)
- Modify: `src/activities/settings/AozoraActivity.cpp:90-112` (onWifiSelectionComplete)
- Modify: `src/activities/settings/AozoraActivity.cpp:349-420` (TOP_MENU の loop)
- Modify: `src/activities/settings/AozoraActivity.cpp:825-849` (TOP_MENU の render)

- [ ] **Step 1: TOP_MENU_COUNT を 5 → 6 に変更**

`src/activities/settings/AozoraActivity.cpp:63`:

```cpp
static constexpr int TOP_MENU_COUNT = 6;
```

- [ ] **Step 2: onWifiSelectionComplete で favoritesManager_ をロード**

`src/activities/settings/AozoraActivity.cpp` の `onWifiSelectionComplete` 内、`indexManager_.loadAndPurge();` の直後に追加:

```cpp
  favoritesManager_.load();
```

- [ ] **Step 3: TOP_MENU の loop() — インデックスを1つずつシフト**

`src/activities/settings/AozoraActivity.cpp` の TOP_MENU 内の `switch (selectedIndex_)` を以下に変更:

```cpp
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      switch (selectedIndex_) {
        case 0:  // お気に入り作家
        {
          RenderLock lock(*this);
          pushState(FAVORITE_AUTHORS);
        }
          requestUpdate();
          break;
        case 1:  // 作家から探す
          searchMode_ = SEARCH_AUTHOR;
          {
            RenderLock lock(*this);
            pushState(KANA_SELECT);
          }
          requestUpdate();
          break;
        case 2:  // 作品名から探す
          searchMode_ = SEARCH_TITLE;
          {
            RenderLock lock(*this);
            pushState(KANA_SELECT);
          }
          requestUpdate();
          break;
        case 3:  // ジャンルから探す
        {
          RenderLock lock(*this);
          pushState(GENRE_SELECT);
        }
          requestUpdate();
          break;
        case 4:  // 新着作品
        {
          {
            RenderLock lock(*this);
            pushState(LOADING);
          }
          requestUpdateAndWait();

          if (fetchWorks("sort=newest&limit=50")) {
            RenderLock lock(*this);
            state_ = WORK_LIST;
            selectedIndex_ = 0;
          } else {
            RenderLock lock(*this);
            state_ = ERROR;
          }
          requestUpdate();
        } break;
        case 5:  // ダウンロード済み
        {
          RenderLock lock(*this);
          pushState(DOWNLOADED_LIST);
        }
          requestUpdate();
          break;
      }
    }
```

- [ ] **Step 4: TOP_MENU の render() — メニュー項目を更新**

`src/activities/settings/AozoraActivity.cpp` の TOP_MENU レンダリング内の `drawList` ラムダを以下に変更:

```cpp
        [](int index) -> std::string {
          switch (index) {
            case 0:
              return tr(STR_FAVORITE_AUTHORS);
            case 1:
              return tr(STR_SEARCH_BY_AUTHOR);
            case 2:
              return tr(STR_SEARCH_BY_TITLE);
            case 3:
              return tr(STR_SEARCH_BY_GENRE);
            case 4:
              return tr(STR_NEWEST_WORKS);
            case 5:
              return tr(STR_DOWNLOADED_BOOKS);
            default:
              return "";
          }
        },
```

- [ ] **Step 5: ビルド確認**

```bash
cd /Users/zrn_ns/projects/crosspoint-reader && pio run 2>&1 | tail -5
```

Expected: BUILD SUCCESS

- [ ] **Step 6: コミット**

```bash
git add src/activities/settings/AozoraActivity.cpp
git commit -m "👍 TOP_MENUにお気に入り作家メニューを追加（Issue #34）"
```

---

### Task 5: FAVORITE_AUTHORS 状態の入力処理と描画

**Files:**
- Modify: `src/activities/settings/AozoraActivity.cpp` (loop() と render() に FAVORITE_AUTHORS を追加)

- [ ] **Step 1: FAVORITE_AUTHORS の loop() を追加**

`src/activities/settings/AozoraActivity.cpp` の `loop()` 内、`} else if (state_ == DOWNLOADED_LIST) {` の直前に以下を挿入:

```cpp
  } else if (state_ == FAVORITE_AUTHORS) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      {
        RenderLock lock(*this);
        popState();
      }
      requestUpdate();
      return;
    }

    const auto& favEntries = favoritesManager_.entries();

    if (!favEntries.empty()) {
      buttonNavigator_.onNextRelease([this, &favEntries] {
        if (selectedIndex_ < static_cast<int>(favEntries.size()) - 1) {
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
        const auto& fav = favEntries[selectedIndex_];
        selectedAuthorId_ = fav.authorId;
        snprintf(selectedAuthorName_, sizeof(selectedAuthorName_), "%s", fav.name);
        actionReturnState_ = FAVORITE_AUTHORS;
        {
          RenderLock lock(*this);
          pushState(AUTHOR_ACTION);
          actionMenuIndex_ = 0;
        }
        requestUpdate();
      }
    }

  } else if (state_ == AUTHOR_ACTION) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      {
        RenderLock lock(*this);
        popState();
      }
      requestUpdate();
      return;
    }

    buttonNavigator_.onNextRelease([this] {
      if (actionMenuIndex_ < 1) {
        actionMenuIndex_++;
        requestUpdate();
      }
    });

    buttonNavigator_.onPreviousRelease([this] {
      if (actionMenuIndex_ > 0) {
        actionMenuIndex_--;
        requestUpdate();
      }
    });

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (actionMenuIndex_ == 0) {
        // 作品を見る
        {
          RenderLock lock(*this);
          // AUTHOR_ACTION をスタックから除去して LOADING に置き換え
          state_ = LOADING;
        }
        requestUpdateAndWait();

        char query[64];
        snprintf(query, sizeof(query), "author_id=%d", selectedAuthorId_);
        if (fetchWorks(query)) {
          RenderLock lock(*this);
          state_ = WORK_LIST;
          selectedIndex_ = 0;
        } else {
          RenderLock lock(*this);
          state_ = ERROR;
        }
        requestUpdate();
      } else {
        // お気に入り追加/削除
        bool wasFavorited = favoritesManager_.isFavorited(selectedAuthorId_);
        if (wasFavorited) {
          favoritesManager_.removeAuthor(selectedAuthorId_);
        } else {
          // AUTHOR_LIST から来た場合、kana 情報が必要
          // authors_ ベクターから検索
          const char* kana = "";
          for (const auto& a : authors_) {
            if (a.id == selectedAuthorId_) {
              kana = a.kana;
              break;
            }
          }
          favoritesManager_.addAuthor(selectedAuthorId_, selectedAuthorName_, kana);
        }
        {
          RenderLock lock(*this);
          popState();
        }
        requestUpdate();
      }
    }

```

- [ ] **Step 2: FAVORITE_AUTHORS の render() を追加**

`src/activities/settings/AozoraActivity.cpp` の `render()` 内、`} else if (state_ == DOWNLOADED_LIST) {` の直前に以下を挿入:

```cpp
  } else if (state_ == FAVORITE_AUTHORS) {
    const auto& favEntries = favoritesManager_.entries();

    if (favEntries.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_NO_FAVORITE_AUTHORS));
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    } else {
      GUI.drawList(
          renderer,
          Rect{0, contentTop, pageWidth,
               pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing},
          static_cast<int>(favEntries.size()), selectedIndex_,
          [&favEntries](int index) -> std::string { return favEntries[index].name; }, nullptr, nullptr,
          nullptr, false, nullptr);

      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }

  } else if (state_ == AUTHOR_ACTION) {
    // ヘッダーに作家名を表示
    renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding, contentTop, selectedAuthorName_);
    const int listTop = contentTop + renderer.getLineHeight(UI_12_FONT_ID) + metrics.verticalSpacing;

    bool isFav = favoritesManager_.isFavorited(selectedAuthorId_);
    GUI.drawList(
        renderer,
        Rect{0, listTop, pageWidth, pageHeight - listTop - metrics.buttonHintsHeight - metrics.verticalSpacing},
        2, actionMenuIndex_,
        [isFav](int index) -> std::string {
          if (index == 0) return tr(STR_VIEW_WORKS);
          return isFav ? tr(STR_REMOVE_FROM_FAVORITES) : tr(STR_ADD_TO_FAVORITES);
        },
        nullptr, nullptr, nullptr, false, nullptr);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

```

- [ ] **Step 3: ビルド確認**

```bash
cd /Users/zrn_ns/projects/crosspoint-reader && pio run 2>&1 | tail -5
```

Expected: BUILD SUCCESS

- [ ] **Step 4: コミット**

```bash
git add src/activities/settings/AozoraActivity.cpp
git commit -m "✨ FAVORITE_AUTHORS/AUTHOR_ACTION 状態の入力処理と描画を実装（Issue #34）"
```

---

### Task 6: AUTHOR_LIST の Confirm をアクションメニュー経由に変更 & ★表示

**Files:**
- Modify: `src/activities/settings/AozoraActivity.cpp:608-632` (AUTHOR_LIST の Confirm 処理)
- Modify: `src/activities/settings/AozoraActivity.cpp:885-906` (AUTHOR_LIST の render)

- [ ] **Step 1: AUTHOR_LIST の Confirm をアクションメニュー遷移に変更**

`src/activities/settings/AozoraActivity.cpp` の AUTHOR_LIST 内、`if (mappedInput.wasPressed(MappedInputManager::Button::Confirm))` ブロックを以下に変更:

```cpp
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (!authors_.empty()) {
        const auto& author = authors_[selectedIndex_];
        selectedAuthorId_ = author.id;
        snprintf(selectedAuthorName_, sizeof(selectedAuthorName_), "%s", author.name);
        actionReturnState_ = AUTHOR_LIST;
        {
          RenderLock lock(*this);
          pushState(AUTHOR_ACTION);
          actionMenuIndex_ = 0;
        }
        requestUpdate();
      }
    }
```

- [ ] **Step 2: AUTHOR_LIST の render に★表示を追加**

`src/activities/settings/AozoraActivity.cpp` の AUTHOR_LIST レンダリング内の `drawList` ラムダを変更。作家名の先頭に★を付与:

```cpp
          [this](int index) -> std::string {
            if (favoritesManager_.isFavorited(authors_[index].id)) {
              char buf[56];
              snprintf(buf, sizeof(buf), "★ %s", authors_[index].name);
              return buf;
            }
            return authors_[index].name;
          },
```

- [ ] **Step 3: ビルド確認**

```bash
cd /Users/zrn_ns/projects/crosspoint-reader && pio run 2>&1 | tail -5
```

Expected: BUILD SUCCESS

- [ ] **Step 4: コミット**

```bash
git add src/activities/settings/AozoraActivity.cpp
git commit -m "👍 AUTHOR_LISTをアクションメニュー経由に変更＆★表示を追加（Issue #34）"
```

---

### Task 7: 最終ビルド確認とコード品質チェック

**Files:**
- 全体

- [ ] **Step 1: クリーンビルド**

```bash
cd /Users/zrn_ns/projects/crosspoint-reader && pio run -t clean && pio run 2>&1 | tail -10
```

Expected: BUILD SUCCESS, 0 errors, 0 warnings

- [ ] **Step 2: clang-format**

```bash
cd /Users/zrn_ns/projects/crosspoint-reader && find src -name "FavoriteAuthorsManager.*" | xargs clang-format -i
```

- [ ] **Step 3: フォーマット差分があればコミット**

```bash
git diff --stat
```

差分がある場合:
```bash
git add src/FavoriteAuthorsManager.h src/FavoriteAuthorsManager.cpp
git commit -m "🎨 FavoriteAuthorsManager のフォーマットを修正"
```

- [ ] **Step 4: git diff で意図しない変更がないことを確認**

```bash
git diff HEAD~6..HEAD --stat
```

Expected: 変更ファイルが計画通りの6ファイルのみ（japanese.yaml, english.yaml, FavoriteAuthorsManager.h, FavoriteAuthorsManager.cpp, AozoraActivity.h, AozoraActivity.cpp）
