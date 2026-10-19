#pragma once

#include <cstdint>

// The reader's memory ladder: what it does when the images on a page cannot be decoded for lack of memory, as on a
// page drawn in a TrueType reader font beside a book's three accent families (41-43 KB free on the X4). Each step
// frees more than the one before it, and costs more:
//
//   Plain          the decode with the lent framebuffer as its arena and every font loaded, as always
//   LightRelief    the page's glyphs prefetched into the glyph store, then what the TrueType set's misses hold
//                  (the ring, and a scratch block or SD page cache made on the heap; TtfFontSet::releaseMissMemory)
//                  and the built-in font caches freed for the decode: the next miss and prewarm make them again
//   HeavyRelief    the whole TrueType set unloaded for the decode, which needs no font, and the same set loaded again
//                  after it: no install and the same font IDs, so the page keeps its layout
//                  (SdCardFontManager::suspendTtf)
//   DropAccent     the set cannot be loaded back: the least used accent family is left out of the book, for
//                  good, and the chapter laid out again without it; with no accent family left, a restart
//   Emergency      no decode fits, even with every TrueType family unloaded: the undecoded images are drawn as "not
//                  enough memory" boxes, the text as usual, and the status bar says the page is degraded
//
// A decode that settled for a coarse picture for lack of memory (a progressive JPEG's DC preview, or a coarser scale:
// PixelCache::PXC_MAGIC_COARSE) climbs once, to the light step, and only where the workspace its next finer scale
// needs can be had there (image_scratch::coarseNeedFits): checked before the step, with what the step frees counted,
// and again before its decode. Never to the heavy step: a picture is drawn either way, and is not worth the TrueType
// set's reload, nor the accent family or the restart a failed one costs. Otherwise the coarse picture is drawn, never
// a box.
//
// The decisions alone, apart from the reader, so the host can test them: from how a step's decode went and what is
// loaded, what comes next.
namespace ImageMemoryLadder {

// The steps that decode.
enum class Step : uint8_t { Plain, LightRelief, HeavyRelief };

// How a step's decode went.
enum class Decode : uint8_t {
  Done,       // every image on the page has its pixel cache
  Coarse,     // every image has one, but a decode short of memory settled for a coarse picture (heapShortCount moved)
  HeapShort,  // an image was refused or cut short for memory (image_scratch::heapShortCount moved)
  Failed,     // an image failed for another reason, or input preempted the decode: more memory would not help now
};

enum class Action : uint8_t {
  Done,         // draw the page
  LightRelief,  // decode again at that step
  HeavyRelief,
  DropAccent,  // leave the least used accent family out and lay the chapter out again
  Emergency,   // draw the page, its undecoded images as low-memory boxes
  Restart,     // not even the body's family loads again: a restart's fresh heap loads it, and the images are cached
};

struct Input {
  Step step = Step::Plain;
  Decode decode = Decode::Done;
  bool ttfLoaded = false;   // a TrueType set was loaded when the step began
  bool canUnload = false;   // no section build that laid text out with the set is live: HeavyRelief may run
  bool fontsBack = true;    // what the step freed came back: the set (HeavyRelief)
  uint8_t accents = 0;      // the book's accent families loaded beside the body when the step began
  bool coarseFits = false;  // Decode::Coarse: the light step can make room for the next finer scale's workspace
};

constexpr Action next(const Input& in) {
  if (!in.fontsBack) {
    // However the decode went, the page needs its fonts first. Fewer families take less; with only the body's left,
    // the heap is too broken up for it, and a restart's fresh heap loads it.
    return in.accents > 0 ? Action::DropAccent : Action::Restart;
  }
  switch (in.decode) {
    case Decode::Done:
    case Decode::Failed:
      return Action::Done;
    case Decode::Coarse:
      return in.step == Step::Plain && in.coarseFits ? Action::LightRelief : Action::Done;
    case Decode::HeapShort:
      break;
  }
  switch (in.step) {
    case Step::Plain:
      return Action::LightRelief;
    case Step::LightRelief:
      return in.ttfLoaded && in.canUnload ? Action::HeavyRelief : Action::Emergency;
    case Step::HeavyRelief:
      return Action::Emergency;  // with every TrueType family unloaded: fewer families cannot help the decode
  }
  return Action::Emergency;
}

}  // namespace ImageMemoryLadder
