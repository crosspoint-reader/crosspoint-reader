// Pocket Library for CrossPoint
// Copyright (C) 2026 Pocket Library contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. See LICENSE-GPL-3.0 at the repository root.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "activities/Activity.h"
#include "activities/util/KeyboardEntryActivity.h"

// Search across the card: the keyboard with results as you type, under a row
// of scope chips (All, eBooks, Wikipedia, More: every other collection and
// group). Opened from the Library, a collection, the Home tab bar or the
// power button; it starts in the scope it was opened from.
//
// Results: an exact title in several collections is one row ("4 sources"),
// which asks which one to open; then books whose title or author matches;
// then the collections' titles, taking turns (zim::searchMany). An empty field
// lists recent searches (tap to search again) and recent articles.
//
// This screen draws nothing of its own: it lives under the keyboard, and
// comes back to it (with the same text and scope) when an article it opened
// is closed.
class SearchActivity final : public Activity {
 public:
  // `collection`: the collection searched from (its scope is chosen), or -1.
  // `group`: a group searched from ("Medicine"), used when collection is -1.
  SearchActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int collection = -1, std::string group = "");

  void onEnter() override;

 private:
  struct Scope {
    std::string label;
    bool books = false;
    std::vector<size_t> collections;
  };
  struct Book {
    std::string fold;        // title, folded (the index's own fold)
    std::string authorFold;  // author, folded
    std::string title;
    std::string author;
    uint16_t ordinal = 0;
  };
  enum class PickKind : uint8_t { None, Article, Sources, Book, Place, More };
  struct Pick {
    PickKind kind = PickKind::None;
    size_t collection = 0;
    uint32_t entry = 0;
    std::vector<std::pair<size_t, uint32_t>> sources;  // PickKind::Sources
    uint16_t book = 0;                                 // PickKind::Book: ordinal
    size_t place = 0;                                  // PickKind::Place: ReadingHistory index
  };

  void buildScopes(int collection, const std::string& group);
  void openKeyboard();
  void fill(const std::string& text, int scope, std::vector<KeyboardEntryActivity::LiveRow>& rows, std::string& status);
  void compute(const std::string& text, int scope, size_t maxRows, size_t booksInAll,
               std::vector<KeyboardEntryActivity::LiveRow>& rows, std::vector<Pick>& picks, std::string& status);
  void openResults();
  void resume();
  void loadBooks();
  void act(const Pick& pick);
  void openArticle(size_t collection, uint32_t entry);
  void chooseSource(const std::vector<std::pair<size_t, uint32_t>>& sources);
  void openBook(uint16_t ordinal);

  std::vector<Scope> scopes_;
  size_t pinned_ = 0;
  int scope_ = 0;
  std::string query_;
  std::vector<Pick> picks_;    // parallel to the rows the keyboard shows
  std::vector<Pick> results_;  // parallel to the full-screen results
  bool inResults_ = false;     // an article opened from the results returns to them
  size_t resultsMax_ = 0;      // rows in the full list ("More results" adds a page)
  int resultsFocus_ = -1;      // row selected when the list reopens
  std::vector<Book> books_;
  bool booksLoaded_ = false;
  std::string booksStatus_;  // why there are no books, if there are none
};
