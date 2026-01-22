#pragma once
#include <string>
#include <vector>

class RecentBooksStore {
  // Static instance
  static RecentBooksStore instance;

  std::vector<std::string> recentBooks;

 public:
  ~RecentBooksStore() = default;

  // Get singleton instance
  static RecentBooksStore& getInstance() { return instance; }

  // Add a book path to the recent list (moves to front if already exists)
  void addBook(const std::string& path);

  // Get the list of recent book paths (most recent first)
  const std::vector<std::string>& getBooks() const { return recentBooks; }

  // Get the count of recent books
  int getCount() const { return static_cast<int>(recentBooks.size()); }

  bool saveToFile() const;

  bool loadFromFile();
};

// Helper macro to access recent books store
#define RECENT_BOOKS RecentBooksStore::getInstance()
