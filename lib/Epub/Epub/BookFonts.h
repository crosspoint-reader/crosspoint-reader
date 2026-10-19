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
/// The body text's family, and up to kMaxAccents accent families, ranked by the blocks (headings, a dedication, a
/// code block) set in them. The glyph store holds them together, or the accent families beside a TrueType reader
/// font (TTF spec, section 11.1). What is refused, so the book keeps the reader's font (or, for an accent family,
/// the next one takes its place): a family that is generic or not embedded; CFF outlines (`OTTO`), WOFF, WOFF2 and
/// font collections; fonts encrypted rather than obfuscated; faces over kMaxFaceBytes.
namespace BookFonts {

/// Bump when what prepare() decides or writes changes: every book is examined again.
/// v2: the state file holds a Family's name to its end, without a length byte.
/// v3: the faces sit in fonts/ itself as <style>.ttf, not in a folder named after the family; the state file is
/// fonts.bin beside it.
/// v4: the state file also holds the body's font-family and the accent family (fonts/accent/), and a book without
/// @font-face rules is probed too, for its body's generic family.
/// v5: up to kMaxAccents accent families, in fonts/accent<n>/.
constexpr uint8_t kVersion = 5;
/// Accent families copied at most: the glyph store's eight style blocks hold a body of four styles and three
/// one-style families, and its face table (32 faces) a book's body sizes and a few of each.
constexpr uint8_t kMaxAccents = 3;
/// A face bigger than this is not copied. Not for the font area: a face too large for flash mode is drawn in SD mode
/// (its tables in flash, its outlines read from the copy on the card), so the size left to bound is the copy itself,
/// extracted from the book once, behind the open's popup, and kept on the card. A full CJK face, as books embed them,
/// is 8-20 MB; one over this is taken for an error rather than minutes of extraction.
constexpr size_t kMaxFaceBytes = 24 * 1024 * 1024;
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
  uint32_t body = 0;  ///< the body text's font-family (CssStyle::fontFamily); 0 for the reader's font
  /// The accent families' identities (CssFontFamily), the most used first, each in familyDir(, n); 0 past the last.
  uint32_t accents[kMaxAccents] = {};
};

/// What prepare() found for the book whose cache is at `cachePath`. `*name` gets the name the book declares for
/// its body family (when the State is Family, else ""), or with `accent` 0..kMaxAccents-1 for that accent family
/// ("" for none).
State read(const std::string& cachePath, std::string* name = nullptr, Families* families = nullptr, int accent = -1);
/// Where a Family's files are: `<cachePath>/fonts`, one `<style>.ttf` per style (EpdFontFamily numbering); accent
/// family n's, named the same, in `<cachePath>/fonts/accent<n>`. The state file is `<cachePath>/fonts.bin`.
std::string familyDir(const std::string& cachePath, int accent = -1);

/// The reader's memory ladder (ImageMemoryLadder) leaves the book's least used accent families out when a page and its
/// images do not fit beside them: the accent families it keeps, from then on, while the font settings stay those it
/// was found under (`settingsKey`, the caller's hash of them). kMaxAccents when it keeps them all: none was set, or
/// it was set under other settings, which forgets it. In `<cachePath>/fontlimit.bin`.
uint8_t accentLimit(const std::string& cachePath, uint32_t settingsKey);
/// Keeps `accents` (kMaxAccents: all, which forgets the limit).
void setAccentLimit(const std::string& cachePath, uint32_t settingsKey, uint8_t accents);

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
  /// The accent families' identities: the families blocks (p, div, li, dd, blockquote, pre, h1-h6) are set in,
  /// other than the body's, among those `records` (CssParser::loadFontFaces) has a regular face of, the one the most
  /// blocks are set in first; 0 past the last. A block is set in its text's family when all of its text is in one,
  /// else in its own.
  uint32_t accents[kMaxAccents] = {};
};
/// `*failed` is set when a chapter could not be read.
Found detectFamilies(const Epub& epub, BuildArena* scratch, bool* failed, std::string_view records = {});

/// The IDPF obfuscation key (EPUB OCF): the SHA-1 of the package's unique identifier with its XML white space
/// removed.
bool idpfKey(std::string uniqueIdentifier, uint8_t (&key)[20]);
/// Adobe's: the 16 bytes of the book's `urn:uuid:` identifier. False when it is not one.
bool adobeKey(std::string_view uuid, uint8_t (&key)[16]);

}  // namespace BookFonts
