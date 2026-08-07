#pragma once

// Text normalisation for the Library index: the fold used for search and sort
// keys, the filename parse that produces the pre-enrichment row, and the author
// key that merges spellings of one person.
//
// Pure functions over UTF-8, no hardware and no allocation beyond the returned
// strings, so the whole unit is host-testable (test/library_text).
//
// Design notes that are easy to get wrong and were measured on a real card
// (docs/superpowers/specs/2026-08-05-addendum-a-findability.md, A2.1-A2.8):
//
//   * The fold DECOMPOSES. `utf8ComposeNfc()` goes the other way, so a fold
//     built on it passes on a card holding only decomposed text and then mangles
//     the first precomposed file to arrive from Windows or Calibre. Both forms
//     must produce the same output, and the host tests assert exactly that.
//   * Some letters have no canonical decomposition at all — U+00F8 (ø) is a
//     distinct letter, not o-with-stroke — so a decompose-only fold turns
//     "Søren" into "Sren". Those need an explicit map.
//   * The author KEY sorts its tokens, so for identity purposes name order
//     stops mattering and no First/Last guess is needed there. Display-side
//     helpers do use narrow rules — cleanPersonName inverts a single-comma
//     "Last, First", surnameKey takes the last word — because showing
//     "Austen, Jane" and "Jane Austen" as two people is worse than the guess.

#include <cstdint>
#include <string>
#include <string_view>

namespace library {

// Longest author key written into an index record. Sized so a key fits the
// record's fixed field; measured to collide 0 times over a 69-book library.
inline constexpr size_t AUTHOR_KEY_MAX_BYTES = 12;

// Casefold and strip diacritics for matching and sorting.
//
// Maps a handful of letters that have no canonical decomposition, decomposes the
// rest and drops combining marks, lowercases ASCII alphanumerics, and turns
// everything else into a single space. Space runs collapse and the result is
// trimmed. Apostrophes survive as ASCII '\'' so "O'Malley" and "L'\xC3\x89n\xC3\xA9ideé"
// keep their shape.
//
// `stripArticle` additionally removes one leading article ("the ", "le ", "la ",
// ...) — correct for sort keys and search text, wrong for anything displayed.
std::string fold(std::string_view text, bool stripArticle = false);

// Title and author as they can be recovered from a filename alone. This is what
// the row shows before metadata enrichment has run, so it is never allowed to be
// empty-handed: `title` falls back to the whole stem.
struct ParsedName {
  std::string title;
  std::string author;  // empty when the filename carries no usable author
};

// Split a filename stem (basename minus extension) on " -- ".
//
// When at least two segments are present, segment 0 is the title and segment 1
// the candidate author; segments 2 and beyond are discarded unread. That is safe
// because no title in the measured corpus contains " -- " and segment counts are
// never exactly 2. Segment 0 is never classified, so the rule cannot mangle a
// title.
//
// Segment 1 passes through one guard (`looksLikeMetadata`) because exporters
// that omit the author leave a publisher, a year or a hash in that position.
ParsedName parseFilename(std::string_view stem);

// True when `raw` is a metadata field rather than a person: a bare hash, an
// ISBN, a year, a "publisher, 2019" tail, or an archive credit.
//
// Takes RAW text, not folded: the fold turns ',' ';' and '#' into spaces, and
// the "publisher, year" shape is defined by exactly those separators. Exposed
// for testing. Only ever applied to a candidate author, never to a title.
bool looksLikeMetadata(std::string_view raw);

// Tidy a person's name for DISPLAY, without reordering it.
//
// Drops bracketed spans ("George Sand [Sand, George]"), everything after a
// multi-author separator, and the trailing underscores and punctuation that
// exporters leave behind ("Lu Xun_", "Herbert G_ Wells"). An underscore
// between letters becomes a full stop, since that is what it replaced in a name
// a filesystem refused to hold.
//
// A single-comma "Austen, Jane" IS turned round into "Jane Austen" — with one
// book per author the spelling vote has no majority to settle it, so the comma
// is the one signal acted on here. Multi-comma names ("Smith, John, Jr.") and
// author lists are left exactly as written. Harmonising the several spellings
// of one person is done by picking the most common one that actually occurs,
// which needs the whole library and so belongs to the index build.
std::string cleanPersonName(std::string_view author);

// Order-insensitive identity for one person, at most AUTHOR_KEY_MAX_BYTES.
//
// Drops bracketed spans and everything after ';' (multi-author separator), folds,
// drops single-character tokens (initials), sorts the remaining tokens and joins
// them. "Lu, Xun", "Xun, Lu" and "Lu Xun [Xun, Lu]" all
// collapse to one key. Truncation is on BYTES, not a token boundary: the sort
// puts a short forename first, so a whole-token cut would reduce
// "Wollstonecraft, Mary" to "alex" and merge every Alex in the library; the byte
// cut keeps "mary wollsto", still a prefix of the full key.
std::string authorKey(std::string_view author);

// Whether the filename's title segment should win over the OPF `dc:title`.
//
// True only when dc:title is a word-boundary prefix of the filename title AND
// the filename adds at least two further words of three or more letters AND its
// last word is not a stop word. The last condition is what blocks an exporter's
// mid-phrase truncation ("...a new science of") from replacing a complete title.
bool preferFilenameTitle(std::string_view dcTitle, std::string_view fnTitle);

// Does a book match what has been typed so far?
//
// Both sides are already folded — accents stripped, case dropped, punctuation
// turned to spaces — so "eneide" finds "L'Énéide" and "eluard" finds
// "Éluard". `haystack` is the record's stored fold; `needle` is the query put
// through the same fold.
//
// Every query word must PREFIX some word of the book. That is the rule that fits
// the hardware: with no partial refresh, each keypress costs a full ~185 ms panel
// repaint, so the reader wants to stop typing as early as possible. "dar mat"
// — six keys — finds "Wuthering Heights", where a plain substring test would demand the
// whole of one word and give nothing for the effort of a second.
//
// An empty query matches everything, so the list is the unfiltered shelf before
// the first key is pressed.
bool matchesQuery(std::string_view haystack, std::string_view needle);

// Ordering key for a shelf sorted by author: surname first, then the rest.
// "Herman Melville" becomes "melville herman", so the shelf reads C where a library
// would put it.
//
// Deliberately NOT the same key as authorKey(). That one sorts a name's words so
// that "Victor Hugo" and "Hugo Victor" hash alike and are recognised as one person;
// it is a GROUPING key and would be wrong to order by. This is derived from the
// DISPLAY name instead, which is safe because the spelling vote has already made
// every book by one author show the same name — so a group cannot split across
// two places on the shelf.
//
// The last word is taken as the surname. That is right for the western names on
// this card and wrong for some others, which is a limit worth stating rather than
// hiding: a single word name simply keys on itself.
std::string surnameKey(std::string_view displayAuthor);

}  // namespace library
