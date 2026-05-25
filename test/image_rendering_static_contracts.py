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
        "lib/Epub/Epub/Page.h",
        "void renderTextOnly(GfxRenderer& renderer, int fontId, int xOffset, int yOffset) const;",
    )

    page_cpp = read("lib/Epub/Epub/Page.cpp")
    render_text_only = slice_between(page_cpp, "void Page::renderTextOnly", "bool Page::serialize")
    assert "TAG_PageLine" in render_text_only
    assert "element->render(renderer, fontId, xOffset, yOffset);" in render_text_only

    epub_reader_cpp = read("src/activities/reader/EpubReaderActivity.cpp")
    render_contents = slice_between(
        epub_reader_cpp,
        "void EpubReaderActivity::renderContents(",
        "void EpubReaderActivity::renderStatusBar()",
    )
    assert (
        "page->renderTextOnly(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);"
        in render_contents
    ), "Font prewarm scan pass must avoid image rendering"
    assert (
        render_contents.count(
            "page->renderTextOnly(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);"
        )
        >= 3
    ), "Text AA passes must not decode/render images"
    assert "page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);" in render_contents

    chapter_parser_cpp = read("lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp")
    image_branch = slice_between(
        chapter_parser_cpp,
        "if (matches(name, IMAGE_TAGS, std::size(IMAGE_TAGS)))",
        "  if (matches(name, SKIP_TAGS",
    )
    for needle in (
        'constexpr const char* IMAGE_TAGS[] = {"img", "image"};',
        '"src"',
        '"href"',
        '"xlink:href"',
        "src.substr(0, hashPos)",
        "FsHelpers::normalisePath(self->contentBase + src)",
        "std::make_shared<PageImage>",
        "Parsed image element:",
        "Resolved image:",
        "Image placeholder by reader setting",
        "Image suppressed by reader setting",
        "Image skipped by CSS display:none",
        "Unsupported image format",
    ):
        assert needle in chapter_parser_cpp if needle.startswith("constexpr") else needle in image_branch, needle

    assert_contains(
        "lib/Epub/Epub/blocks/ImageBlock.cpp",
        "Decode successful",
        "Loading from cache:",
        "Render bounds rejected:",
    )


if __name__ == "__main__":
    main()
