#pragma once
#include <HalStorage.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>
#include <string>

namespace serialization {

// Sequential buffered wrappers over HalFile.
//
// SdFat keeps ONE shared 512-byte sector cache per volume, so interleaving small
// reads/writes across two or more files evicts and reloads that sector on nearly
// every call -- each 4-byte pod becomes a full SD transaction (measured: 31s to
// stream ~200KB through BookMetadataCache::buildBookBin on a 1,732-spine EPUB).
// Batching into chunk-sized transfers keeps each file at sequential SD speed.
//
// Heap: one fixed buffer per wrapper, allocated once at construction and freed at
// scope exit. If the allocation fails the wrapper degrades to unbuffered
// passthrough -- correct, just slow -- so callers never need an OOM path.
//
// Constraint: the wrapper must be the file's ONLY accessor while alive (it tracks
// the underlying position itself); mixing direct HalFile calls in desynchronizes it.

class BufferedFileWriter {
 public:
  BufferedFileWriter(HalFile& file, const size_t capacity)
      : file(file), buf(makeUniqueNoThrow<uint8_t[]>(capacity)), cap(buf ? capacity : 0), pos(file.position()) {}
  ~BufferedFileWriter() { flush(); }
  BufferedFileWriter(const BufferedFileWriter&) = delete;
  BufferedFileWriter& operator=(const BufferedFileWriter&) = delete;

  void write(const void* src, const size_t len) {
    pos += len;
    const auto* p = static_cast<const uint8_t*>(src);
    if (fill + len > cap) {
      flushBuffer();
    }
    if (len >= cap) {  // also the cap == 0 passthrough
      okFlag &= file.write(p, len) == len;
      return;
    }
    // Typed local: cppcheck misreads unique_ptr<uint8_t[]>::get() arithmetic as void*.
    uint8_t* const data = buf.get();
    memcpy(data + fill, p, len);
    fill += len;
  }

  // Logical write position (bytes written since the file was opened).
  size_t position() const { return pos; }

  // Flush buffered bytes; returns false if any write so far has failed short.
  bool flush() {
    flushBuffer();
    return okFlag;
  }

 private:
  void flushBuffer() {
    if (fill == 0) return;
    okFlag &= file.write(buf.get(), fill) == fill;
    fill = 0;
  }

  HalFile& file;
  std::unique_ptr<uint8_t[]> buf;
  const size_t cap;
  size_t fill = 0;
  size_t pos;
  bool okFlag = true;
};

class BufferedFileReader {
 public:
  BufferedFileReader(HalFile& file, const size_t capacity)
      : file(file), buf(makeUniqueNoThrow<uint8_t[]>(capacity)), cap(buf ? capacity : 0), bufStart(file.position()) {}
  BufferedFileReader(const BufferedFileReader&) = delete;
  BufferedFileReader& operator=(const BufferedFileReader&) = delete;

  size_t read(void* dst, size_t len) {
    auto* p = static_cast<uint8_t*>(dst);
    if (cap == 0) {  // passthrough
      const int n = file.read(p, len);
      const size_t got = n < 0 ? 0 : static_cast<size_t>(n);
      bufStart += got;
      return got;
    }
    size_t total = 0;
    while (len > 0) {
      if (off == fill) {
        bufStart += fill;
        off = 0;
        const int n = file.read(buf.get(), cap);
        fill = n < 0 ? 0 : static_cast<size_t>(n);
        if (fill == 0) break;  // EOF or error
      }
      const size_t chunk = std::min(len, fill - off);
      // Typed local: cppcheck misreads unique_ptr<uint8_t[]>::get() arithmetic as void*.
      const uint8_t* const data = buf.get();
      memcpy(p, data + off, chunk);
      p += chunk;
      off += chunk;
      len -= chunk;
      total += chunk;
    }
    return total;
  }

  // Logical read position.
  size_t position() const { return bufStart + off; }

  bool seek(const size_t target) {
    // Within the buffered window: just move the cursor.
    if (cap != 0 && target >= bufStart && target < bufStart + fill) {
      off = target - bufStart;
      return true;
    }
    if (!file.seek(target)) return false;
    bufStart = target;
    fill = 0;
    off = 0;
    return true;
  }

 private:
  HalFile& file;
  std::unique_ptr<uint8_t[]> buf;
  const size_t cap;
  size_t fill = 0;
  size_t off = 0;
  size_t bufStart;
};

// serialization:: overloads mirroring the HalFile ones in Serialization.h.
template <typename T>
void writePod(BufferedFileWriter& out, const T& value) {
  out.write(&value, sizeof(T));
}

template <typename T>
void readPod(BufferedFileReader& in, T& value) {
  in.read(&value, sizeof(T));
}

inline void writeString(BufferedFileWriter& out, const std::string& s) {
  const uint32_t len = s.size();
  writePod(out, len);
  out.write(s.data(), len);
}

inline void readString(BufferedFileReader& in, std::string& s) {
  uint32_t len;
  readPod(in, len);
  s.resize(len);
  if (len > 0) {
    in.read(&s[0], len);
  }
}

}  // namespace serialization
