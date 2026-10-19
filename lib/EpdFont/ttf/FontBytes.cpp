#include "FontBytes.h"

namespace ttf {

const uint8_t* SdPageCache::page(const FileSource& file, const uint32_t index) {
  // An outline is read a few bytes at a time, almost always from the page the last read came from.
  if (owner_[last_] == file.ctx && index_[last_] == index) return data_[last_];
  uint8_t slot = 0;
  for (uint8_t i = 0; i < kPages; ++i) {
    if (owner_[i] == file.ctx && index_[i] == index) {
      used_[i] = ++clock_;
      last_ = i;
      return data_[i];
    }
    if (used_[i] < used_[slot]) slot = i;
  }
  const uint32_t start = index * kPageBytes;
  if (start >= file.size || !file.read) return nullptr;
  const uint32_t bytes = file.size - start < kPageBytes ? file.size - start : kPageBytes;
  owner_[slot] = nullptr;  // a failed read leaves the page empty
  if (!file.read(file.ctx, start, data_[slot], bytes)) return nullptr;
  ++misses_;
  owner_[slot] = file.ctx;
  index_[slot] = index;
  used_[slot] = ++clock_;
  last_ = slot;
  return data_[slot];
}

bool SdPageCache::read(const FileSource& file, uint32_t offset, uint32_t len, void* dst) {
  if (file.ctx == nullptr || offset > file.size || len > file.size - offset) return false;
  auto* out = static_cast<uint8_t*>(dst);
  while (len > 0) {
    const uint8_t* page = this->page(file, offset / kPageBytes);
    if (!page) return false;
    const uint32_t at = offset % kPageBytes;
    const uint32_t n = len < kPageBytes - at ? len : kPageBytes - at;
    std::memcpy(out, page + at, n);
    out += n;
    offset += n;
    len -= n;
  }
  return true;
}

}  // namespace ttf

bool FontBytes::readFar(const uint32_t offset, const uint32_t len, void* dst) {
  if (sd_ && offset >= sd_->fontOffset && offset - sd_->fontOffset <= sd_->length &&
      len <= sd_->length - (offset - sd_->fontOffset)) {
    if (sd_->cache && sd_->cache->read(sd_->file, sd_->fileOffset + (offset - sd_->fontOffset), len, dst)) return true;
    ioError_ = true;
    return false;
  }
  outOfRange_ = true;
  return false;
}
