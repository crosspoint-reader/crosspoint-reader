#pragma once

#include <cstdint>

// Who has the secondary framebuffer when an image decode and Background-B's look-ahead build both want it. The decode
// needs the lent buffer as its arena; B builds the next chapter inside the same buffer, and a decode without it runs on
// the heap beside B's build. X3, 2026-10-19, building through a chapter's last pages (BG_BUILD_THROUGH_PAGES) that held
// a JPEG, a progressive JPEG and a PNG: the JPEGs decoded at 11.8 and 15.7 KB free, the PNG found no 32 KB block for
// its ring and was drawn as a low-memory box.
//
// The rule: images the reader is about to see come first.
//   - The image lane looks at its window (the page on screen and the next few) before B may take the buffer, and B
//     starts only once the lane has looked and found nothing to decode, or cannot run at all.
//   - When the lane finds an image to decode while B builds in the buffer, or a page render finds one, B gives the
//     buffer back (its build is discarded; the inflated chapter stays on the card, so the retry skips the inflate)
//     and the decode runs in it.
//
// A yield costs B its parse, so B yields at most kMaxYields times per chapter it builds. Past that the images wait for
// B: the lane decodes them once B is done, and a page render draws them as placeholders, which the lane fills in by
// redrawing the page.
//
// B still makes progress. Waiting for the lane takes the lane's settle after a page appears and one look at the
// window; the window has a few pages, and each of its pages is decoded, given up on after two failed tries, or, when a
// press cuts its decode short, decoded by the render that shows it. So by a chapter's last page at the latest the
// lane finds nothing left and B starts. A yield discards a build, but at most kMaxYields per chapter, and the build
// that follows the last one runs to its end.
//
// A yield is not one of B's preemptions (BG_BUILD_MAX_PREEMPTIONS). Those count builds that lose the race against page
// turns, and two of them leave the chapter to the foreground: the parse does not fit between turns. A yield says
// nothing about that. On a chapter's last pages, where B builds through the turns, two pictures would otherwise hand
// the next chapter to the "Indexing" popup, which building through exists to prevent.
//
// The decisions alone, apart from the reader, so the host can test them.
namespace LaneBeforeBuild {

// What the image lane knows about its window for the page on screen.
enum class Window : uint8_t {
  Closed,   // the lane cannot run (no pages, Background-C holds or lacks the buffer): B decides by its own gates
  Unknown,  // not looked at since the reader moved, the look waits (settle, input) or was cut short, or the lane waits
            // for B's build
  Dirty,    // the lane decoded a page this tick
  Clean,    // looked at: nothing left to decode (the pages the lane gave up on aside)
};

// B may take the buffer for a build only once the lane is done with the window, or cannot use it.
constexpr bool buildMayBorrow(const Window window) { return window == Window::Clean || window == Window::Closed; }

// The yields one target chapter allows. Reading forward, an image decode can meet B's build only on the last
// BG_BUILD_THROUGH_PAGES pages of a chapter, where B keeps the buffer across turns, and on the page where a build that
// began earlier kept it: four pages, and one yield, the lane's or a page render's, decodes a page's images. So a
// chapter that ends in a picture on each of them loses none, and B restarts at most four times: its setup and the
// parse so far (an X3 text chapter: 0.4-1 s).
constexpr uint8_t kMaxYields = 4;

// An image is to be decoded while B holds the buffer for a live build.
enum class Decode : uint8_t {
  TakeFromBuild,  // B gives the buffer back; the decode runs in it
  AfterBuild,     // B keeps it: the lane waits for B, a page render draws the images as placeholders
};

constexpr Decode decodeWhileBuilding(const uint8_t yieldsSoFar) {
  return yieldsSoFar < kMaxYields ? Decode::TakeFromBuild : Decode::AfterBuild;
}

}  // namespace LaneBeforeBuild
