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

#include "PocketLibrary.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <ZimSearch.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace pocketlib {
namespace {

constexpr const char* kLibraryDir = "/library";
constexpr const char* kManifestPath = "/library/manifest.json";
constexpr size_t kMaxManifestBytes = 64 * 1024;

// One file on the card as a zim::Source. Reads go through HalStorage (one
// mutex for all SD access). The position is tracked so sequential reads,
// the common case inside a cluster, skip the seek.
class HalFileSource final : public zim::Source {
 public:
  static std::unique_ptr<HalFileSource> open(const std::string& path) {
    HalFile f;
    if (!Storage.openFileForRead("PLIB", path, f)) return nullptr;
    auto src = makeUniqueNoThrow<HalFileSource>();
    if (!src) return nullptr;
    src->size_ = f.fileSize64();
    src->file_ = std::move(f);
    return src;
  }

  uint64_t size() const override { return size_; }

  bool read(uint64_t offset, void* dst, size_t len) override {
    if (offset > size_ || len > size_ - offset) return false;
    if (len == 0) return true;
    if (offset != pos_) {
      if (!file_.seek64(offset)) {
        pos_ = UINT64_MAX;
        return false;
      }
    }
    auto* out = static_cast<uint8_t*>(dst);
    size_t done = 0;
    while (done < len) {
      // HalFile::read takes an int-sized count; stay well inside it.
      const size_t chunk = std::min<size_t>(len - done, 1u << 20);
      const int n = file_.read(out + done, chunk);
      if (n <= 0) {
        pos_ = UINT64_MAX;
        return false;
      }
      done += static_cast<size_t>(n);
    }
    pos_ = offset + len;
    return true;
  }

 private:
  HalFile file_;
  uint64_t size_ = 0;
  uint64_t pos_ = 0;
};

void* psramAllocate(size_t bytes) { return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
void psramRelease(void* p) { heap_caps_free(p); }
const zim::Allocator kPsramAllocator{psramAllocate, psramRelease};

bool endsWith(const std::string& s, const char* suffix) {
  const size_t n = strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// "wikipedia_en_all_nopic_2026-06.zimaa" -> "2026-06"
std::string dateFromName(const std::string& name) {
  const size_t dot = name.find('.');
  const std::string stem = name.substr(0, dot);
  const size_t us = stem.rfind('_');
  return us == std::string::npos ? std::string() : stem.substr(us + 1);
}

}  // namespace

Library& Library::instance() {
  static Library library;
  return library;
}

size_t Library::load() {
  close();
  collections_.clear();
  loadError_.clear();
  if (!loadManifest()) scanFolders();
  slots_.clear();
  slots_.resize(collections_.size());
  LOG_INF("PLIB", "%u collections on the card", static_cast<unsigned>(collections_.size()));
  return collections_.size();
}

bool Library::loadManifest() {
  HalFile f;
  if (!Storage.openFileForRead("PLIB", kManifestPath, f)) return false;
  const size_t size = f.fileSize();
  if (size == 0 || size > kMaxManifestBytes) {
    loadError_ = "manifest.json has an unexpected size";
    return false;
  }
  std::string text(size, '\0');
  if (f.read(text.data(), size) != static_cast<int>(size)) {
    loadError_ = "manifest.json could not be read";
    return false;
  }
  f.close();

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, text);
  if (err) {
    loadError_ = std::string("manifest.json: ") + err.c_str();
    LOG_ERR("PLIB", "%s", loadError_.c_str());
    return false;
  }
  for (JsonObject c : doc["collections"].as<JsonArray>()) {
    Collection col;
    col.key = c["key"] | "";
    col.title = c["title"] | "";
    col.description = c["description"] | "";
    col.date = c["date"] | "";
    if (col.date.size() > 7) col.date.resize(7);  // "2026-06-17" -> "2026-06"
    for (JsonObject p : c["zim"]["parts"].as<JsonArray>()) {
      const char* path = p["path"] | "";
      if (!*path) continue;
      col.parts.push_back(path[0] == '/' ? std::string(path) : std::string("/") + path);
      col.bytes += p["size"] | 0ull;
    }
    const char* ix = c["index"]["path"] | "";
    if (*ix) col.indexPath = ix[0] == '/' ? std::string(ix) : std::string("/") + ix;
    col.group = c["group"] | "";
    const char* icon = c["icon"]["path"] | "";
    if (*icon) col.iconPath = icon[0] == '/' ? std::string(icon) : std::string("/") + icon;
    if (col.title.empty()) col.title = col.key;
    if (!col.parts.empty()) collections_.push_back(std::move(col));
  }
  return !collections_.empty();
}

void Library::scanFolders() {
  auto root = Storage.open(kLibraryDir);
  if (!root || !root.isDirectory()) {
    if (loadError_.empty()) loadError_ = "No /library folder on the card";
    return;
  }
  char name[128];
  root.rewindDirectory();
  for (auto dir = root.openNextFile(); dir; dir = root.openNextFile()) {
    if (!dir.isDirectory()) continue;
    dir.getName(name, sizeof(name));
    if (name[0] == '.') continue;
    Collection col;
    col.key = name;
    col.title = name;
    if (!col.title.empty()) col.title[0] = static_cast<char>(toupper(col.title[0]));
    const std::string folder = std::string(kLibraryDir) + "/" + name;
    std::vector<std::pair<std::string, uint64_t>> zims;
    dir.rewindDirectory();
    for (auto f = dir.openNextFile(); f; f = dir.openNextFile()) {
      if (f.isDirectory()) continue;
      f.getName(name, sizeof(name));
      const std::string file = name;
      if (file[0] == '.') continue;
      const size_t dot = file.rfind('.');
      const std::string ext = dot == std::string::npos ? "" : file.substr(dot);
      const bool part = ext.size() == 6 && ext.compare(0, 4, ".zim") == 0;  // .zimaa ...
      if (ext == ".zim" || part) zims.emplace_back(folder + "/" + file, f.fileSize64());
      if (ext == ".pltitles") col.indexPath = folder + "/" + file;
    }
    std::sort(zims.begin(), zims.end());
    for (auto& z : zims) {
      col.parts.push_back(z.first);
      col.bytes += z.second;
    }
    if (!col.parts.empty()) {
      col.date = dateFromName(col.parts.front().substr(col.parts.front().rfind('/') + 1));
      collections_.push_back(std::move(col));
    }
  }
  std::sort(collections_.begin(), collections_.end(),
            [](const Collection& a, const Collection& b) { return a.key < b.key; });
  if (collections_.empty() && loadError_.empty()) loadError_ = "No ZIM files under /library";
}

zim::Archive* Library::open(size_t i, zim::Error* error) {
  zim::Archive* archive = ensureOpen(i, error);
  if (!archive) return archive;
  // Only the collection being read keeps decoded clusters in PSRAM. Every
  // time, not only when the focus moves: a search may have decoded listing
  // clusters in the others since.
  dropOtherCaches(i);
  focus_ = i;
  return archive;
}

zim::Archive* Library::ensureOpen(size_t i, zim::Error* error) {
  if (error) *error = zim::Error::None;
  if (i >= collections_.size() || i >= slots_.size()) {
    if (error) *error = zim::Error::NotFound;
    return nullptr;
  }
  Slot& slot = slots_[i];
  if (slot.archive) return slot.archive.get();
  if (slot.failed) {
    if (error) *error = zim::Error::Io;
    return nullptr;
  }

  const Collection& col = collections_[i];
  std::unique_ptr<zim::Source> source;
  if (col.parts.size() == 1) {
    source = HalFileSource::open(col.parts[0]);
  } else {
    auto split = makeUniqueNoThrow<zim::SplitSource>();
    if (split) {
      for (const auto& p : col.parts) {
        auto part = HalFileSource::open(p);
        if (!part || !split->addPart(std::move(part))) {
          LOG_ERR("PLIB", "cannot open part %s", p.c_str());
          split.reset();
          break;
        }
      }
    }
    source = std::move(split);
  }
  if (!source) {
    slot.failed = true;
    if (error) *error = zim::Error::Io;
    return nullptr;
  }

  auto archive = makeUniqueNoThrow<zim::Archive>();
  if (!archive) {
    if (error) *error = zim::Error::NoMemory;
    return nullptr;
  }
  zim::Options options;
  options.allocator = &kPsramAllocator;
  // Three decoded clusters (up to ~2 MiB each): the article being read, a
  // picture's, and the article before it (Back), so a picture no longer
  // pushes the article out. PSRAM also holds the article text and SD fonts;
  // the caches are dropped when PSRAM runs short (see below).
  options.clusterCacheSize = 3;
  const uint32_t t0 = millis();
  const zim::Error e = archive->open(std::move(source), options);
  if (e != zim::Error::None) {
    LOG_ERR("PLIB", "open %s: %s", col.key.c_str(), zim::errorName(e));
    slot.failed = e != zim::Error::NoMemory;
    if (error) *error = e;
    return nullptr;
  }
  LOG_INF("PLIB", "opened %s: %u parts, %u entries, %u titles in %u ms", col.key.c_str(),
          static_cast<unsigned>(col.parts.size()), static_cast<unsigned>(archive->entryCount()),
          static_cast<unsigned>(archive->titleCount()), static_cast<unsigned>(millis() - t0));
  // Opening may have decoded a listing; that belongs to no article yet.
  if (focus_ != i) archive->clearCache();

  if (!col.indexPath.empty()) {
    auto index = makeUniqueNoThrow<zim::TitleIndex>();
    auto src = HalFileSource::open(col.indexPath);
    if (index && src && index->open(std::move(src)) == zim::Error::None && index->matches(*archive)) {
      slot.index = std::move(index);
    } else {
      LOG_ERR("PLIB", "title index %s missing or built for another file", col.indexPath.c_str());
    }
  }
  slot.archive = std::move(archive);
  return slot.archive.get();
}

void Library::dropOtherCaches(size_t keep) {
  for (size_t k = 0; k < slots_.size(); k++) {
    if (k != keep && slots_[k].archive) slots_[k].archive->clearCache();
  }
}

zim::TitleIndex* Library::titleIndex(size_t i) {
  if (i >= slots_.size()) return nullptr;
  const Slot& slot = slots_[i];
  return slot.archive && slot.index && slot.index->isOpen() ? slot.index.get() : nullptr;
}

bool Library::search(const std::vector<size_t>& scope, std::string_view query, size_t max, std::vector<Hit>& out) {
  out.clear();
  const uint32_t t0 = millis();
  std::vector<zim::SearchSource> sources;
  std::vector<size_t> owners;
  sources.reserve(scope.size());
  owners.reserve(scope.size());
  for (size_t i : scope) {
    zim::Archive* archive = ensureOpen(i, nullptr);
    if (!archive) continue;
    Slot& slot = slots_[i];
    zim::TitleIndex* index = titleIndex(i);
    if (index && !slot.cursor) slot.cursor = makeUniqueNoThrow<zim::TitleIndex::Cursor>();
    if (index && !slot.cursor) continue;
    sources.push_back({archive, index, index ? slot.cursor.get() : nullptr});
    owners.push_back(i);
  }
  if (sources.empty()) return false;
  std::vector<zim::MultiHit> hits;
  const zim::Error err = zim::searchMany(sources, query, max, hits);
  lastSearchMs = millis() - t0;
  if (err != zim::Error::None) LOG_ERR("PLIB", "search: %s", zim::errorName(err));
  LOG_DBG("PLIB", "search \"%.*s\" in %u: %u hits, %u ms", static_cast<int>(query.size()), query.data(),
          static_cast<unsigned>(sources.size()), static_cast<unsigned>(hits.size()),
          static_cast<unsigned>(lastSearchMs));
  dropOtherCaches(focus_);  // collections without an index may have decoded title listings
  out.reserve(hits.size());
  for (auto& h : hits) out.push_back({owners[h.source], h.hit.entry, std::move(h.hit.title), h.hit.exact});
  return err == zim::Error::None;
}

void Library::close() {
  for (Slot& slot : slots_) slot = Slot();
  focus_ = SIZE_MAX;
}

std::string formatBytes(uint64_t bytes) {
  char buf[24];
  if (bytes >= 1000ull * 1000 * 1000)
    snprintf(buf, sizeof(buf), "%.1f GB", bytes / 1e9);
  else if (bytes >= 1000ull * 1000)
    snprintf(buf, sizeof(buf), "%u MB", static_cast<unsigned>(bytes / 1000000ull));
  else
    snprintf(buf, sizeof(buf), "%u KB", static_cast<unsigned>(bytes / 1000ull));
  return buf;
}

std::string formatCount(uint32_t n) {
  std::string digits = std::to_string(n);
  std::string out;
  const int len = static_cast<int>(digits.size());
  for (int i = 0; i < len; i++) {
    out.push_back(digits[i]);
    const int left = len - 1 - i;
    if (left > 0 && left % 3 == 0) out.push_back(',');
  }
  return out;
}

}  // namespace pocketlib

#endif  // POCKET_LIBRARY
