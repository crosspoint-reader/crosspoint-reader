// Pocket Library for CrossPoint
// Copyright (C) 2026 Pocket Library contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. See LICENSE-GPL-3.0 at the repository root.

#include "ZimArchive.h"

#include <algorithm>
#include <cstring>

#include "ZimDecompress.h"

namespace zim {

namespace {

constexpr uint32_t kMagic = 0x044D495Au;  // "ZIM\x04" little-endian
constexpr size_t kHeaderSize = 80;
constexpr uint64_t kAbsent64 = 0xffffffffffffffffull;
// Directory entries are read in one go when they fit; longer ones (very long
// paths) grow the read up to this cap.
constexpr size_t kDirentFirstRead = 512;
constexpr size_t kDirentMaxRead = 64 * 1024;
constexpr size_t kMimeListMaxRead = 64 * 1024;

uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
uint64_t le64(const uint8_t* p) { return static_cast<uint64_t>(le32(p)) | (static_cast<uint64_t>(le32(p + 4)) << 32); }

// Byte-wise comparison of (namespace, text) pairs, matching the order the
// directory and title lists are sorted in.
int compareKey(char nsA, std::string_view a, char nsB, std::string_view b) {
  const auto ua = static_cast<unsigned char>(nsA);
  const auto ub = static_cast<unsigned char>(nsB);
  if (ua != ub) return ua < ub ? -1 : 1;
  const int c = a.compare(b);  // char_traits<char> compares as unsigned char
  return c < 0 ? -1 : (c > 0 ? 1 : 0);
}

}  // namespace

const char* errorName(Error e) {
  switch (e) {
    case Error::None:
      return "ok";
    case Error::Io:
      return "read error";
    case Error::BadMagic:
      return "not a ZIM file";
    case Error::BadVersion:
      return "unsupported ZIM version";
    case Error::BadHeader:
      return "corrupt header";
    case Error::BadDirent:
      return "corrupt directory entry";
    case Error::BadCluster:
      return "corrupt cluster";
    case Error::Unsupported:
      return "unsupported compression";
    case Error::Decompress:
      return "decompression failed";
    case Error::TooLarge:
      return "cluster too large";
    case Error::NoMemory:
      return "out of memory";
    case Error::NotFound:
      return "not found";
    case Error::OutOfRange:
      return "index out of range";
    case Error::RedirectLoop:
      return "redirect loop";
    case Error::NotContent:
      return "entry has no content";
  }
  return "unknown error";
}

Archive::Archive() = default;
Archive::~Archive() { close(); }

void Archive::close() {
  releaseCache();
  source_.reset();
  mimeTypes_.clear();
  header_ = Header();
  titles_ = TitleList();
  titleCount_ = 0;
  titleSource_ = TitleSource::None;
  articleList_ = TitleList();
  articleListCount_ = 0;
  newNamespaces_ = false;
  stats_ = CacheStats();
}

Error Archive::open(std::unique_ptr<Source> source, const Options& options) {
  close();
  if (!source) return Error::Io;
  source_ = std::move(source);
  options_ = options;
  if (options_.clusterCacheSize == 0) options_.clusterCacheSize = 1;
  allocator_ = options_.allocator ? options_.allocator : &Allocator::defaultAllocator();

  Error err = readHeader();
  if (err == Error::None) err = readMimeList();
  if (err == Error::None) err = setUpTitleIndex();
  if (err != Error::None) close();
  return err;
}

Error Archive::readHeader() {
  const uint64_t fileSize = source_->size();
  if (fileSize < kHeaderSize) return Error::BadHeader;
  uint8_t h[kHeaderSize];
  if (!source_->read(0, h, sizeof(h))) return Error::Io;
  if (le32(h) != kMagic) return Error::BadMagic;

  header_.majorVersion = le16(h + 4);
  header_.minorVersion = le16(h + 6);
  std::memcpy(header_.uuid, h + 8, 16);
  header_.entryCount = le32(h + 24);
  header_.clusterCount = le32(h + 28);
  header_.pathPtrPos = le64(h + 32);
  header_.titlePtrPos = le64(h + 40);
  header_.clusterPtrPos = le64(h + 48);
  header_.mimeListPos = le64(h + 56);
  header_.mainPage = le32(h + 64);
  header_.layoutPage = le32(h + 68);
  header_.checksumPos = le64(h + 72);

  if (header_.majorVersion != 5 && header_.majorVersion != 6) return Error::BadVersion;
  newNamespaces_ = header_.majorVersion == 6 && header_.minorVersion >= 1;

  // Every table must lie inside the file. Sizes are computed in 64 bits; the
  // counts are 32-bit so the products cannot overflow.
  auto fits = [fileSize](uint64_t pos, uint64_t bytes) { return pos <= fileSize && bytes <= fileSize - pos; };
  if (header_.mimeListPos < kHeaderSize || header_.mimeListPos >= fileSize) return Error::BadHeader;
  if (!fits(header_.pathPtrPos, 8ull * header_.entryCount)) return Error::BadHeader;
  if (!fits(header_.clusterPtrPos, 8ull * header_.clusterCount)) return Error::BadHeader;
  if (header_.titlePtrPos != kAbsent64 && header_.titlePtrPos != 0 &&
      !fits(header_.titlePtrPos, 4ull * header_.entryCount)) {
    return Error::BadHeader;
  }
  // The MD5 checksum (16 bytes) sits at checksumPos; the last cluster ends there.
  if (!fits(header_.checksumPos, 16)) return Error::BadHeader;
  return Error::None;
}

Error Archive::readMimeList() {
  const uint64_t fileSize = source_->size();
  const uint64_t avail = fileSize - header_.mimeListPos;
  const size_t want = static_cast<size_t>(std::min<uint64_t>(avail, kMimeListMaxRead));
  std::string buf(want, '\0');
  if (!source_->read(header_.mimeListPos, buf.data(), want)) return Error::Io;

  size_t pos = 0;
  while (true) {
    const size_t end = buf.find('\0', pos);
    if (end == std::string::npos) return Error::BadHeader;  // unterminated list
    if (end == pos) break;                                  // empty string ends the list
    if (mimeTypes_.size() >= kMimeDeleted) return Error::BadHeader;
    mimeTypes_.emplace_back(buf, pos, end - pos);
    pos = end + 1;
  }
  return Error::None;
}

const std::string& Archive::mimeType(uint16_t index) const {
  static const std::string kEmpty;
  return index < mimeTypes_.size() ? mimeTypes_[index] : kEmpty;
}

// --- directory ---------------------------------------------------------------

Error Archive::entryAt(uint32_t index, Entry& out) {
  if (!source_) return Error::Io;
  if (index >= header_.entryCount) return Error::OutOfRange;

  uint8_t ptrBytes[8];
  if (!source_->read(header_.pathPtrPos + 8ull * index, ptrBytes, 8)) return Error::Io;
  const uint64_t offset = le64(ptrBytes);
  const uint64_t fileSize = source_->size();
  if (offset >= fileSize) return Error::BadDirent;

  size_t want = kDirentFirstRead;
  std::string buf;
  while (true) {
    const bool capped = fileSize - offset <= want;
    const size_t n = capped ? static_cast<size_t>(fileSize - offset) : want;
    buf.resize(n);
    if (!source_->read(offset, buf.data(), n)) return Error::Io;
    if (n < 8) return Error::BadDirent;

    const auto* p = reinterpret_cast<const uint8_t*>(buf.data());
    out.index = index;
    out.mime = le16(p);
    const uint8_t paramLen = p[2];
    out.ns = static_cast<char>(p[3]);
    out.revision = le32(p + 4);
    out.redirectIndex = kNoEntry;
    out.cluster = 0;
    out.blob = 0;

    size_t strings;
    if (out.mime == kMimeRedirect) {
      if (n < 12) return Error::BadDirent;
      out.redirectIndex = le32(p + 8);
      strings = 12;
    } else if (out.mime == kMimeLinkTarget || out.mime == kMimeDeleted) {
      strings = 8;
    } else {
      if (n < 16) return Error::BadDirent;
      if (out.mime >= mimeTypes_.size()) return Error::BadDirent;
      out.cluster = le32(p + 8);
      out.blob = le32(p + 12);
      strings = 16;
    }

    const size_t pathEnd = buf.find('\0', strings);
    const size_t titleEnd = pathEnd == std::string::npos ? std::string::npos : buf.find('\0', pathEnd + 1);
    if (titleEnd != std::string::npos && titleEnd + 1 + paramLen <= n) {
      out.path.assign(buf, strings, pathEnd - strings);
      out.title.assign(buf, pathEnd + 1, titleEnd - pathEnd - 1);
      if (out.title.empty()) out.title = out.path;
      if (out.isRedirect() && out.redirectIndex >= header_.entryCount) return Error::BadDirent;
      if (out.isContent() && out.cluster >= header_.clusterCount) return Error::BadDirent;
      return Error::None;
    }
    // Strings not terminated within what we read: read more, unless we
    // already hit the end of the file or the size cap.
    if (capped || want >= kDirentMaxRead) return Error::BadDirent;
    want = std::min(want * 4, kDirentMaxRead);
  }
}

int Archive::comparePathAt(uint32_t index, char ns, std::string_view path, Entry& scratch, Error& err) {
  err = entryAt(index, scratch);
  if (err != Error::None) return 0;
  return compareKey(scratch.ns, scratch.path, ns, path);
}

Error Archive::findByPath(char ns, std::string_view path, Entry& out) {
  if (!source_) return Error::Io;
  uint32_t lo = 0;
  uint32_t hi = header_.entryCount;
  Entry scratch;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    Error err;
    const int c = comparePathAt(mid, ns, path, scratch, err);
    if (err != Error::None) return err;
    if (c == 0) {
      out = std::move(scratch);
      return Error::None;
    }
    if (c < 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return Error::NotFound;
}

Error Archive::resolve(Entry& entry) {
  int hops = 0;
  while (entry.isRedirect()) {
    if (++hops > options_.maxRedirects) return Error::RedirectLoop;
    const uint32_t target = entry.redirectIndex;
    const Error err = entryAt(target, entry);
    if (err != Error::None) return err;
  }
  return Error::None;
}

Error Archive::mainEntry(Entry& out) {
  if (header_.mainPage != kNoEntry && header_.mainPage < header_.entryCount) {
    Error err = entryAt(header_.mainPage, out);
    if (err == Error::None) err = resolve(out);
    return err;
  }
  Error err = findByPath('W', "mainPage", out);
  if (err == Error::None) err = resolve(out);
  return err;
}

Error Archive::metadata(std::string_view name, std::string& out) {
  Entry e;
  Error err = findByPath('M', name, e);
  if (err != Error::None) return err;
  err = resolve(e);
  if (err != Error::None) return err;
  return read(e, out);
}

// --- title index -------------------------------------------------------------

Error Archive::findListing(std::string_view path, TitleList& list, uint32_t& count) {
  count = 0;
  Entry e;
  Error err = findByPath('X', path, e);
  if (err != Error::None) return err;
  err = resolve(e);
  if (err != Error::None) return err;
  if (!e.isContent()) return Error::NotContent;
  uint64_t bytes = 0;
  err = blobSize(e.cluster, e.blob, bytes);
  if (err != Error::None) return err;
  if (bytes % 4 != 0 || bytes / 4 > 0xffffffffull) return Error::BadCluster;
  list.inBlob = true;
  list.cluster = e.cluster;
  list.blob = e.blob;
  count = static_cast<uint32_t>(bytes / 4);
  return Error::None;
}

Error Archive::setUpTitleIndex() {
  titleCount_ = 0;
  titleSource_ = TitleSource::None;
  articleListCount_ = 0;
  if (newNamespaces_) {
    uint32_t count = 0;
    const Error err = findListing("listing/titleOrdered/v0", titles_, count);
    if (err == Error::None && count > 0) {
      titleCount_ = count;
      titleSource_ = TitleSource::Listing;
    } else if (err != Error::None && err != Error::NotFound) {
      return err;
    }
    uint32_t articles = 0;
    const Error errV1 = findListing("listing/titleOrdered/v1", articleList_, articles);
    if (errV1 == Error::None) {
      articleListCount_ = articles;
    } else if (errV1 != Error::NotFound) {
      return errV1;
    }
  }
  if (titleCount_ == 0 && header_.titlePtrPos != kAbsent64 && header_.titlePtrPos != 0) {
    titles_.inBlob = false;
    titles_.fileOffset = header_.titlePtrPos;
    titleCount_ = header_.entryCount;
    titleSource_ = TitleSource::Header;
  }
  if (titleCount_ == 0 && articleListCount_ > 0) {
    titles_ = articleList_;
    titleCount_ = articleListCount_;
    titleSource_ = TitleSource::Articles;
  }
  return Error::None;
}

Error Archive::readU32At(const TitleList& list, uint32_t position, uint32_t& value) {
  uint8_t b[4];
  if (list.inBlob) {
    const Error err = readBlobRange(list.cluster, list.blob, 4ull * position, 4, b);
    if (err != Error::None) return err;
  } else if (!source_->read(list.fileOffset + 4ull * position, b, 4)) {
    return Error::Io;
  }
  value = le32(b);
  return Error::None;
}

Error Archive::titleEntryAt(uint32_t position, Entry& out) {
  if (position >= titleCount_) return Error::OutOfRange;
  uint32_t index = 0;
  const Error err = readU32At(titles_, position, index);
  if (err != Error::None) return err;
  if (index >= header_.entryCount) return Error::BadHeader;
  return entryAt(index, out);
}

Error Archive::articleListEntryAt(uint32_t position, Entry& out) {
  if (position >= articleListCount_) return Error::OutOfRange;
  uint32_t index = 0;
  const Error err = readU32At(articleList_, position, index);
  if (err != Error::None) return err;
  if (index >= header_.entryCount) return Error::BadCluster;
  return entryAt(index, out);
}

Error Archive::articleListIndices(uint32_t first, uint32_t count, uint32_t* out) {
  if (first > articleListCount_ || count > articleListCount_ - first) return Error::OutOfRange;
  if (count == 0) return Error::None;
  std::vector<uint8_t> raw(4ull * count);
  const Error err = readBlobRange(articleList_.cluster, articleList_.blob, 4ull * first, raw.size(), raw.data());
  if (err != Error::None) return err;
  for (uint32_t i = 0; i < count; ++i) {
    out[i] = le32(raw.data() + 4ull * i);
    if (out[i] >= header_.entryCount) return Error::BadCluster;
  }
  return Error::None;
}

Error Archive::lowerBoundTitle(char ns, std::string_view title, uint32_t& position) {
  uint32_t lo = 0;
  uint32_t hi = titleCount_;
  Entry scratch;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    const Error err = titleEntryAt(mid, scratch);
    if (err != Error::None) return err;
    if (compareKey(scratch.ns, scratch.title, ns, title) < 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  position = lo;
  return Error::None;
}

Error Archive::findByTitle(char ns, std::string_view title, Entry& out) {
  if (titleCount_ == 0) return Error::NotFound;
  uint32_t pos = 0;
  Error err = lowerBoundTitle(ns, title, pos);
  if (err != Error::None) return err;
  if (pos >= titleCount_) return Error::NotFound;
  err = titleEntryAt(pos, out);
  if (err != Error::None) return err;
  if (out.ns != ns || out.title != title) return Error::NotFound;
  return Error::None;
}

// --- clusters and blobs ------------------------------------------------------

Error Archive::clusterInfo(uint32_t cluster, ClusterInfo& info) {
  if (cluster >= header_.clusterCount) return Error::OutOfRange;
  uint8_t b[16];
  const bool last = cluster + 1 == header_.clusterCount;
  if (!source_->read(header_.clusterPtrPos + 8ull * cluster, b, last ? 8 : 16)) return Error::Io;
  info.start = le64(b);
  info.end = last ? header_.checksumPos : le64(b + 8);
  if (info.start >= info.end || info.end > source_->size()) return Error::BadCluster;
  uint8_t infoByte = 0;
  if (!source_->read(info.start, &infoByte, 1)) return Error::Io;
  info.compression = infoByte & 0x0f;
  info.extended = (infoByte & 0x10) != 0;
  return Error::None;
}

Error Archive::locateBlob(uint32_t cluster, uint32_t blob, BlobLocation& loc) {
  ClusterInfo info;
  Error err = clusterInfo(cluster, info);
  if (err != Error::None) return err;

  const size_t offSize = info.extended ? 8 : 4;
  const uint8_t* memory = nullptr;
  uint64_t dataSize = 0;
  uint64_t dataStart = 0;  // file offset of the data, for uncompressed clusters

  if (info.compression <= 1) {
    dataStart = info.start + 1;
    dataSize = info.end - dataStart;
  } else {
    CachedCluster* cached = nullptr;
    err = loadCluster(cluster, info, cached);
    if (err != Error::None) return err;
    memory = cached->data;
    dataSize = cached->size;
  }

  auto readOffset = [&](uint64_t at, uint64_t& value) -> Error {
    if (at > dataSize || offSize > dataSize - at) return Error::BadCluster;
    uint8_t b[8];
    if (memory) {
      std::memcpy(b, memory + at, offSize);
    } else if (!source_->read(dataStart + at, b, offSize)) {
      return Error::Io;
    }
    value = offSize == 8 ? le64(b) : le32(b);
    return Error::None;
  };

  uint64_t first = 0;
  err = readOffset(0, first);
  if (err != Error::None) return err;
  // The first offset points just past the table, so it encodes the table size.
  if (first % offSize != 0 || first < offSize || first > dataSize) return Error::BadCluster;
  const uint64_t blobCount = first / offSize - 1;
  if (blob >= blobCount) return Error::OutOfRange;
  err = validateTable(cluster, memory, dataStart, dataSize, offSize, first);
  if (err != Error::None) return err;

  uint64_t begin = 0;
  uint64_t end = 0;
  err = readOffset(offSize * blob, begin);
  if (err == Error::None) err = readOffset(offSize * (static_cast<uint64_t>(blob) + 1), end);
  if (err != Error::None) return err;
  if (begin < first || begin > end || end > dataSize) return Error::BadCluster;

  loc.memory = memory ? memory + begin : nullptr;
  loc.fileOffset = memory ? 0 : dataStart + begin;
  loc.size = end - begin;
  return Error::None;
}

// Checks once per cluster that the whole offset table is non-decreasing and
// stays inside the cluster, so a corrupt count or stray offset is caught
// even when the damaged entries are never requested directly.
Error Archive::validateTable(uint32_t cluster, const uint8_t* memory, uint64_t dataStart, uint64_t dataSize,
                             size_t offSize, uint64_t first) {
  for (size_t i = 0; i < validatedCount_; ++i) {
    if (validated_[i] == cluster) return Error::None;
  }
  if (first > kMaxValidatedTableBytes) return Error::None;  // per-entry checks still apply

  std::string fileTable;
  const uint8_t* table = memory;
  if (!memory) {
    fileTable.resize(static_cast<size_t>(first));
    if (!source_->read(dataStart, fileTable.data(), fileTable.size())) return Error::Io;
    table = reinterpret_cast<const uint8_t*>(fileTable.data());
  }
  uint64_t prev = first;
  for (uint64_t at = offSize; at < first; at += offSize) {
    const uint64_t v = offSize == 8 ? le64(table + at) : le32(table + at);
    if (v < prev || v > dataSize) return Error::BadCluster;
    prev = v;
  }
  validated_[validatedNext_] = cluster;
  validatedNext_ = (validatedNext_ + 1) % kValidatedRing;
  if (validatedCount_ < kValidatedRing) ++validatedCount_;
  return Error::None;
}

Error Archive::blobSize(uint32_t cluster, uint32_t blob, uint64_t& size) {
  BlobLocation loc;
  const Error err = locateBlob(cluster, blob, loc);
  if (err == Error::None) size = loc.size;
  return err;
}

Error Archive::readBlobRange(uint32_t cluster, uint32_t blob, uint64_t offset, size_t len, void* dst) {
  BlobLocation loc;
  const Error err = locateBlob(cluster, blob, loc);
  if (err != Error::None) return err;
  if (offset > loc.size || len > loc.size - offset) return Error::OutOfRange;
  if (loc.memory) {
    std::memcpy(dst, loc.memory + offset, len);
    return Error::None;
  }
  return source_->read(loc.fileOffset + offset, dst, len) ? Error::None : Error::Io;
}

Error Archive::readBlob(uint32_t cluster, uint32_t blob, std::string& out) {
  BlobLocation loc;
  const Error err = locateBlob(cluster, blob, loc);
  if (err != Error::None) return err;
  if (loc.size > options_.maxClusterBytes) return Error::TooLarge;
  // Built without exceptions, a failed resize would abort: check there is
  // room first.
  if (loc.size > out.capacity()) {
    void* probe = allocator_->allocate(static_cast<size_t>(loc.size) + 1);
    if (!probe) return Error::NoMemory;
    allocator_->release(probe);
  }
  out.resize(static_cast<size_t>(loc.size));
  if (loc.size == 0) return Error::None;
  if (loc.memory) {
    std::memcpy(out.data(), loc.memory, out.size());
    return Error::None;
  }
  return source_->read(loc.fileOffset, out.data(), out.size()) ? Error::None : Error::Io;
}

Error Archive::read(const Entry& entry, std::string& out) {
  if (!entry.isContent()) return Error::NotContent;
  return readBlob(entry.cluster, entry.blob, out);
}

Error Archive::readView(const Entry& entry, std::string& storage, std::string_view& out) {
  out = {};
  if (!entry.isContent()) return Error::NotContent;
  BlobLocation loc;
  const Error err = locateBlob(entry.cluster, entry.blob, loc);
  if (err != Error::None) return err;
  if (loc.memory) {
    out = std::string_view(reinterpret_cast<const char*>(loc.memory), static_cast<size_t>(loc.size));
    return Error::None;
  }
  const Error e = readBlob(entry.cluster, entry.blob, storage);
  if (e == Error::None) out = storage;
  return e;
}

Error Archive::loadCluster(uint32_t cluster, const ClusterInfo& info, CachedCluster*& out) {
  for (auto& c : cache_) {
    if (c.index == cluster) {
      c.lastUse = ++useCounter_;
      ++stats_.hits;
      out = &c;
      return Error::None;
    }
  }
  ++stats_.misses;

  // Make room before decoding: a full cache would otherwise hold its
  // clusters while the new one is decoded beside them, needing one cluster
  // more than the cache size (several MB on a device with 8 MB of PSRAM).
  while (!cache_.empty() && cache_.size() >= options_.clusterCacheSize) evictOldestCluster();

  uint8_t* data = nullptr;
  size_t size = 0;
  Error err = decompress(info, data, size);
  if (err == Error::NoMemory && !cache_.empty()) {
    // Memory is short (other users of the heap, or fragmentation): drop
    // every cached cluster and try once more.
    while (!cache_.empty()) evictOldestCluster();
    err = decompress(info, data, size);
  }
  if (err != Error::None) return err;
  ++stats_.decompressions;

  cache_.push_back(CachedCluster{cluster, data, size, ++useCounter_});
  out = &cache_.back();
  return Error::None;
}

Error Archive::decompress(const ClusterInfo& info, uint8_t*& data, size_t& size) {
  const uint64_t compressedSize = info.end - info.start - 1;
  if (compressedSize > options_.maxCompressedClusterBytes) return Error::TooLarge;

  auto* compressed = static_cast<uint8_t*>(allocator_->allocate(compressedSize ? compressedSize : 1));
  if (!compressed) return Error::NoMemory;
  if (!source_->read(info.start + 1, compressed, static_cast<size_t>(compressedSize))) {
    allocator_->release(compressed);
    return Error::Io;
  }

  DecodeResult r;
  switch (info.compression) {
    case 5:
      r = decodeZstd(compressed, static_cast<size_t>(compressedSize), options_.maxClusterBytes, *allocator_);
      break;
    case 4:
      r = decodeXz(compressed, static_cast<size_t>(compressedSize), options_.maxClusterBytes, *allocator_);
      break;
    default:  // 2 = zlib and 3 = bzip2 are deprecated and absent from current Kiwix files
      allocator_->release(compressed);
      return Error::Unsupported;
  }
  allocator_->release(compressed);
  if (r.status != DecodeStatus::Ok) {
    switch (r.status) {
      case DecodeStatus::TooLarge:
        return Error::TooLarge;
      case DecodeStatus::NoMemory:
        return Error::NoMemory;
      default:
        return Error::Decompress;
    }
  }
  data = r.data;
  size = r.size;
  return Error::None;
}

void Archive::releaseCache() {
  for (auto& c : cache_) {
    if (allocator_ && c.data) allocator_->release(c.data);
  }
  cache_.clear();
  validatedCount_ = 0;
  validatedNext_ = 0;
}

void Archive::clearCache() { releaseCache(); }

void Archive::evictOldestCluster() {
  auto victim = std::min_element(cache_.begin(), cache_.end(),
                                 [](const CachedCluster& a, const CachedCluster& b) { return a.lastUse < b.lastUse; });
  allocator_->release(victim->data);
  cache_.erase(victim);
}

}  // namespace zim
