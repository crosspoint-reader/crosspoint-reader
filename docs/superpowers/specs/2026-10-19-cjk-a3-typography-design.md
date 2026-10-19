# CJK typography (PR A3) and per-language text rules: design

Status (2026-10-19): draft for review.

Working material, like every spec here: when the work merges, lasting parts move to reference docs, open
items to `docs/future_work/cjk.md`, and this file is deleted.

## 1. Why

CJK books now break lines correctly (A2, #497): no hyphens, kinsoku, Korean words whole. What they still lack
is the spacing a CJK typesetter would give them. The maintainer does not read CJK, so the bar is not "looks
right to us" but the published layout requirements:
- **JLREQ**, Requirements for Japanese Text Layout (W3C);
- **CLREQ**, Requirements for Chinese Text Layout (W3C);
- **KLREQ**, Requirements for Hangul Text Layout and Typography (W3C).

**The gaps, against those documents.** Upstream crosspoint-reader (checked at `upstream/develop`
261726f6, 2026-10-19) has kinsoku and basic ruby, and none of the rest:

| What the requirements describe | Upstream | KOReader | Here, after A3 |
|---|---|---|---|
| Full-width punctuation set half-width at line edges and in runs | no | yes, one table per language | yes, one table per language |
| Justified CJK lines that end flush | no, ragged by up to one glyph | yes, but the remainder is lost or biased | yes, remainder spread evenly |
| Push-in: shrink punctuation on a line before breaking it earlier | no | only to avoid a one-glyph hole | yes, in every breaker |
| A gap between CJK and Latin letters or digits | no | ¼ em, fixed | ¼ em, shrinks and stretches with the line |
| Characters on a whole-em grid | no | opt-in style tweak | on for Chinese and Japanese books |
| Rules chosen per language from one place | no | yes (`TextLangCfg`) | yes, the typography profile (section 3) |

## 2. Decided

By the maintainer, in conversation on 2026-10-19:
- **A3 is "CJK punctuation and spacing":** the planned justification, squeeze and grid, plus push-in, the
  per-language punctuation tables and the CJK–Latin gap.
- **Ruby's extras stay with ruby (PR B):** emphasis dots (傍点) and proper ruby spacing (section 9).
- **The book's language is enough.** `lang` attributes inside a book are not followed in this round.
- **One generic routine decides the typographic rules per language**, and text direction belongs in it even
  though only left-to-right horizontal text is implemented now.

## 3. The typography profile

**What it is.** One read-only description of how a language's text is laid out, chosen once from the book's
language tag. Layout code asks the profile instead of testing for "is this a CJK book" in several places, so a
new language, or a correction for an existing one, changes one table entry.

**Fields** (the names are the builder's):
- **Hyphenation:** the patterns to use. Today's `LanguageRegistry` lookup becomes the profile's; the profile
  replaces `Hyphenator::setPreferredLanguage(epub->getLanguage())`'s role of deciding it.
- **Line breaking:** at spaces (Latin and the default); between characters (Chinese, Japanese); whole words with
  line-end syllable splits when hyphenation is on (Korean). A2's character-class rules
  (`lib/Epub/Epub/CjkLineBreak`) stay character-driven (section 5.6); the profile only says which language rules
  apply on top.
- **Justification:** at word spaces, or at the gaps between characters.
- **Punctuation table:** none, Simplified Chinese, Traditional Chinese or Japanese (section 5.2).
- **CJK–Latin gap:** on or off (section 5.4).
- **Whole-em grid:** on or off (section 5.5).
- **Direction:** see section 4.

**Selection** (my call, matching KOReader's `TextLangCfg`, which is proven on these tags):

| Book language | Profile |
|---|---|
| `ja*` | Japanese |
| `zh-Hant*`, `zh-TW`, `zh-HK`, `zh-MO`, `lzh` (Classical Chinese) | Traditional Chinese |
| any other `zh*` | Simplified Chinese |
| `ko*` | Korean |
| anything else, missing or unknown | the language's existing behaviour: Latin rules, its hyphenation patterns |

**Where it lives:** with the layout code (`lib/Epub/Epub`), resolved once per section build from
`Epub::getLanguage()` and handed to the parser and `ParsedText`. How it is passed is the builder's call; it must
not become another global that a background task could see half-updated.

## 4. Direction (data only in this round)

There are three separate questions, and they have different owners:

| Question | Owner | Values | Now |
|---|---|---|---|
| Which way characters run inside a line | the script, so the profile | left-to-right; right-to-left for Arabic and Hebrew, with Latin and digits embedded left-to-right | only left-to-right is implemented |
| Whether lines are horizontal or vertical | the book (CSS `writing-mode`); the profile only says whether vertical is legitimate | horizontal; vertical, columns right-to-left (most Japanese novels, many Traditional Chinese books); vertical left-to-right (Mongolian) | only horizontal |
| Which way pages turn | the book (OPF spine `page-progression-direction`) | left-to-right; right-to-left (vertical Japanese and Chinese, Arabic, Hebrew) | only left-to-right |

**In A3:** the profile carries the inline direction (left-to-right for every language this round) and a
"vertical allowed" flag (Japanese, Chinese). Nothing reads them yet. The reserved RTL fields in
`lib/Epub/Epub/content/CompiledContent.h` (base direction bits, per-word bidi level) are where right-to-left
text will land; the profile is where it will be decided. Parsing `writing-mode` and `page-progression-direction`
is not part of A3.

## 5. A3: what changes on the page

### 5.1 Justification

- **CJK gaps stretch** when a line is justified; the CJK–Latin gap stretches too (5.4).
- **The remainder is spread:** after `spare / gaps`, the leftover pixels go 1 px each to evenly spaced gaps, so a
  justified CJK line ends flush. Upstream's lines are ragged by up to one glyph.
- **Latin rules stay as they are:** no stretch before closing punctuation, and the `effWidth/8` cap.
- **No guide dots** at a CJK gap (decided at render time from the boundary's codepoints; nothing stored).
- **Last lines** of a paragraph stay unjustified, as today. (KOReader gives the last line the spacing of the line
  above to keep columns; out of scope, section 10.)

### 5.2 Punctuation widths, per language

- **Classes:** the JLREQ character classes for punctuation: opening bracket, closing bracket, dividing
  punctuation (！？), middle dot (・：；), full stop (。．), comma (、，), ideographic space, the ambiguous quotes
  (＂＇), and curly quotes “”‘’ when they are full-width glyphs in CJK context.
- **Each class has a width per neighbour,** in eighths of an em: a **forced** width (always applied) and a
  **minimum** width (applied only when the line needs it, section 5.3).
- **The tables follow the requirements of each language:**
  - **Simplified Chinese (CLREQ):** punctuation is half-width at a line end and an opening bracket at a line start;
    in a run of punctuation all but one are half-width (」。, ）（); a closing-then-opening pair is 6 + 6 eighths;
    elsewhere half-width is allowed when needed. This is also the table for CJK punctuation met in a non-CJK book.
  - **Traditional Chinese (CLREQ):** punctuation glyphs are centred in their em, so everything stays full-width;
    it compresses (to 6 eighths, brackets 4) only when needed.
  - **Japanese (JLREQ):** 。 stays full-width mid-line and may become half-width at a line end; a comma or closing
    bracket at a line end is half or full, nothing in between; ：； may halve; ？！ stay full-width; a quarter em is
    kept before an opening bracket and after a closing bracket.
  - **Korean:** Korean uses Western punctuation (KLREQ), so the Simplified Chinese table applies to the rare CJK
    punctuation in Korean text.
- **Licensing (my call):** the firmware is MIT, KOReader's crengine is GPL. The tables are written from the W3C
  requirements, which are free to use; crengine's tables (`textlang.cpp#L813-L972`, research copy in the
  session notes) are a cross-check, not a source to copy. This replaces the earlier plan to copy crengine's
  values.
- **Every squeezable punctuation mark is its own token** (my call). Kinsoku glues a run such as 字」。 into one
  token today, and `TextBlock` draws a token with one `drawText` at the font's advances, so a mark inside a token
  could be measured narrow but not drawn narrow. Splitting squeezable punctuation into glued tokens
  (`continues = true`: no space, no break, exactly as kinsoku needs) gives each mark its own x position.
  `TextBlock`, the section format and the render path do not change, and the rule checker reads every width from
  the page dump. The cost is more tokens, bounded by A2's 32-codepoint flush during layout and a few bytes per
  mark on a resident page.
- **Line edges vs mid-line:** an entry that depends only on the neighbour is applied to token widths before line
  breaking. An entry that depends on the break (line start, line end) is applied **by the breakers as well as by
  `extractLine`** (my call): every breaker measures a candidate line with its first and last token at their
  line-edge widths (forced entries applied, optional ones counted as shrinkable, 5.3). Otherwise a line that fits
  with a half-width 。 at its end would be broken early, and a line-edge width could exceed what the breaker
  measured. `extractLine` then applies exactly what the breaker measured, and the space freed goes to
  justification.
- **Placement:** a full-width glyph in a narrowed box sits by where its ink is: ink in the left half,
  left-aligned; in the right half, right-aligned (the token's drawing origin moves left by the width removed);
  otherwise centred. This uses the glyph metrics every font already reports (`left`, `width` against the
  advance); no per-character data.
- **An ideographic space at a paragraph start** keeps its full width.

### 5.3 Push-in before push-out

- **What it is:** JLREQ's preferred line adjustment. When the next token does not fit, or kinsoku glues it to a
  character that does not fit, the line first tries to *shrink*: mid-line punctuation down to its minimum width,
  then the CJK–Latin gaps (5.4). Only if that is not enough does the line break earlier and get *stretched*
  (push-out).
- **In every breaker:** the DP breaker (`computeLineBreaks`) and the three greedy ones
  (`computeSingleLineBreakNoHyphen`, `computeHyphenatedLineBreaks`, `computeHyphenatedLineBreaksFromIndex`). A2's
  lesson stands: a rule in one breaker only shows up as wrong lines elsewhere.
- **Fit test:** a line fits when its width with every shrinkable part at its minimum is within the available
  width. In the DP, a shrunk line costs more than an exact one and less than a badly stretched one, so it is
  chosen when it avoids a loose line, not by default. (The exact cost is the builder's; the requirement is that
  pure-CJK paragraphs with no punctuation lay out exactly as before.)
- **The order of shrinking** follows JLREQ's line adjustment priorities; `extractLine` applies the shrink it
  needs, in that order, and spreads any remainder as in 5.1.
- **KOReader** only pushes in to avoid a one-glyph hole at the line end; this is the general form.

### 5.4 The gap between CJK and Latin

- **Where:** at a boundary between a Han or kana character and a Latin letter or digit, in either order, when the
  source has no space there. Not next to punctuation, not next to an existing space, not for Hangul (Korean
  spaces its words; KLREQ asks for nothing here).
- **Width:** a quarter em of the CJK character's size. It stretches with justification and shrinks under
  push-in (5.3), within JLREQ's limits.
- **In which books:** Chinese and Japanese profiles. KOReader also applies it to Korean and to CJK runs in any
  book; this design does not (my call, KLREQ does not ask for it).
- **Not a break opportunity on its own:** A2's break rules decide where a line may end; the gap only adds width.

### 5.5 The whole-em grid

- **When:** Chinese and Japanese profiles.
- **Width:** a paragraph's text width is rounded down to a whole number of ems of its font size, and half the
  difference goes to each side.
- **Indent:** a non-zero first-line indent rounds down to whole ems, at least 1 em (my call, as KOReader). A
  paragraph with no indent keeps none: headings, centred text and paragraphs that indent with leading U+3000
  spaces (common in Chinese EPUBs) are unchanged.
- **Effect:** pure CJK lines fill exactly and characters line up in columns. Mixed lines still justify (5.1).
- **Floats** keep their own width, as in KOReader.

### 5.6 A missing or wrong book language

Many Chinese EPUBs declare `en` or nothing. **My call:** the rules that follow from the characters apply in every
book, as A2's already do: line breaking, kinsoku, the Simplified Chinese punctuation table on CJK punctuation,
and stretching CJK gaps. The rules that need the language apply only with a Chinese or Japanese profile: the
Traditional Chinese and Japanese tables, the CJK–Latin gap and the grid. No content-based language detection in
this round.

## 6. Constraints

- **No new settings.** Everything is on by default for the profiles it belongs to (YAGNI; a setting can follow if
  readers ask).
- **Heap:** no new per-token field beyond one byte; the extra punctuation tokens of 5.2 are allowed. The tables are
  a few hundred bytes of flash. A2's host measurements (25 KB arena peak, 47-57 KB heap on the CJK excerpts) must
  not grow by more than 5 %.
- **Layout cost** (my call): a section build of the CJK excerpts on the X3 may take at most 10 % longer than with
  A2, and page time must not change beyond noise (the render path does not change, 5.2). Measured before merge;
  if the DP's push-in exceeds the budget, prefix sums of the shrinkable amounts are the expected fix.
- **Section file version** bumps (layout changes). Today it is 94.
- **Latin books lay out exactly as before:** the Latin goldens in `test/epub_pipeline` must not change. The CJK
  goldens (`test_cjk_*.golden.txt`) change, and each change must be explained by a rule in section 5.
- **KOSync positions** are unchanged: this is layout only.
- **Upstream contributors** are credited as in A1/A2 where their code is the base (the porting convention).

## 7. Validation

The maintainer does not read CJK, so pass or fail rests on rules and screenshots, as in A2.

- **Host rule checker** (`test/epub_pipeline/CjkRulesTest.cpp`) gains a rule per feature:
  - **justification:** a justified line's natural width plus its gaps equals the text width exactly;
  - **punctuation:** every squeezed character has the width its profile's table gives it, by class and
    neighbour;
  - **push-in:** no line is wider than the text width; a line that shrank could not have been laid out at its
    natural width;
  - **CJK–Latin:** every Han/kana–Latin boundary without a source space carries the gap, and no other boundary
    does;
  - **grid:** in Chinese and Japanese books the text width is a whole number of ems.
- **Profile selection:** a host test of the tag table in section 3, `lzh` and the region subtags included.
- **Books:** the A2 fixture (`make_test_cjk_line_breaking.py`) gains cases for punctuation runs, line-edge
  punctuation, push-in, CJK–Latin boundaries; the three real excerpts (Soseki, Hong Lou Meng, Hyeon Jin-geon)
  run through every rule; a Traditional Chinese excerpt is added (a public-domain text tagged `zh-Hant`).
- **Device:** X3 screenshots of the excerpts, reviewed by Claude for line edges, punctuation widths, CJK–Latin
  gaps and grid alignment; the language set with `CMD:SET:language` (#510) where the UI is involved.

## 8. Left to the builder

- names, and how the profile reaches the parser and `ParsedText`;
- the exact table values, written from JLREQ/CLREQ and cross-checked against crengine;
- the DP cost of a shrunk line, within the requirement in 5.3;
- whether the squeeze class is computed on the fly or cached per token (within the heap limit in section 6);
- how many PRs A3 becomes. A natural split: the profile and the per-language tables first (no visible change for
  Latin books), then push-in and the CJK–Latin gap.

## 9. Carried to PR B (ruby)

PR B is planned from the earlier spec (`docs/superpowers/specs/2026-10-16-cjk-layout-design.md`, section 7, on
branch `docs/cjk-layout-design`). Two additions decided here:
- **Ruby spacing (JLREQ):** when ruby and base differ in length, the shorter is spread across the longer with
  the 1:2:1 spacing JLREQ describes, instead of only being centred.
- **Emphasis dots (傍点, CSS `text-emphasis`):** dots or sesame marks above each character of a span, as
  Japanese fiction uses (Aozora Bunko books mark them). They share ruby's storage and its taller line.

## 10. Not in this round

- `lang` attributes inside a book (the book's language decides);
- right-to-left text, vertical text, `writing-mode` and `page-progression-direction` parsing (section 4);
- hanging punctuation (optional in JLREQ; KOReader does not hang CJK punctuation either);
- the special indent of an opening bracket at a paragraph start;
- last-line grid continuity;
- a CJK default for line spacing (to be judged from X3 screenshots first);
- hyphenating Latin words inside CJK books;
- content-based language detection.
