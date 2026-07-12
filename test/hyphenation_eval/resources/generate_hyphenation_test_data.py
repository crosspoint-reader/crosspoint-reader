"""
Generate hyphenation test data from one or more text or EPUB files.

This script extracts unique words from books and generates ground truth
hyphenations using the pyphen library, which can be used to test and validate
the hyphenation implementations (e.g., German, English, Russian).

Usage:
    python generate_hyphenation_test_data.py <input_file> <output_file>
        [--additional-input <input_file>] [--language de_DE]
        [--max-words 5000] [--min-prefix 2] [--min-suffix 2] [--casefold]

Requirements:
    pip install pyphen
"""

import argparse
import re
from collections import Counter
from fractions import Fraction
from html.parser import HTMLParser
from pathlib import Path
from typing import ClassVar
import zipfile


GUTENBERG_START_RE = re.compile(
    r"\*{3}\s*START OF (?:THIS|THE) PROJECT GUTENBERG EBOOK.*?\*{3}",
    flags=re.IGNORECASE,
)
GUTENBERG_END_RE = re.compile(
    r"\*{3}\s*END OF (?:THIS|THE) PROJECT GUTENBERG EBOOK.*?\*{3}",
    flags=re.IGNORECASE,
)
LEGACY_GUTENBERG_END_RE = re.compile(
    r"\bEnd of (?:the )?Project Gutenberg", flags=re.IGNORECASE
)


class EpubTextExtractor(HTMLParser):
    """Extract visible book text while excluding EPUB navigation and boilerplate."""

    SKIPPED_TAGS: ClassVar[set[str]] = {"head", "nav", "script", "style"}
    VOID_TAGS: ClassVar[set[str]] = {
        "area",
        "base",
        "br",
        "col",
        "embed",
        "hr",
        "img",
        "input",
        "link",
        "meta",
        "param",
        "source",
        "track",
        "wbr",
    }

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.parts = []
        self.skip_depth = 0

    def handle_starttag(self, tag, attrs):
        if tag in self.VOID_TAGS:
            return

        attributes = dict(attrs)
        classes = set(attributes.get("class", "").split())
        should_skip = (
            tag in self.SKIPPED_TAGS
            or "pg-boilerplate" in classes
            or attributes.get("id") in {"pg-header", "pg-footer"}
        )
        if self.skip_depth or should_skip:
            self.skip_depth += 1

    def handle_startendtag(self, _tag, _attrs):
        return

    def handle_endtag(self, _tag):
        if self.skip_depth:
            self.skip_depth -= 1

    def handle_data(self, data):
        if not self.skip_depth:
            self.parts.append(data)

    def text(self):
        return " ".join(self.parts)


def strip_gutenberg_boilerplate(text):
    """Keep only text between Project Gutenberg's book boundary markers."""
    start = GUTENBERG_START_RE.search(text)
    if start:
        text = text[start.end() :]

    end = GUTENBERG_END_RE.search(text)
    if end:
        return text[: end.start()]

    legacy_end = LEGACY_GUTENBERG_END_RE.search(text)
    if legacy_end:
        return text[: legacy_end.start()]

    return text


def extract_text_from_epub(epub_path):
    """Extract visible book text from an EPUB archive."""
    texts = []
    with zipfile.ZipFile(epub_path, "r") as z:
        for name in sorted(z.namelist()):
            lower = name.lower()
            if (
                lower.endswith(".xhtml")
                or lower.endswith(".html")
                or lower.endswith(".htm")
            ):
                data = z.read(name).decode("utf-8", errors="ignore")
                extractor = EpubTextExtractor()
                extractor.feed(data)
                text = strip_gutenberg_boilerplate(extractor.text())
                if text.strip():
                    texts.append(text)
    return "\n".join(texts)


def extract_words(text):
    """Extract all words from text, preserving original case."""
    # Match runs of Unicode letters (any script) while excluding digits/underscores
    return re.findall(r"[^\W\d_]+", text, flags=re.UNICODE)


def clean_word(word):
    """Normalize word for hyphenation testing."""
    # Keep original case but strip any non-letter characters
    return word.strip()


def read_source(input_file):
    """Read book text from a UTF-8 text file or EPUB archive."""
    if str(input_file).lower().endswith(".epub"):
        print("  Detected .epub input; extracting visible HTML content")
        return extract_text_from_epub(input_file)

    with open(input_file, "r", encoding="utf-8") as f:
        return strip_gutenberg_boilerplate(f.read())


def generate_hyphenation_data(
    input_files,
    output_file,
    language="de_DE",
    min_length=6,
    max_words=5000,
    min_prefix=2,
    min_suffix=2,
    casefold=False,
):
    """
    Generate hyphenation test data from text or EPUB files.

    Args:
        input_files: Paths to input text or EPUB files
        output_file: Path to output file with hyphenation data
        language: Language code for pyphen (e.g., 'de_DE', 'en_US')
        min_length: Minimum word length to include
        max_words: Maximum number of words to include (default: 5000)
        min_prefix: Minimum characters allowed before the first hyphen (default: 2)
        min_suffix: Minimum characters allowed after the last hyphen (default: 2)
        casefold: Normalize words with Unicode case folding before counting
    """
    import pyphen

    source_paths = [Path(input_file) for input_file in input_files]
    word_counts = Counter()
    ranking_scores = Counter()

    for input_file in source_paths:
        print(f"Reading from: {input_file}")
        words = extract_words(read_source(input_file))
        if casefold:
            words = [word.casefold() for word in words]

        source_counts = Counter(words)
        source_word_total = sum(source_counts.values())
        if not source_word_total:
            raise ValueError(f"No words found in source: {input_file}")

        print(
            f"  Found {source_word_total} total words and "
            f"{len(source_counts)} unique words"
        )
        word_counts.update(source_counts)
        for word, count in source_counts.items():
            ranking_scores[word] += Fraction(count, source_word_total)

    print(f"Found {sum(word_counts.values())} total words")
    print(f"Found {len(word_counts)} unique words")

    # Initialize pyphen hyphenator
    print(
        f"Initializing hyphenator for language: {language} (min_prefix={min_prefix}, min_suffix={min_suffix})"
    )
    try:
        hyphenator = pyphen.Pyphen(lang=language, left=min_prefix, right=min_suffix)
    except KeyError:
        print(f"Error: Language '{language}' not found in pyphen.")
        print("Available languages include: de_DE, en_US, en_GB, fr_FR, etc.")
        return

    # Generate hyphenations
    print("Generating hyphenations...")
    hyphenation_data = []

    # Each source contributes equal weight so a long book cannot dominate the
    # selected vocabulary. Aggregate counts remain in the output for context.
    sorted_words = sorted(
        word_counts.items(),
        key=lambda item: (
            -ranking_scores[item[0]],
            -item[1],
            item[0].lower(),
            item[0],
        ),
    )

    for word, count in sorted_words:
        # Filter by minimum length
        if len(word) < min_length:
            continue

        # Get hyphenation (may produce no '=' characters)
        hyphenated = hyphenator.inserted(word, hyphen="=")

        # Include all words (so we can take the top N most common words even if
        # they don't have hyphenation points). This replaces the previous filter
        # which dropped words without '='.
        hyphenation_data.append(
            {"word": word, "hyphenated": hyphenated, "count": count}
        )

        # Stop if we've reached max_words
        if max_words and len(hyphenation_data) >= max_words:
            break

    print(f"Generated {len(hyphenation_data)} hyphenated words")

    # Write output file
    print(f"Writing to: {output_file}")
    with open(output_file, "w", encoding="utf-8") as f:
        # Write header with metadata
        f.write("# Hyphenation Test Data\n")
        if len(source_paths) == 1:
            f.write(f"# Source: {source_paths[0].name}\n")
        else:
            f.write("# Sources:\n")
            for source_path in source_paths:
                f.write(f"# - {source_path.name}\n")
            f.write("# Ranking: equal-weight normalized frequency across sources\n")
        if casefold:
            f.write("# Case: Unicode casefolded\n")
        f.write(f"# Language: {language}\n")
        f.write(f"# Min prefix: {min_prefix}\n")
        f.write(f"# Min suffix: {min_suffix}\n")
        f.write(f"# Total words: {len(hyphenation_data)}\n")
        f.write("# Format: word | hyphenated_form | frequency_in_sources\n")
        f.write(f"#\n")
        f.write(f"# Hyphenation points are marked with '='\n")
        f.write(f"# Example: Silbentrennung -> Sil=ben=tren=nung\n")
        f.write(f"#\n\n")

        # Write data
        for item in hyphenation_data:
            f.write(f"{item['word']}|{item['hyphenated']}|{item['count']}\n")

    print("Done!")

    # Print some statistics
    print("\n=== Statistics ===")
    print(f"Total unique words extracted: {len(word_counts)}")
    print(f"Words with hyphenation points: {len(hyphenation_data)}")
    print(
        f"Average hyphenation points per word: {sum(h['hyphenated'].count('=') for h in hyphenation_data) / len(hyphenation_data):.2f}"
    )

    # Print some examples
    print("\n=== Examples (first 10) ===")
    for item in hyphenation_data[:10]:
        print(
            f"  {item['word']:20} -> {item['hyphenated']:30} (appears {item['count']}x)"
        )


def main():
    parser = argparse.ArgumentParser(
        description="Generate hyphenation test data from text or EPUB files",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Generate test data from a German book
  python generate_hyphenation_test_data.py ../data/books/bobiverse_1.txt hyphenation_test_data.txt
  
  # Limit to 500 most common words
  python generate_hyphenation_test_data.py ../data/books/bobiverse_1.txt hyphenation_test_data.txt --max-words 500

  # Balance vocabulary across multiple Catalan EPUBs and normalize case
  python generate_hyphenation_test_data.py book1.epub test_ca.txt --additional-input book2.epub --language ca --casefold
  
  # Use English hyphenation (when available)
  python generate_hyphenation_test_data.py book.txt test_en.txt --language en_US
        """,
    )

    parser.add_argument("input_file", help="Input text file to extract words from")
    parser.add_argument("output_file", help="Output file for hyphenation test data")
    parser.add_argument(
        "--additional-input",
        action="append",
        default=[],
        help="Additional text or EPUB input; may be repeated",
    )
    parser.add_argument(
        "--language", default="de_DE", help="Language code (default: de_DE)"
    )
    parser.add_argument(
        "--min-length", type=int, default=6, help="Minimum word length (default: 6)"
    )
    parser.add_argument(
        "--max-words",
        type=int,
        default=5000,
        help="Maximum number of words to include (default: 5000)",
    )
    parser.add_argument(
        "--min-prefix",
        type=int,
        default=2,
        help="Minimum characters permitted before the first hyphen (default: 2)",
    )
    parser.add_argument(
        "--min-suffix",
        type=int,
        default=2,
        help="Minimum characters permitted after the last hyphen (default: 2)",
    )
    parser.add_argument(
        "--casefold",
        action="store_true",
        help="Normalize words with Unicode case folding before counting",
    )

    args = parser.parse_args()

    generate_hyphenation_data(
        [args.input_file, *args.additional_input],
        args.output_file,
        language=args.language,
        min_length=args.min_length,
        max_words=args.max_words,
        min_prefix=args.min_prefix,
        min_suffix=args.min_suffix,
        casefold=args.casefold,
    )


if __name__ == "__main__":
    main()
