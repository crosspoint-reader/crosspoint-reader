#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

class BuildArena;
class Epub;

/// A book's embedded fonts (the Publisher Fonts setting), made ready for the TTF engine: found once per book from
/// its CSS and the text of its first chapters, and copied out of the EPUB as plain TrueType files to
/// `<book cache>/fonts/`, where they load like a `.ttf` family on the card.
///
/// Two families at most: the body text's, and an accent family, the one the most other blocks (headings, a
/// dedication) are set in. The glyph store holds them together, or the accent family beside a TrueType reader font
/// (TTF spec, section 11.1). What is refused, so the book keeps the reader's font: a family that is generic or not
/// embedded; CFF outlines (`OTTO`), WOFF, WOFF2 and font collections; fonts encrypted rather than obfuscated; faces
/// over kMaxFaceBytes.
namespace BookFonts {

/// Bump when what prepare() decides or writes changes: every book is examined again.
/// v2: the state file holds a Family's name to its end, without a length byte.
/// v3: the faces sit in fonts/ itself as <style>.ttf, not in a folder named after the family; the state file is
/// fonts.bin beside it.
/// v4: the state file also holds the body's font-family and the accent family (fonts/accent/), and a book without
/// @font-face rules is probed too, for its body's generic family.
constexpr uint8_t kVersion = 4;
/// A face bigger than this is not copied: the font area could not install it in flash mode anyway.
constexpr size_t kMaxFaceBytes = 2 * 1024 * 1024;
/// prepare() reads at most this much of each chapter, and at most kProbeChapters chapters from the text start.
constexpr size_t kProbeChapterBytes = 64 * 1024;
constexpr int kProbeChapters = 6;
/// prepare() stops reading chapters once it has seen this much text.
constexpr uint32_t kProbeEnoughText = 16 * 1024;

enum class State : uint8_t {
  Pending,  ///< not examined yet (or by another kVersion), or the last attempts failed for a passing reason
  None,     ///< examined: the book's body keeps the reader's font
  Family,   ///< examined: its body family is on the card, in familyDir()
};

/// The families prepare() found, besides the body family's State.
struct Families {
  uint32_t body = 0;    ///< the body text's font-family (CssStyle::fontFamily); 0 for the reader's font
  uint32_t accent = 0;  ///< the accent family's identity (CssFontFamily) when it is in familyDir(, true), else 0
};

/// What prepare() found for the book whose cache is at `cachePath`. `*name` gets the name the book declares for
/// its body family when `body` and the State is Family, else for its accent family ("" for none).
State read(const std::string& cachePath, std::string* name = nullptr, Families* families = nullptr, bool body = true);
/// Where a Family's files are: `<cachePath>/fonts`, one `<style>.ttf` per style (EpdFontFamily numbering); the
/// accent family's, named the same, in `<cachePath>/fonts/accent`. The state file is `<cachePath>/fonts.bin`.
std::string familyDir(const std::string& cachePath, bool accent = false);

/// Finds the book's body and accent families and copies their font files to the card. `scratch`, when set, hosts
/// the inflate ring and the parser state (the lent framebuffer at book open); otherwise the heap. The CSS must be
/// loaded (Epub::load without skipping it). Writes the state file: Family or None, or, when a read failed, another
/// failed attempt, so a passing shortage is retried at the next open and a broken book is not retried for ever.
/// Returns the state the next read() gives.
State prepare(const Epub& epub, BuildArena* scratch);

// --- Exposed for tests ---

struct Found {
  /// The body family: the family most of the text is set in (CssStyle::fontFamily; families compare by identity),
  /// an inline accent's text counting for its paragraph's, over the first kProbeChapterBytes of up to
  /// kProbeChapters chapters from the text start (fewer once kProbeEnoughText has been seen). 0 for the reader's font.
  uint32_t body = 0;
  /// The accent family's identity: the family the most blocks (p, div, li, dd, blockquote, pre, h1-h6) are set in,
  /// other than the body's, among those `records` (CssParser::loadFontFaces) has a regular face of; 0 for none. A
  /// block is set in its text's family when all of its text is in one, else in its own.
  uint32_t accent = 0;
};
/// `*failed` is set when a chapter could not be read.
Found detectFamilies(const Epub& epub, BuildArena* scratch, bool* failed, std::string_view records = {});

/// The IDPF obfuscation key (EPUB OCF): the SHA-1 of the package's unique identifier with its XML white space
/// removed.
bool idpfKey(std::string uniqueIdentifier, uint8_t (&key)[20]);
/// Adobe's: the 16 bytes of the book's `urn:uuid:` identifier. False when it is not one.
bool adobeKey(std::string_view uuid, uint8_t (&key)[16]);

}  // namespace BookFonts
