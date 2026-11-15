#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Books the reader marked as favorites, from the Library or Browse Files row
// menu. Persisted to /.crosspoint/favorites.json so the list survives reboots
// and firmware updates, and keyed by the book's full path like RecentBooksStore.
//
// Title and author are display copies. Browse Files only knows the file name,
// so an entry marked there starts with the file name as its title and
// `resolved == false`; the Library replaces both with the index's own title and
// author the next time it shows the Favorites shelf, then marks it resolved.
struct FavoriteBook {
  std::string path;
  std::string title;
  std::string author;
  bool resolved = false;
  // In memory only: the Library already looked this entry up in the index
  // this boot and did not find it, so it is not scanned for again until the
  // next boot, the next index rebuild, or a change of its path.
  bool lookupTried = false;
  // In memory only: this entry's exact size in favorites.json.
  size_t jsonBytes = 0;
};

class FavoritesStore : public PersistableStore<FavoritesStore> {
 public:
  // Bounded so the Library's per-favorite lookups and the JSON document stay
  // small on every board. Adding past the cap is refused, not evicting.
  static constexpr int MAX_FAVORITES = 200;
  // SDCardManager::readFile() returns at most this many bytes.
  static constexpr size_t READ_LIMIT_BYTES = 50000;
  // Largest favorites.json a save may write, so every saved file loads.
  static constexpr size_t MAX_FILE_BYTES = 49000;
  // Adds, and adopting longer resolved titles, stop here. The 9 KB above it is
  // kept for path repoints (a book moved to /Read, a folder moved), which
  // follow a move that already happened and so cannot be refused.
  static constexpr size_t ADD_FILE_BYTES = 40000;
  // Per-field bounds, so no single entry needs a large allocation. Longer
  // titles and authors are cut on a UTF-8 boundary (read from a file without
  // copying the full value first); an entry whose path is longer is not
  // stored, and one that a move would lengthen past it is removed (logged).
  static constexpr size_t MAX_PATH_BYTES = 1024;
  static constexpr size_t MAX_TITLE_BYTES = 256;
  static constexpr size_t MAX_AUTHOR_BYTES = 128;

 private:
  // One small allocation per book plus a pointer array (4 B per entry, at most
  // 800 B at the cap), so a full list never needs one large contiguous block.
  std::vector<std::unique_ptr<FavoriteBook>> favorites;
  // Memory holds a change the card does not have yet (a save failed after a
  // move or delete that already happened on disk). flushPending() retries.
  bool unsaved = false;
  // The file on the card could not be read completely this boot (out of
  // memory, or unreadable and could not be set aside); saving would replace it
  // with the partial list in RAM, so every save is refused until reboot.
  bool locked = false;
  // fromJson ran out of memory partway (read by readOnce()).
  bool loadOom = false;
  // load() found no file: first use, or a card unreadable at boot. Cleared by
  // the first successful save; refreshFromCard() reloads if the file shows up.
  bool absentAtLoad = false;

  enum class SaveResult { Saved, TooBig, Failed };
  // Saves unless locked or the list is over MAX_FILE_BYTES; tracks `unsaved`.
  SaveResult persist();

  // One read of the file. Transient = out of memory or a read shorter than
  // the file (worth retrying, never grounds to discard the file); Corrupt = read
  // in full and not a favorites list, or too big for readFile() to return.
  enum class ReadResult { Ok, Missing, Transient, Corrupt };
  ReadResult readOnce();
  // Position of the entry for path, or -1.
  int find(const std::string& path) const;
  // Exact size of the whole list as toJson() would write it.
  size_t memBytes() const;
  // Views, so measuring a candidate (canAdd, setResolved) allocates nothing.
  static size_t entryBytes(std::string_view path, std::string_view title, std::string_view author, bool resolved);
  // Sets the display fields (clamped) and the entry's cached size.
  static void setFields(FavoriteBook& book, const std::string& title, const std::string& author, bool resolved);
  // Null for an empty or oversized path, or when memory runs out.
  static std::unique_ptr<FavoriteBook> makeBook(const std::string& path, const std::string& title,
                                                const std::string& author, bool resolved);
  // Without exceptions a failed std::string or vector allocation aborts, so
  // these check the heap first (the HalStorage::readFile pattern). Another task
  // allocating between a check and its use is not covered.
  // Room for MAX_FAVORITES pointers, taken once so the list never regrows.
  bool reserveList();
  // Enough heap to build one book with a path of pathLen bytes.
  static bool roomForBook(size_t pathLen);

  FavoritesStore() = default;
  ~FavoritesStore() = default;

  friend class PersistableStore<FavoritesStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/favorites.json"; }
  // Boot-time load. Use this, not loadFromFile(). Up to three reads; an
  // existing file that is read in full and still does not parse is renamed to
  // favorites.json.bad (never over an existing .bad) and the store starts
  // empty; one that cannot be read completely (memory, short reads) locks the
  // store against saves for this boot and is left alone.
  void load();
  // Reloads when the file was missing at load but exists now (see
  // absentAtLoad): the card was unreadable at boot. Every mutation calls it,
  // and the Library and the row menus call it before reading.
  void refreshFromCard();
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  bool isFavorite(const std::string& path) const { return find(path) >= 0; }

  // Adds the book (title/author as given, `resolved` as given) or removes it if
  // already present, then persists. Returns true when the book is a favorite
  // afterwards. An add that canAdd() refuses returns false. A save that fails
  // on the card rolls the change back; a removal that leaves the list still
  // over MAX_FILE_BYTES is kept (and saved once the list fits).
  bool toggle(const std::string& path, const std::string& title, const std::string& author, bool resolved);

  // Saves again if an earlier save failed. Cheap no-op otherwise.
  void flushPending();

  // Removes the entry for path, or every entry under it when path is a folder
  // ("/a" or "/a/" removes "/a/x.epub" but not "/ab.epub"), and persists.
  // Returns true if anything was removed.
  bool removeByPath(const std::string& path);

  // After a delete of a book or folder: removes its favorites (removeByPath)
  // when the path is really gone from the card and its parent folder is
  // still visible, whatever the delete reported. When the path is still there
  // (a folder delete that stopped partway), removes the favorites under it
  // whose books are gone. Every delete path calls it.
  void forgetIfGone(const char* path);

  // Repoints entries after a rename or move, then persists: the entry whose
  // path is oldPath, and every entry under oldPath + "/" when a folder moved.
  // Entries already at the destination that are not part of the move are
  // dropped (an overwriting move replaced those books). No-op when nothing
  // matches. Takes C strings so a caller's (possibly long) path is never
  // copied before its size is checked.
  void updatePath(const char* oldPath, const char* newPath);

  // Adopts the Library index's title and author for entry i and marks it
  // resolved, unless that would grow the list past ADD_FILE_BYTES (then the
  // entry is unchanged and false is returned). Does not persist; the caller
  // calls flushPending() once after a batch.
  bool setResolved(size_t i, const std::string& title, const std::string& author);
  // Records that a lookup for entry i found nothing (see lookupTried).
  void markLookupTried(size_t i);
  // After the index was rebuilt: every entry is looked up again, so titles
  // follow a metadata-mode change or a book replaced in place. The old display
  // copy stays until the lookup succeeds.
  void invalidateResolved();

  // Drops entries whose file is gone from the card. Does not persist; marks
  // the store for flushPending() when anything was dropped. Skipped when the
  // card's /.crosspoint folder is not visible (card not readable). One exists()
  // per favorite, so the Library runs it once per boot and after a rebuild.
  bool pruneMissing();

  // A change is waiting to be saved (see flushPending()).
  bool hasUnsaved() const { return unsaved; }

  // Entry i, 0 <= i < getCount().
  const FavoriteBook& at(size_t i) const { return *favorites[i]; }
  int getCount() const { return static_cast<int>(favorites.size()); }
  // Whether toggle() could add this exact entry: not read-only, under the
  // count cap, a storable path, and the list in memory plus this entry's
  // measured JSON fits under ADD_FILE_BYTES.
  bool canAdd(const std::string& path, const std::string& title, const std::string& author) const;
};

#define FAVORITES FavoritesStore::getInstance()
