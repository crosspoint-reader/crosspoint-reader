from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(rel: str) -> str:
    path = ROOT / rel
    assert path.exists(), f"Missing expected file: {rel}"
    return path.read_text(encoding="utf-8")


def assert_contains(rel: str, *needles: str) -> None:
    text = read(rel)
    for needle in needles:
        assert needle in text, f"{rel} is missing {needle!r}"


def slice_between(text: str, start: str, end: str) -> str:
    start_idx = text.index(start)
    end_idx = text.index(end, start_idx)
    return text[start_idx:end_idx]


def main() -> None:
    assert_contains(
        "src/CrossPointSettings.h",
        "uint8_t skipCoverOnBookEntry = 1;",
    )
    assert_contains(
        "src/SettingsList.h",
        "SettingInfo::Toggle(StrId::STR_SKIP_COVER_ON_BOOK_ENTRY, &CrossPointSettings::skipCoverOnBookEntry",
        '"skipCoverOnBookEntry"',
        "StrId::STR_CAT_READER",
    )
    assert_contains(
        "lib/I18n/translations/english.yaml",
        'STR_SKIP_COVER_ON_BOOK_ENTRY: "Skip covers on book entry"',
    )

    assert_contains(
        "docs/file-formats.md",
        "#define EXPECTED_VERSION 6",
        "String coverPageHref",
    )
    assert_contains(
        "lib/Epub/Epub/BookMetadataCache.h",
        "std::string coverPageHref;",
    )
    assert_contains(
        "lib/Epub/Epub/BookMetadataCache.cpp",
        "constexpr uint8_t BOOK_CACHE_VERSION = 6;",
        "metadata.coverPageHref.size()",
        "serialization::writeString(bookFile, metadata.coverPageHref);",
        "serialization::readString(bookFile, coreMetadata.coverPageHref);",
    )

    assert_contains(
        "lib/Epub/Epub.cpp",
        "bookMetadata.coverPageHref = opfParser.guideCoverPageHref;",
        "bool Epub::isCoverWrapperSpine",
        "int Epub::getFreshEntrySpineIndex(bool skipCoverOnBookEntry) const",
        "Fresh entry spine:",
        "text/start reference",
        "Cover page at spine 0; skipping to spine 1",
        "Cover wrapper at spine 0; skipping to spine 1",
        "single-spine book",
    )
    assert_contains(
        "lib/Epub/Epub.h",
        "bool isCoverWrapperSpine(int spineIndex) const;",
        "int getFreshEntrySpineIndex(bool skipCoverOnBookEntry) const;",
    )
    assert_contains(
        "lib/Epub/Epub/parsers/ContentOpfParser.cpp",
        'type == "text" || (type == "start" && self->textReferenceHref.empty())',
    )

    epub_reader_cpp = read("src/activities/reader/EpubReaderActivity.cpp")
    on_enter = slice_between(epub_reader_cpp, "void EpubReaderActivity::onEnter()", "void EpubReaderActivity::onExit()")
    for needle in (
        "bool loadedSavedProgress = false;",
        "bool savedProgressAtBookStart = false;",
        "loadedSavedProgress = true;",
        "savedProgressAtBookStart = currentSpineIndex == 0 && nextPageNumber == 0;",
        "const bool freshBookEntry = !loadedSavedProgress || savedProgressAtBookStart;",
        "if (SETTINGS.skipCoverOnBookEntry && freshBookEntry)",
        "epub->getFreshEntrySpineIndex(SETTINGS.skipCoverOnBookEntry)",
        "Treating saved 0,0 progress as cover-start progress",
        "cachedChapterTotalPageCount = 0;",
    ):
        assert needle in on_enter, needle


if __name__ == "__main__":
    main()
