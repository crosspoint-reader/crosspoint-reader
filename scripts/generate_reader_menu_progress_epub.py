#!/usr/bin/env python3
"""
Generate test EPUB for the reader menu's chapter/book progress line (#3434).

The menu header reads "Chapter: X/Y pages | Book: N%". Opening a sub-screen
(Select Chapter, Text Settings) releases the current section to free RAM; on
return the header must show the same values, and "Go to %" must open at the
same book percentage.

Chapter 1 is a few pages, so its layout finalizes as soon as it opens: after
returning from a sub-screen the header must be identical. Chapter 2 is long
enough that layout is still running while it is read: while that is the case
the total after returning may be lower than before, and it must match once
the whole chapter is laid out. Chapter 3 keeps the book percentage below 100.

Verification instructions are embedded as the first paragraph of each chapter
so a tester can confirm the expected result on device or in the simulator.

Reuses create_epub/make_chapter from generate_dictionary_synonyms_test_epub.py.
"""

import random

from generate_dictionary_synonyms_test_epub import OUTPUT_DIR, create_epub, make_chapter

WORDS = (
    "reader menu chapter page progress section cache header band battery "
    "portrait landscape margin serif glyph render buffer flash heap stack "
    "spine anchor offset percent total index fallback release restore "
    "quiet steady narrow bright faint slow careful plain small whole"
).split()


def prose(seed, words):
    """Deterministic filler prose of roughly `words` words."""
    rng = random.Random(seed)
    paragraphs = []
    remaining = words
    while remaining > 0:
        count = min(remaining, rng.randint(55, 85))
        sentences, sentence = [], []
        for _ in range(count):
            sentence.append(rng.choice(WORDS))
            if len(sentence) >= rng.randint(7, 14):
                sentences.append(' '.join(sentence).capitalize() + '.')
                sentence = []
        if sentence:
            sentences.append(' '.join(sentence).capitalize() + '.')
        paragraphs.append('<p>' + ' '.join(sentences) + '</p>')
        remaining -= count
    return '\n'.join(paragraphs)


if __name__ == '__main__':
    chapters = [
        ("Short Chapter", make_chapter("Short Chapter",
            "<p><b>Check:</b> turn two pages, open the reader menu and note the Chapter and "
            "Book values. Select Chapter, then Back: the values must be unchanged. Repeat with "
            "Text Settings. Go to % must open at the Book value.</p>\n" + prose(3434, 400))),
        ("Long Chapter", make_chapter("Long Chapter",
            "<p><b>Check:</b> this chapter is still being laid out while you read it. Turn ten "
            "pages, open the reader menu, note the Chapter total, Select Chapter, then Back. "
            "While layout is still running the total may be lower than before; once the whole "
            "chapter is laid out it must match.</p>\n" + prose(3435, 3000))),
        ("Closing", make_chapter("Closing", prose(3436, 150))),
    ]

    output_file = OUTPUT_DIR / 'test_reader_menu_progress.epub'
    create_epub(output_file, 'Reader Menu Progress', chapters)
    print(f"Created: {output_file}")
