from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def read(rel: str) -> str:
    path = ROOT / rel
    assert path.exists(), f"Missing expected file: {rel}"
    return path.read_text(encoding="utf-8")


def assert_contains(rel: str, *needles: str) -> None:
    text = read(rel)
    for needle in needles:
        assert needle in text, f"{rel} is missing {needle!r}"


def assert_not_contains(rel: str, *needles: str) -> None:
    text = read(rel)
    for needle in needles:
        assert needle not in text, f"{rel} must not contain {needle!r}"


def main() -> None:
    assert_contains(
        "lib/Epub/Epub/IncrementalSectionTypes.h",
        "CACHE_MAGIC = 0x43504953U",
        "PAGE_INDEX_RECORD_SIZE = 16",
        "focusReadingEnabled",
        "pathsForSection",
        '".met"',
        '".pag"',
        '".idx"',
        '".anc"',
    )

    assert_contains(
        "lib/Epub/Epub/IncrementalSectionCache.h",
        "struct PageIndexRecord",
        "uint32_t pageOffset",
        "uint32_t pageLength",
        "uint16_t paragraphIndex",
        "uint16_t listItemIndex",
        "uint32_t sourceByteOffset",
        "static_assert(sizeof(PageIndexRecord) == PAGE_INDEX_RECORD_SIZE",
        "getPageForListItemIndex",
    )

    assert_contains(
        "lib/Epub/Epub/IncrementalSectionBuilder.h",
        "IncrementalBuildState",
        "DeferredLowMemory",
        "IncrementalBuildOptions",
        "focusReadingEnabled",
        "pump(const IncrementalBuildBudget& budget)",
    )

    assert_contains(
        "lib/Epub/Epub/SectionHandle.h",
        "SectionHandleMode",
        "openOrCreate",
        "knownPageCount",
        "finalPageCount",
        "getPageForListItemIndex",
        "knownPageCount_",
    )

    section_handle_cpp = read("lib/Epub/Epub/SectionHandle.cpp")
    assert "return pageNumber < knownPageCount_;" in section_handle_cpp
    assert "incrementalCache_->hasPage" not in section_handle_cpp
    assert "return knownPageCount_;" in section_handle_cpp

    builder_cpp = read("lib/Epub/Epub/IncrementalSectionBuilder.cpp")
    assert "const uint32_t pageNumber = knownPageCount_;" in builder_cpp
    assert "knownPageCount_ = pageNumber + 1;" in builder_cpp
    assert "return pageNumber < knownPageCount_;" in builder_cpp
    assert "uint32_t IncrementalSectionBuilder::knownPageCount() const { return knownPageCount_; }" in builder_cpp

    cache_cpp = read("lib/Epub/Epub/IncrementalSectionCache.cpp")
    assert "auto page = Page::deserialize(pages);" in cache_cpp
    assert "pages.close();" in cache_cpp
    assert "const auto pageCount = knownPageCountFromIndexBytes(index.size());" in cache_cpp
    assert "index.close();" in cache_cpp

    assert_contains(
        "src/activities/reader/EpubReaderUtils.h",
        "f.close();",
    )

    assert_contains(
        "lib/Epub/Epub/parsers/ChapterHtmlSlimParser.h",
        "enum class ParsePumpStatus",
        "struct ParsePumpBudget",
        "bool begin(",
        "ParsePumpStatus pump(",
        "bool finish()",
        "void close()",
        "bool parseAndBuildPages()",
        "focusReadingEnabled",
    )

    assert_contains(
        "src/activities/reader/ReaderProgressPolicy.h",
        "namespace ReaderProgressPolicy",
        "persistablePageCount",
        "decideResumeRemap",
        "deferUntilComplete",
    )

    assert_contains(
        "src/activities/reader/EpubReaderActivity.h",
        "std::unique_ptr<SectionHandle> section",
        "std::unique_ptr<SectionHandle> nextSectionPrewarm",
        "bool backgroundIndexingWorkActive = false",
        "int pendingPageTurnDirection = 0",
        "bool hasCurrentIndexingWork() const",
        "bool hasPrewarmIndexingWork() const",
        "bool hasBackgroundIndexingWork() const",
        "bool pumpBackgroundIndexing()",
        "bool skipLoopDelay() override",
        "bool preventAutoSleep() override",
    )

    assert_contains(
        "src/activities/reader/EpubReaderActivity.cpp",
        "SectionHandle::openOrCreate",
        "pumpSectionUntil",
        "maintainPrewarmWindow",
        "responsiveBackgroundBudget",
        "maxCompletedPages = 1",
        "indexBatchMaxMillis(1)",
        "bool EpubReaderActivity::hasCurrentIndexingWork() const",
        "bool EpubReaderActivity::hasPrewarmIndexingWork() const",
        "bool EpubReaderActivity::hasBackgroundIndexingWork() const",
        "bool EpubReaderActivity::hasBackgroundIndexingWork() const {\n  if (RenderLock::peek()) {",
        "return backgroundIndexingWorkActive;",
        "bool EpubReaderActivity::skipLoopDelay()",
        "bool EpubReaderActivity::preventAutoSleep()",
        "bool EpubReaderActivity::pumpBackgroundIndexing()",
        "if (pendingPageTurnDirection != 0 && !RenderLock::peek())",
        "const bool queuedForwardTurn = pendingPageTurnDirection > 0;",
        "pendingPageTurnDirection = 0;",
        "Loop-side background indexing waits for render ownership to clear",
        "if (!prevTriggered && !nextTriggered) {\n    // Loop-side background indexing waits for render ownership to clear.\n    if (!RenderLock::peek()) {\n      pumpBackgroundIndexing();\n    }\n    return;\n  }",
        "backgroundIndexingWorkActive = false;",
        "backgroundIndexingWorkActive = currentNowActive || prewarmNowActive;",
        "const bool currentWasActive",
        "const bool prewarmWasActive",
        "requestUpdate();",
        "knownPageCount()",
        "finalPageCount()",
        "loadPage(",
        "Queued page turn while render/indexing is active",
        "renderer.copyGrayscaleLsbBuffers();",
        "renderer.copyGrayscaleMsbBuffers();",
        "renderer.displayGrayBuffer();",
        "Skipping EPUB text grayscale AA: failed to store BW buffer",
        "persistablePageCountForSection",
        "ReaderProgressPolicy::decideResumeRemap",
    )

    assert_not_contains(
        "src/activities/reader/EpubReaderActivity.cpp",
        "Background render/indexing owns section state; keep loop-side pumps out until it releases the lock.",
        "  pumpBackgroundIndexing();\n  saveProgress",
        "Ignoring page turn while render/indexing is active",
        "saveProgress(currentSpineIndex, section->currentPage, static_cast<int>(displayPageCount(section.get())))",
    )

    assert_contains(
        "src/components/themes/StatusPageInfo.h",
        "struct StatusPageInfo",
        "formatStatusPageText",
        "shouldDrawChapterProgressBar",
        "shouldDrawCurrentIndexingIndicator",
        "shouldDrawFutureIndexingIndicator",
    )

    assert_contains(
        "src/activities/reader/KOReaderSyncActivity.cpp",
        "IncrementalSection::Cache incrementalCache",
        "incrementalCache.isComplete()",
        "incrementalCache.getPageForListItemIndex",
        "incrementalCache.getPageForAnchor",
        "incrementalCache.getPageForParagraphIndex",
        "Section tempSection",
    )

    assert_contains(
        "lib/FsHelpers/FsHelpers.h",
        "removeDirRecursive",
    )

    assert_contains(
        "lib/FsHelpers/FsHelpers.cpp",
        "bool removeDirRecursive",
        "entry.close();",
        "removeFileWithRetry(childPath)",
        "removeDirectoryWithRetry(path)",
        "Storage.rmdir(path)",
    )

    assert_contains(
        "test/incremental_section/CleanScopeContractTest.sh",
        "INCREMENTAL_SECTION_BASE_REF",
        "origin/master",
        "merge-base",
        "committed_changed_paths",
        "working_changed_paths",
        "inspect_sdk_range",
        "git -C \"$ROOT_DIR/open-x4-sdk\" diff --name-only",
    )

    assert_contains(
        "lib/Epub/Epub/IncrementalSectionCache.cpp",
        "checkedWritePod",
        "checkedWriteString",
        "checkedWriteLayoutKey",
        "checkedWriteIndexRecord",
        "Short write",
    )
    cache_cpp = read("lib/Epub/Epub/IncrementalSectionCache.cpp")
    cache_write_section = cache_cpp[: cache_cpp.index("void readLayoutKey")]
    assert "serialization::writePod" not in cache_write_section
    assert "serialization::writeString" not in cache_write_section

    assert_not_contains(
        "src/activities/reader/EpubReaderActivity.cpp",
        "storeGrayMaskBuffer",
        "displayFactoryGrayBufferFromStoredBwAndGrayMasks",
        "displayFactoryGrayBuffer",
        'aa=%s',
    )

    assert_contains(
        "src/activities/reader/ReaderUtils.h",
        "renderer.copyGrayscaleLsbBuffers();",
        "renderer.copyGrayscaleMsbBuffers();",
        "renderer.displayGrayBuffer();",
    )

    assert_not_contains(
        "src/activities/reader/ReaderUtils.h",
        "storeGrayMaskBuffer",
        "displayFactoryGrayBufferFromStoredBwAndGrayMasks",
        "displayFactoryGrayBuffer",
    )

    assert_not_contains(
        "lib/GfxRenderer/GfxRenderer.h",
        "storeGrayMaskBuffer",
        "displayFactoryGrayBufferFromStoredBwAndGrayMasks",
        "grayMaskBufferChunks",
    )

    assert_not_contains(
        "lib/hal/HalDisplay.h",
        "displayFactoryGrayBuffer",
    )

    display_cpp = read("open-x4-sdk/libs/display/EInkDisplay/src/EInkDisplay.cpp")
    cleanup_start = display_cpp.index("void EInkDisplay::cleanupGrayscaleBuffers")
    cleanup_end = display_cpp.index("void EInkDisplay::displayBuffer", cleanup_start)
    cleanup_body = display_cpp[cleanup_start:cleanup_end]
    assert (
        "inGrayscaleMode = false;" in cleanup_body
    ), "cleanupGrayscaleBuffers must clear grayscale mode after syncing controller RAM"
    assert (
        cleanup_body.rfind("inGrayscaleMode = false;")
        > cleanup_body.rfind("writeRamBuffer(CMD_WRITE_RAM_RED, bwBuffer, bufferSize);")
    ), "cleanupGrayscaleBuffers should clear grayscale mode after the non-X3 RED RAM sync"

    assert_contains(
        "src/activities/settings/ClearCacheActivity.cpp",
        "FsHelpers::removeDirRecursive",
    )

    assert_contains(
        "lib/Epub/Epub.cpp",
        "FsHelpers::removeDirRecursive(cachePath.c_str())",
    )


if __name__ == "__main__":
    main()
