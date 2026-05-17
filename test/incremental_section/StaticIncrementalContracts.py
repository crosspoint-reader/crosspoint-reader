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


def slice_between(text: str, start: str, end: str) -> str:
    start_idx = text.index(start)
    end_idx = text.index(end, start_idx)
    return text[start_idx:end_idx]


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
    assert "if (state == IncrementalBuildState::Parsing)" in section_handle_cpp
    assert "return {SectionPumpStatus::Pumped, pagesBefore, pagesAfter};" in section_handle_cpp

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
    anchor_lookup_body = slice_between(
        cache_cpp,
        "std::optional<uint16_t> Cache::getPageForAnchor",
        "std::optional<uint16_t> Cache::getParagraphIndexForPage",
    )
    assert "readAnchorEntry(file, key, page)" in anchor_lookup_body
    assert "serialization::readString(file, key)" not in anchor_lookup_body
    assert "MAX_ANCHOR_KEY_LEN" in cache_cpp

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
        "bool bookCacheDeleted = false",
        "bool freshBookEntry = false",
        "bool initialBookEntryIndexingPopupShown = false",
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
    epub_reader_cpp = read("src/activities/reader/EpubReaderActivity.cpp")
    pump_background_body = slice_between(
        epub_reader_cpp,
        "bool EpubReaderActivity::pumpBackgroundIndexing()",
        "void EpubReaderActivity::loop()",
    )
    assert "backgroundIndexingWorkActive = currentNowActive || prewarmNowActive;" in pump_background_body
    assert (
        "requestUpdate();" not in pump_background_body
    ), "Background/prewarm indexing state changes must not redraw the current page"
    assert (
        "currentWasActive != currentNowActive || prewarmWasActive != prewarmNowActive" not in pump_background_body
    ), "Background/prewarm active-state transitions should not schedule UI-only repaints"
    assert "displayedSectionProgress" in epub_reader_cpp
    assert "static_cast<float>(section->currentPage) / static_cast<float>(pageCount)" not in epub_reader_cpp
    assert "calculateProgress(currentSpineIndex, displayedSectionProgress(section.get()))" in epub_reader_cpp

    activity_manager_cpp = read("src/activities/ActivityManager.cpp")
    sleep_body = slice_between(
        activity_manager_cpp,
        "void ActivityManager::goToSleep()",
        "void ActivityManager::goToBoot()",
    )
    assert "sleepTransitionPending.store(true" in sleep_body
    assert "processPendingActions();" in sleep_body
    assert "loop();" not in sleep_body, "Sleep must not run the outgoing activity loop before drawing the sleep screen"
    assert "sleepTransitionPending.store(false" in sleep_body
    assert "void ActivityManager::processPendingActions()" in activity_manager_cpp
    assert "bool isSleepTransitionPending() const" in read("src/activities/ActivityManager.h")
    activity_loop_body = slice_between(
        activity_manager_cpp,
        "void ActivityManager::loop()",
        "void ActivityManager::processPendingActions()",
    )
    assert "currentActivity->loop();" in activity_loop_body
    assert "processPendingActions();" in activity_loop_body

    assert "popupFn" not in epub_reader_cpp, "Normal EPUB reader section loads must not wire parser popup redraws"
    assert (
        epub_reader_cpp.count("GUI.drawPopup(renderer, tr(STR_INDEXING));") == 1
    ), "Indexing popup should be centralized behind a once-only helper, not drawn by first-page load"
    assert "showIndexingPopupOnce" in epub_reader_cpp
    assert "hasCompatibleCompleteIncrementalCache" in epub_reader_cpp
    assert "Section open/create:" in epub_reader_cpp
    assert "First-page pump" in epub_reader_cpp
    assert "Initial-window pump" in epub_reader_cpp
    assert "Position-resolution pump" in epub_reader_cpp
    assert "Page load:" in epub_reader_cpp
    assert "Page render/display:" in epub_reader_cpp
    pump_until_body = slice_between(
        epub_reader_cpp,
        "SectionPumpLoopResult pumpSectionUntil(",
        "bool ensureOutrunWindowAvailable(",
    )
    assert "activityManager.isSleepTransitionPending()" in pump_until_body

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
        'refineRemotePositionFromLookup(incrementalCache, remotePosition, "Cached")',
        "getPageForListItemIndex",
        "getPageForAnchor",
        "getPageForParagraphIndex",
        "refineRemoteProgressWithIncrementalIndexing",
        "SectionHandle::openOrCreate",
        "while (!refined && sectionHandle->hasActiveBuilder())",
        "sectionHandle->pump(indexBudget(IncrementalBuildBudgetProfile::Outrun))",
        'refineRemotePositionFromLookup(*sectionHandle, position, "Indexed")',
        "Section tempSection",
    )
    koreader_sync_cpp = read("src/activities/reader/KOReaderSyncActivity.cpp")
    indexing_status = koreader_sync_cpp.index("statusMessage = tr(STR_INDEXING);")
    indexing_wait = koreader_sync_cpp.index("requestUpdateAndWait();", indexing_status)
    refine_after_wait = koreader_sync_cpp.index(
        "refined = refineRemoteProgressWithIncrementalIndexing(remotePosition);", indexing_wait
    )
    assert indexing_status < indexing_wait < refine_after_wait

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
        "gh_release_block",
        "^\\[env:gh_release\\]$",
    )

    assert_contains(
        "test/run_incremental_section_tests.sh",
        "command -v python",
        "command -v python3",
        "StaticIncrementalContracts.py",
    )

    assert_contains(
        "lib/Epub/Epub/IncrementalSectionCache.cpp",
        "checkedWritePod",
        "checkedWriteString",
        "checkedWriteLayoutKey",
        "checkedWriteIndexRecord",
        "readNextIndexRecord",
        "indexSize / PAGE_INDEX_RECORD_SIZE",
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

    assert_contains(
        "lib/Epub/Epub/BookMetadataCache.h",
        "FAST_LOOKUP_SPINE_THRESHOLD = 64",
    )
    assert_contains(
        "lib/Epub/Epub/BookMetadataCache.cpp",
        "spineCount >= FAST_LOOKUP_SPINE_THRESHOLD",
        "Using fast TOC spine index",
        "Using batch size lookup for %d spine items",
        "buildBookBin timings:",
    )
    assert_not_contains(
        "lib/Epub/Epub/BookMetadataCache.cpp",
        "spineCount >= LARGE_SPINE_THRESHOLD",
    )
    assert_contains(
        "lib/Epub/Epub/parsers/ContentOpfParser.h",
        "FAST_LOOKUP_SPINE_THRESHOLD = 64",
    )
    assert_contains(
        "lib/Epub/Epub/parsers/ContentOpfParser.cpp",
        "itemIndex.size() >= FAST_LOOKUP_SPINE_THRESHOLD",
        "Using fast spine item index",
    )
    assert_not_contains(
        "lib/Epub/Epub/parsers/ContentOpfParser.cpp",
        "itemIndex.size() >= LARGE_SPINE_THRESHOLD",
    )
    on_enter_body = slice_between(
        epub_reader_cpp,
        "void EpubReaderActivity::onEnter()",
        "void EpubReaderActivity::onExit()",
    )
    assert "bool loadedSavedProgress = false;" in on_enter_body
    assert "loadedSavedProgress = true;" in on_enter_body
    assert "freshBookEntry = !loadedSavedProgress;" in on_enter_body
    render_load_section_body = slice_between(
        epub_reader_cpp,
        "if (!section) {",
        "    const auto sectionOpenStart = millis();",
    )
    assert "freshBookEntry &&" in render_load_section_body
    assert "!hasCompatibleCompleteIncrementalCache(epub, currentSpineIndex, layoutKey)" in render_load_section_body
    assert "initialBookEntryIndexingPopupShown" in render_load_section_body
    assert "showIndexingPopupOnce(renderer, indexingPopupShown);" in render_load_section_body
    assert "freshBookEntry = false;" in epub_reader_cpp

    reader_activity_cpp = read("src/activities/reader/ReaderActivity.cpp")
    assert_contains(
        "src/activities/reader/ReaderActivity.h",
        "loadEpub(const std::string& path, const GfxRenderer& renderer,",
        "onGoToEpubReader(std::unique_ptr<Epub> epub, bool initialIndexingPopupShown)",
    )
    load_epub_body = slice_between(
        reader_activity_cpp,
        "std::unique_ptr<Epub> ReaderActivity::loadEpub",
        "std::unique_ptr<Xtc> ReaderActivity::loadXtc",
    )
    assert "makeUniqueNoThrow<Epub>" in load_epub_body
    assert '"/book.bin"' in load_epub_body
    assert '"/progress.bin"' in load_epub_body
    assert "GUI.drawPopup(renderer, tr(STR_INDEXING));" in load_epub_body
    assert "indexingPopupShown = true;" in load_epub_body
    assert "epub->load(true, SETTINGS.embeddedStyle == 0)" in load_epub_body
    assert "onGoToEpubReader(std::move(epub), initialIndexingPopupShown);" in reader_activity_cpp
    delete_cache_body = slice_between(
        epub_reader_cpp,
        "case EpubReaderMenuActivity::MenuAction::DELETE_CACHE:",
        "case EpubReaderMenuActivity::MenuAction::SCREENSHOT:",
    )
    assert "epub->clearCache();" in delete_cache_body
    assert "bookCacheDeleted = true;" in delete_cache_body
    assert "saveProgress" not in delete_cache_body
    assert "setupCacheDir" not in delete_cache_body
    assert "Book cache deleted; progress cache removed" in delete_cache_body
    render_body = slice_between(
        epub_reader_cpp,
        "void EpubReaderActivity::render(RenderLock&& lock)",
        "void EpubReaderActivity::renderContents(",
    )
    assert "if (bookCacheDeleted) {\n    return;\n  }" in render_body


if __name__ == "__main__":
    main()
