"""
Generate hyphenation test data from a text file.

This script extracts unique words from a book and generates ground truth
hyphenations using the pyphen library, which can be used to test and validate
the hyphenation implementations (e.g., German, English, Russian).

Usage:
    python generate_hyphenation_test_data.py <input_file> <output_file>
        [--language de_DE] [--max-words 5000] [--min-prefix 2] [--min-suffix 2]

Requirements:
    pip install pyphen
"""

import argparse
import re
from collections import Counter
import pyphen
from pathlib import Path
import zipfile


def extract_text_from_epub(epub_path):
    """Extract textual content from an .epub archive by concatenating HTML/XHTML files."""
    texts = []
    with zipfile.ZipFile(epub_path, "r") as z:
        for name in z.namelist():
            lower = name.lower()
            if (
                lower.endswith(".xhtml")
                or lower.endswith(".html")
                or lower.endswith(".htm")
            ):
                try:
                    data = z.read(name).decode("utf-8", errors="ignore")
                except Exception:
                    continue
                # Remove tags
                text = re.sub(r"<[^>]+>", " ", data)
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


def generate_hyphenation_data(
    input_file,
    output_file,
    language="de_DE",
    min_length=6,
    max_words=5000,
    min_prefix=2,
    min_suffix=2,
):
    """
    Generate hyphenation test data from a text file.

    Args:
        input_file: Path to input text file
        output_file: Path to output file with hyphenation data
        language: Language code for pyphen (e.g., 'de_DE', 'en_US')
        min_length: Minimum word length to include
        max_words: Maximum number of words to include (default: 5000)
        min_prefix: Minimum characters allowed before the first hyphen (default: 2)
        min_suffix: Minimum characters allowed after the last hyphen (default: 2)
    """
    print(f"Reading from: {input_file}")

    # Read the input file
    if str(input_file).lower().endswith(".epub"):
        print("Detected .epub input; extracting HTML content")
        text = extract_text_from_epub(input_file)
    else:
        with open(input_file, "r", encoding="utf-8") as f:
            text = f.read()

    # Extract words
    print("Extracting words...")
    words = extract_words(text)
    print(f"Found {len(words)} total words")

    # Count word frequencies
    word_counts = Counter(words)
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

    # Sort by frequency (most common first) then alphabetically
    sorted_words = sorted(word_counts.items(), key=lambda x: (-x[1], x[0].lower()))

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
        f.write(f"# Hyphenation Test Data\n")
        f.write(f"# Source: {Path(input_file).name}\n")
        f.write(f"# Language: {language}\n")
        f.write(f"# Min prefix: {min_prefix}\n")
        f.write(f"# Min suffix: {min_suffix}\n")
        f.write(f"# Total words: {len(hyphenation_data)}\n")
        f.write(f"# Format: word | hyphenated_form | frequency_in_source\n")
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
        description="Generate hyphenation test data from a text file",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Generate test data from a German book
  python generate_hyphenation_test_data.py ../data/books/bobiverse_1.txt hyphenation_test_data.txt
  
  # Limit to 500 most common words
  python generate_hyphenation_test_data.py ../data/books/bobiverse_1.txt hyphenation_test_data.txt --max-words 500
  
  # Use English hyphenation (when available)
  python generate_hyphenation_test_data.py book.txt test_en.txt --language en_US
        """,
    )

    parser.add_argument("input_file", help="Input text file to extract words from")
    parser.add_argument("output_file", help="Output file for hyphenation test data")
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

    args = parser.parse_args()

    generate_hyphenation_data(
        args.input_file,
        args.output_file,
        language=args.language,
        min_length=args.min_length,
        max_words=args.max_words,
        min_prefix=args.min_prefix,
        min_suffix=args.min_suffix,
    )


if __name__ == "__main__":
    main()
