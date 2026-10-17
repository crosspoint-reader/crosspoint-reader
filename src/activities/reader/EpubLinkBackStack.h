#pragma once

#include <cstddef>
#include <cstdint>

// The EPUB reader's link Back stack: where each followed link (footnote or not) was tapped, newest
// last, so Back returns there instead of closing the book. Kept beside progress.bin as
// linkstack.bin, little-endian:
//   version(1) depth(1) then depth x [spine(2) page(2) pageCount(2) paragraph(2)]
//
// It is persisted so a stack survives sleep and a reopen: the reader reopens on the page it was
// showing, which for a followed link is the destination, and without the stack Back on it closed
// the book. An entry keeps the whole position the reader recorded -- paragraph anchor and page
// count as well as the page -- and is restored by the same path as one that never left memory
// (EpubReaderActivity::restoreSavedPosition): through the paragraph, else the page rescaled by the
// page count. So a stack saved under one layout still finds its place after a font or settings
// change, or under a firmware whose section layout version moved.
//
// Ported from crosspoint-reader PR #3764 ("fix: reopen on the viewed page after following a link",
// suhsoohong / @suhsoohong): theirs is the idea of persisting the stack and the rule that a full
// stack drops its oldest entry. Their file holds spine and page only; this one also carries the
// paragraph anchor and page count, and is not consumed on open (see linkStackOnDisk_).
struct EpubLinkBackStack {
  struct Entry {
    int spineIndex = 0;
    int pageNumber = 0;
    int pageCount = 0;  // 0: unknown (recorded mid-build)
    uint16_t paragraphIndex = 0;
    bool hasParagraph = false;
  };

  static constexpr int kMaxDepth = 3;
  static constexpr uint8_t kVersion = 1;
  static constexpr size_t kEntrySize = 8;
  static constexpr size_t kMaxSize = 2 + kMaxDepth * kEntrySize;

  // `sessionDepth` counts the entries on top of the stack that were pushed in this reader session;
  // the ones below them were loaded from linkstack.bin, so it starts at 0 whatever was loaded. It
  // is what "the reader is on a detour right now" means to anything that is not Back -- the KOSync
  // push on sleep is skipped while it is non-zero -- and keeps that meaning exactly what it was
  // before the stack outlived the session: a link followed and not yet returned from, this session.
  // Only entries are persisted, never this count.

  // A full stack drops its OLDEST entry, so Back from the newest jump always returns to where that
  // jump was made. Refusing the push instead -- what the reader used to do -- sent Back from a
  // fourth jump to the origin of the third, skipping the page the reader had just left.
  static void push(Entry (&stack)[kMaxDepth], int& depth, int& sessionDepth, const Entry& origin) {
    if (depth >= kMaxDepth) {
      for (int i = 1; i < kMaxDepth; i++) stack[i - 1] = stack[i];
      depth = kMaxDepth - 1;
    }
    stack[depth++] = origin;
    // Session entries are always the newest, so the oldest one dropped above is a loaded one for
    // as long as any are left; once all are from this session the count simply stays at the depth.
    if (sessionDepth < depth) sessionDepth++;
  }

  // Removes the newest entry, which is a session one whenever any are left. The caller reads it at
  // stack[depth] afterwards.
  static void pop(int& depth, int& sessionDepth) {
    if (depth <= 0) return;
    depth--;
    if (sessionDepth > 0) sessionDepth--;
  }

  // A link that lands back where an earlier one was followed from -- a note's "Return", a backlink
  // to the marker the reader tapped -- is a return, not one more detour. Answers the depth the stack
  // goes back to: the index of the newest entry on `spine` within a page of `page`, which is dropped
  // with everything above it, as Back would have. -1 when there is none. The newest entry itself is
  // never matched: it was pushed for this very link, and is where it was tapped.
  //
  // Within a page rather than on it, because an id inside a paragraph names the page of a nearby
  // line, not of the marker itself: the page its paragraph starts on, or the one the next block
  // starts on. A backlink to the marker can therefore land a page either side of the saved one.
  //
  // Based on crosspoint-reader PR #3799 (Ruslan Sukharev, @skhrv), whose FootnoteHistory unwinds the
  // same way; theirs matches any page of the origin's spine when the link crosses spines.
  static int returnDepth(const Entry (&stack)[kMaxDepth], const int depth, const int spine, const int page) {
    for (int i = depth - 2; i >= 0; i--) {
      const int pagesAway = stack[i].pageNumber - page;
      if (stack[i].spineIndex == spine && pagesAway >= -1 && pagesAway <= 1) return i;
    }
    return -1;
  }

  // Bytes written to `out`; 0 for an empty stack, which is stored as no file at all.
  static size_t encode(const Entry (&stack)[kMaxDepth], const int depth, uint8_t (&out)[kMaxSize]) {
    if (depth <= 0 || depth > kMaxDepth) return 0;
    out[0] = kVersion;
    out[1] = static_cast<uint8_t>(depth);
    for (int i = 0; i < depth; i++) {
      uint8_t* p = out + 2 + i * kEntrySize;
      put16(p, stack[i].spineIndex);
      put16(p + 2, stack[i].pageNumber);
      put16(p + 4, stack[i].pageCount);
      // 0 is the paragraph LUT's "none" (Section::getParagraphIndexForPage), never an anchor.
      put16(p + 6, stack[i].hasParagraph ? stack[i].paragraphIndex : 0);
    }
    return 2 + depth * kEntrySize;
  }

  // The stored depth, with `stack` filled; 0 when the file is anything but a whole stack this book
  // can use -- another version, a size that is not exactly the depth's, or an entry naming a spine
  // the book no longer has. All or nothing: a stack missing an entry would send Back to the wrong
  // origin. `stack` is left untouched when the answer is 0.
  static int decode(const uint8_t* data, const size_t size, const int spineCount, Entry (&stack)[kMaxDepth]) {
    if (size < 2 || data[0] != kVersion) return 0;
    const int depth = data[1];
    if (depth < 1 || depth > kMaxDepth || size != 2 + depth * kEntrySize) return 0;
    for (int i = 0; i < depth; i++) {
      if (get16(data + 2 + i * kEntrySize) >= spineCount) return 0;
    }
    for (int i = 0; i < depth; i++) {
      const uint8_t* p = data + 2 + i * kEntrySize;
      Entry e;
      e.spineIndex = get16(p);
      e.pageNumber = get16(p + 2);
      e.pageCount = get16(p + 4);
      e.paragraphIndex = get16(p + 6);
      e.hasParagraph = e.paragraphIndex != 0;
      stack[i] = e;
    }
    return depth;
  }

 private:
  static void put16(uint8_t* out, const int value) {
    out[0] = static_cast<uint8_t>(value & 0xFF);
    out[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
  }
  static uint16_t get16(const uint8_t* in) { return static_cast<uint16_t>(in[0] | (in[1] << 8)); }
};
