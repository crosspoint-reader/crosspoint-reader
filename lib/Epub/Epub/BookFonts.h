#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

class BuildArena;
class Epub;

/// A book's embedded body font (the Publisher Fonts setting), made ready for the TTF engine: found once per
/// book from its CSS and the text of its first chapters, and copied out of the EPUB as plain TrueType files to
/// `<book cache>/fonts/<family>/`, where it loads like a `.ttf` family on the card.
///
/// One family per book, its body text's: the reader draws a book with one family (see the TTF spec, section 11).
/// What is refused, so the book keeps the reader's font: a body in a generic or a non-embedded family; CFF
/// outlines (`OTTO`), WOFF, WOFF2 and font collections; fonts encrypted rather than obfuscated; faces over
/// kMaxFaceBytes.
namespace BookFonts {

/// Bump when what prepare() decides or writes changes: every book is examined again.
/// v2: the state file holds a Family's name to its end, without a length byte.
constexpr uint8_t kVersion = 2;
/// A face bigger than this is not copied: the font area could not install it in flash mode anyway.
constexpr size_t kMaxFaceBytes = 2 * 1024 * 1024;
/// prepare() reads at most this much of each chapter, and at most kProbeChapters chapters from the text start.
constexpr size_t kProbeChapterBytes = 64 * 1024;
constexpr int kProbeChapters = 6;
/// prepare() stops reading chapters once it has seen this much text.
constexpr uint32_t kProbeEnoughText = 16 * 1024;

enum class State : uint8_t {
  Pending,  ///< not examined yet (or by another kVersion), or the last attempts failed for a passing reason
  None,     ///< examined: the book keeps the reader's font
  Family,   ///< examined: its body family is on the card, in familyDir()
};

/// What prepare() found for the book whose cache is at `cachePath`. For Family, `*name` gets the family's name
/// as the book declares it.
State read(const std::string& cachePath, std::string* name = nullptr);
/// Where a Family's files are: `<cachePath>/fonts/<the name, made safe for a FAT name>`.
std::string familyDir(const std::string& cachePath, std::string_view name);

/// Finds the book's body family and copies its font files to the card. `scratch`, when set, hosts the inflate
/// ring and the parser state (the lent framebuffer at book open); otherwise the heap. The CSS must be loaded
/// (Epub::load without skipping it). Writes the state file: Family or None, or, when a read failed, another
/// failed attempt, so a passing shortage is retried at the next open and a broken book is not retried for
/// ever. Returns the state the next read() gives.
State prepare(const Epub& epub, BuildArena* scratch);

// --- Exposed for tests ---

/// The body family: the family most of the text is set in, an inline accent's text counting for its paragraph's,
/// over the first kProbeChapterBytes of up to kProbeChapters chapters from the text start (fewer once
/// kProbeEnoughText has been seen). 0 for the reader's font. `*failed` is set when a chapter could not be read.
uint32_t detectBodyFamily(const Epub& epub, BuildArena* scratch, bool* failed);

/// The IDPF obfuscation key (EPUB OCF): the SHA-1 of the package's unique identifier with its XML white space
/// removed.
bool idpfKey(std::string_view uniqueIdentifier, uint8_t (&key)[20]);
/// Adobe's: the 16 bytes of the book's `urn:uuid:` identifier. False when it is not one.
bool adobeKey(std::string_view uuid, uint8_t (&key)[16]);

}  // namespace BookFonts
