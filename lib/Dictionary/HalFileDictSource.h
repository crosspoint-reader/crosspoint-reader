#pragma once

#include <HalStorage.h>

#include "DictionaryStore.h"

// Adapts an open HalFile to a DictByteSource. The HalFile must stay open and
// outlive any DictionaryStore using this source.
inline DictByteSource halFileDictSource(HalFile& file) {
  DictByteSource source;
  source.ctx = &file;
  source.readAt = [](void* ctx, uint32_t off, void* buf, uint32_t len) {
    auto& f = *static_cast<HalFile*>(ctx);
    if (!f.seekSet(off)) {
      return false;
    }
    return f.read(buf, len) == static_cast<int>(len);
  };
  return source;
}
