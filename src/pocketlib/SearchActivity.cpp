// Pocket Library for CrossPoint
// Copyright (C) 2026 Pocket Library contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. See LICENSE-GPL-3.0 at the repository root.

// Compiled only into Pocket Library builds; stock envs see an empty unit.
#ifdef POCKET_LIBRARY

#include "SearchActivity.h"

#include <LibraryBuilder.h>
#include <LibraryIndexFile.h>
#include <LibraryText.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>

#include "ArticleActivity.h"
#include "LibraryActivities.h"
#include "PocketLibrary.h"
#include "ReadingHistory.h"
#include "activities/ActivityResult.h"

namespace {
constexpr size_t kMaxRows = 12;        // more than fit above the keys
constexpr size_t kMaxBooks = 2000;     // cached titles; the index's own cap is 4096
constexpr size_t kBooksInAll = 3;      // book rows among everything else
constexpr size_t kRecentArticles = 4;  // under the recent searches, empty field
constexpr size_t kMaxResults = 80;     // the full-screen list after OK
constexpr size_t kBooksInResults = 10;
}  // namespace

SearchActivity::SearchActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int collection,
                               std::string group)
    : Activity("PocketSearch", renderer, mappedInput) {
  auto& lib = pocketlib::Library::instance();
  if (lib.collections().empty()) lib.load();
  buildScopes(collection, group);
}

// All, eBooks, and Wikipedia (when the card has it) get chips; then each group
// ("Medicine") and each collection, behind the last chip.
void SearchActivity::buildScopes(int collection, const std::string& group) {
  const auto& cols = pocketlib::Library::instance().collections();
  Scope all{"All", true, {}};
  for (size_t i = 0; i < cols.size(); i++) all.collections.push_back(i);
  scopes_.push_back(std::move(all));
  scopes_.push_back({"eBooks", true, {}});
  int wikipedia = -1;
  for (size_t i = 0; i < cols.size() && wikipedia < 0; i++) {
    if (cols[i].key.rfind("wikipedia", 0) == 0) wikipedia = static_cast<int>(i);
  }
  if (wikipedia >= 0) scopes_.push_back({cols[wikipedia].title, false, {static_cast<size_t>(wikipedia)}});
  pinned_ = scopes_.size();

  // Groups of several collections, as the Library grid shows them; "More"
  // is left out (its members are listed one by one, and the chip is "More").
  std::vector<std::string> groups;
  for (const auto& c : cols) {
    const std::string g = tileGroupFor(c);
    if (g == "More" || std::find(groups.begin(), groups.end(), g) != groups.end()) continue;
    groups.push_back(g);
  }
  for (const auto& g : groups) {
    Scope s{g, false, {}};
    for (size_t i = 0; i < cols.size(); i++) {
      if (tileGroupFor(cols[i]) == g) s.collections.push_back(i);
    }
    if (s.collections.size() > 1) scopes_.push_back(std::move(s));
  }
  for (size_t i = 0; i < cols.size(); i++) {
    if (static_cast<int>(i) != wikipedia) scopes_.push_back({cols[i].title, false, {i}});
  }

  // Start where the search was opened from.
  for (size_t k = 1; k < scopes_.size(); k++) {
    const Scope& s = scopes_[k];
    const bool fromCollection =
        collection >= 0 && s.collections.size() == 1 && s.collections[0] == static_cast<size_t>(collection) && !s.books;
    const bool fromGroup = collection < 0 && !group.empty() && s.label == group && s.collections.size() > 1;
    if (fromCollection || fromGroup) {
      scope_ = static_cast<int>(k);
      break;
    }
  }
}

void SearchActivity::onEnter() {
  Activity::onEnter();
  openKeyboard();
}

void SearchActivity::openKeyboard() {
  auto keyboard =
      makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, "Search", query_, 64, InputType::Text);
  if (!keyboard) {
    LOG_ERR("PLIB", "OOM: search keyboard");
    finish();
    return;
  }
  std::vector<std::string> labels;
  for (const auto& s : scopes_) labels.push_back(s.label);
  keyboard->setLiveScopes(std::move(labels), pinned_, scope_);
  // Runs on the main task after every edit or scope change.
  keyboard->setLiveSuggestions([this](const std::string& text, int scope,
                                      std::vector<KeyboardEntryActivity::LiveRow>& rows,
                                      std::string& status) { fill(text, scope, rows, status); });
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    if (result.isCancelled || !std::holds_alternative<KeyboardResult>(result.data)) {
      finish();
      return;
    }
    const auto& kb = std::get<KeyboardResult>(result.data);
    query_ = kb.text;
    inResults_ = false;
    if (kb.picked < 0 || kb.picked >= static_cast<int>(picks_.size())) {
      // OK: every result, full screen; an empty field keeps the keyboard.
      if (query_.empty())
        openKeyboard();
      else
        openResults();
      return;
    }
    const Pick pick = picks_[kb.picked];
    if (pick.kind == PickKind::None) {
      openKeyboard();
      return;
    }
    if (pick.kind != PickKind::Place) pocketlib::ReadingHistory::instance().recordSearch(query_);
    act(pick);
  });
}

void SearchActivity::fill(const std::string& text, int scope, std::vector<KeyboardEntryActivity::LiveRow>& rows,
                          std::string& status) {
  scope_ = scope;
  compute(text, scope, kMaxRows, kBooksInAll, rows, picks_, status);
}

void SearchActivity::compute(const std::string& text, int scope, size_t maxRows, size_t booksInAll,
                             std::vector<KeyboardEntryActivity::LiveRow>& rows, std::vector<Pick>& picks,
                             std::string& status) {
  picks.clear();
  auto& lib = pocketlib::Library::instance();
  const auto& cols = lib.collections();

  if (text.empty()) {
    for (const auto& q : pocketlib::ReadingHistory::instance().searches()) {
      rows.push_back({q, "searched", true});
      picks.push_back({});  // never picked: the row fills the field
    }
    const auto& places = pocketlib::ReadingHistory::instance().places();
    for (size_t i = 0; i < places.size() && i < kRecentArticles; i++) {
      std::string where = places[i].collection;
      for (const auto& c : cols)
        if (c.key == places[i].collection) where = c.title;
      rows.push_back({places[i].title, where, false});
      Pick p;
      p.kind = PickKind::Place;
      p.place = i;
      picks.push_back(std::move(p));
    }
    if (rows.empty()) status = "Type the start of a title";
    return;
  }

  const Scope& s = scopes_[std::min<size_t>(scope, scopes_.size() - 1)];
  const bool onlyBooks = s.books && s.collections.empty();

  // Books: every query word starts a word of the title or the author.
  std::vector<const Book*> exactBooks;
  std::vector<const Book*> otherBooks;
  if (s.books) {
    loadBooks();
    const std::string needle = library::fold(text);
    for (const Book& b : books_) {
      if (b.fold == needle) {
        exactBooks.push_back(&b);
      } else if (library::matchesQuery(b.fold, needle) || library::matchesQuery(b.authorFold, needle)) {
        otherBooks.push_back(&b);
      }
      if (exactBooks.size() + otherBooks.size() >= maxRows) break;
    }
  }
  auto addBook = [&](const Book& b) {
    rows.push_back({b.title, b.author.empty() ? std::string("eBook") : b.author, false});
    Pick p;
    p.kind = PickKind::Book;
    p.book = b.ordinal;
    picks.push_back(std::move(p));
  };

  std::vector<pocketlib::Library::Hit> hits;
  if (!s.collections.empty()) lib.search(s.collections, text, maxRows, hits);

  // An exact title found in several collections: one row for all of them.
  std::vector<std::pair<size_t, uint32_t>> exact;
  for (const auto& h : hits)
    if (h.exact) exact.emplace_back(h.collection, h.entry);
  const bool grouped = exact.size() > 1;
  if (grouped) {
    // "Wikipedia · Wiktionary · +1": where it is, not just how many.
    std::string where;
    for (size_t i = 0; i < exact.size() && i < 2; i++) {
      if (i) where += " \xC2\xB7 ";
      where += shortTitle(cols[exact[i].first]);
    }
    if (exact.size() > 2) where += " \xC2\xB7 +" + std::to_string(exact.size() - 2);
    rows.push_back({hits.front().title, where, false});
    Pick p;
    p.kind = PickKind::Sources;
    p.sources = exact;
    picks.push_back(std::move(p));
  }
  for (const Book* b : exactBooks) addBook(*b);
  for (size_t i = 0; i < otherBooks.size() && (onlyBooks || i < booksInAll); i++) addBook(*otherBooks[i]);
  for (auto& h : hits) {
    if (grouped && h.exact) continue;
    if (rows.size() >= maxRows) break;
    const bool one = s.collections.size() == 1;
    rows.push_back({std::move(h.title), one ? std::string() : shortTitle(cols[h.collection]), false});
    Pick p;
    p.kind = PickKind::Article;
    p.collection = h.collection;
    p.entry = h.entry;
    picks.push_back(std::move(p));
  }

  if (rows.empty()) {
    if (onlyBooks && books_.empty() && !booksStatus_.empty())
      status = booksStatus_;
    else
      status = "Nothing starts with \"" + text + "\"";
  }
}

// The titles and authors of every book CrossPoint's own library has indexed,
// read once per search screen (about 150 bytes a book).
void SearchActivity::loadBooks() {
  if (booksLoaded_) return;
  booksLoaded_ = true;
  library::LibraryIndexFile index;
  if (library::isLibraryIndexDirty() || !index.open(library::libraryIndexPath())) {
    booksStatus_ = "Open eBooks once to list your books";
    return;
  }
  const uint16_t count = index.bookCount();
  books_.reserve(std::min<size_t>(count, kMaxBooks));
  for (uint16_t i = 0; i < count && books_.size() < kMaxBooks; i++) {
    library::ClixRecord rec{};
    if (!index.readRecord(i, rec)) continue;
    Book b;
    b.fold.assign(rec.fold, rec.foldLen);
    if (!index.readTitle(rec, b.title) || b.title.empty()) index.readName(rec, b.title);
    index.readAuthor(rec, b.author);
    b.authorFold = library::fold(b.author);
    b.ordinal = i;
    books_.push_back(std::move(b));
  }
  if (books_.empty()) booksStatus_ = "No books on the card";
  LOG_INF("PLIB", "search: %u books", static_cast<unsigned>(books_.size()));
}

// After OK: up to kMaxResults results as a full-screen list that scrolls
// (swipe or the side buttons). Back returns to the keyboard; Back from an
// article opened here returns to this list.
void SearchActivity::openResults() {
  std::vector<KeyboardEntryActivity::LiveRow> rows;
  std::string status;
  compute(query_, scope_, kMaxResults, kBooksInResults, rows, results_, status);
  std::vector<std::string> labels;
  std::vector<std::string> subtitles;
  for (auto& r : rows) {
    labels.push_back(std::move(r.text));
    subtitles.push_back(std::move(r.tag));
  }
  if (labels.empty()) {
    labels.push_back(status.empty() ? "No results" : status);
    subtitles.emplace_back();
    results_.assign(1, Pick{});
  }
  const std::string where = scopes_[std::min<size_t>(scope_, scopes_.size() - 1)].label;
  auto list = makeUniqueNoThrow<ChoiceListActivity>(renderer, mappedInput,
                                                    "\xE2\x80\x9C" + query_ + "\xE2\x80\x9D \xC2\xB7 " + where,
                                                    std::move(labels), std::move(subtitles));
  if (!list) {
    openKeyboard();
    return;
  }
  inResults_ = true;
  startActivityForResult(std::move(list), [this](const ActivityResult& result) {
    if (result.isCancelled || !std::holds_alternative<MenuResult>(result.data)) {
      inResults_ = false;
      openKeyboard();
      return;
    }
    const int row = std::get<MenuResult>(result.data).action;
    if (row < 0 || row >= static_cast<int>(results_.size()) || results_[row].kind == PickKind::None) {
      openResults();
      return;
    }
    pocketlib::ReadingHistory::instance().recordSearch(query_);
    act(results_[row]);
  });
}

// Where to come back to after an article or a list closes.
void SearchActivity::resume() {
  if (inResults_)
    openResults();
  else
    openKeyboard();
}

void SearchActivity::act(const Pick& pick) {
  switch (pick.kind) {
    case PickKind::None:
      resume();
      return;
    case PickKind::Article:
      openArticle(pick.collection, pick.entry);
      return;
    case PickKind::Sources:
      chooseSource(pick.sources);
      return;
    case PickKind::Book:
      openBook(pick.book);
      return;
    case PickKind::Place: {
      const auto& places = pocketlib::ReadingHistory::instance().places();
      size_t collection = 0;
      uint32_t entry = 0;
      if (pick.place < places.size() && locatePlace(places[pick.place], collection, entry)) {
        openArticle(collection, entry);
        return;
      }
      resume();
      return;
    }
  }
}

void SearchActivity::openArticle(size_t collection, uint32_t entry) {
  auto article = makeUniqueNoThrow<ArticleActivity>(renderer, mappedInput, collection, entry);
  if (!article) {
    LOG_ERR("PLIB", "OOM: article activity");
    resume();
    return;
  }
  // Back from the article returns to the results, as they were.
  startActivityForResult(std::move(article), [this](const ActivityResult&) { resume(); });
}

void SearchActivity::chooseSource(const std::vector<std::pair<size_t, uint32_t>>& sources) {
  const auto& cols = pocketlib::Library::instance().collections();
  std::vector<std::string> labels;
  for (const auto& s : sources) labels.push_back(cols[s.first].title);
  auto list = makeUniqueNoThrow<ChoiceListActivity>(renderer, mappedInput, query_, std::move(labels));
  if (!list) {
    resume();
    return;
  }
  startActivityForResult(std::move(list), [this, sources](const ActivityResult& result) {
    if (result.isCancelled || !std::holds_alternative<MenuResult>(result.data)) {
      resume();
      return;
    }
    const int row = std::get<MenuResult>(result.data).action;
    if (row < 0 || row >= static_cast<int>(sources.size())) {
      resume();
      return;
    }
    openArticle(sources[row].first, sources[row].second);
  });
}

void SearchActivity::openBook(uint16_t ordinal) {
  std::string path;
  {
    library::LibraryIndexFile index;
    library::ClixRecord rec{};
    if (!index.open(library::libraryIndexPath()) || !index.readRecord(ordinal, rec) || !index.readPath(rec, path)) {
      LOG_ERR("PLIB", "search: cannot find book %u", static_cast<unsigned>(ordinal));
      resume();
      return;
    }
  }  // index file closed before the reader opens the book
  onSelectBook(path);
}

#endif  // POCKET_LIBRARY
