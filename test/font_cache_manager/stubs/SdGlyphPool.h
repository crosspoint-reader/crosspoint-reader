#pragma once
class SdGlyphPool {
 public:
  unsigned depth = 0;
  unsigned boundaries = 0;
  void beginScope() {
    ++depth;
    ++boundaries;
  }
  void endScope() {
    --depth;
    ++boundaries;
  }
  void release() {}
  void clearCache() {}
  bool trimUnpinned() { return false; }
};
