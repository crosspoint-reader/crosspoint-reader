# Full-pipeline equivalence harness:
# builds the REAL Epub/Section/ParsedText/CssParser pipeline for the host against
# deterministic stubs (GfxRenderer metrics, ESP heap/wdt, I18n) and the real-I/O
# HalStorage shim from test/zip_entry_reader.
enable_language(C)

set(PIPELINE_SOURCES
  PipelineRunner.cpp
  LoggingImpl.cpp
  # lib/Epub — the code under test
  ${REPO_ROOT}/lib/Epub/Epub.cpp
  ${REPO_ROOT}/lib/Epub/Epub/Section.cpp
  ${REPO_ROOT}/lib/Epub/Epub/SpinePageIndex.cpp
  ${REPO_ROOT}/lib/Epub/Epub/Page.cpp
  ${REPO_ROOT}/lib/Epub/Epub/CjkLineBreak.cpp
  ${REPO_ROOT}/lib/Epub/Epub/CjkPunctuation.cpp
  ${REPO_ROOT}/lib/Epub/Epub/ParsedText.cpp
  ${REPO_ROOT}/lib/Epub/Epub/TypographyProfile.cpp
  ${REPO_ROOT}/lib/Epub/Epub/BookMetadataCache.cpp
  ${REPO_ROOT}/lib/Epub/Epub/MetadataSidecar.cpp
  ${REPO_ROOT}/lib/Epub/Epub/FootnotePreviews.cpp
  ${REPO_ROOT}/lib/Epub/Epub/FootnoteShape.cpp
  ${REPO_ROOT}/lib/Epub/Epub/EpubImageManifest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/htmlEntities.cpp
  ${REPO_ROOT}/lib/Epub/Epub/css/CssParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/ContainerParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/ContentOpfParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/PageListSink.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/PageMapParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/TocNavParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/TocNcxParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/blocks/TextBlock.cpp
  ${REPO_ROOT}/lib/Epub/Epub/blocks/ImageBlock.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/HyphenationCommon.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/Hyphenator.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/LanguageRegistry.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/LiangHyphenation.cpp
  ${REPO_ROOT}/lib/Epub/Epub/converters/GifToFramebufferConverter.cpp
  ${REPO_ROOT}/lib/Epub/Epub/converters/ImageDecoderFactory.cpp
  ${REPO_ROOT}/lib/Epub/Epub/converters/ImageToFramebufferDecoder.cpp
  ${REPO_ROOT}/lib/Epub/Epub/converters/JpegToFramebufferConverter.cpp
  ${REPO_ROOT}/lib/Epub/Epub/converters/PngToFramebufferConverter.cpp
  # supporting libs the pipeline links against
  ${REPO_ROOT}/lib/ZipFile/ZipFile.cpp
  ${REPO_ROOT}/lib/InflateReader/InflateReader.cpp
  ${REPO_ROOT}/lib/uzlib/src/tinflate.c
  ${REPO_ROOT}/lib/FsHelpers/FsHelpers.cpp
  ${REPO_ROOT}/lib/FsHelpers/SidecarFiles.cpp
  ${REPO_ROOT}/lib/CooperativeAbort/CooperativeAbort.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/lib/GfxRenderer/Bitmap.cpp
  ${REPO_ROOT}/lib/GfxRenderer/BitmapHelpers.cpp
  ${REPO_ROOT}/lib/JpegToBmpConverter/JpegToBmpConverter.cpp
  ${REPO_ROOT}/lib/ProgressiveJpegDc/ProgressiveJpegDc.cpp
  ${REPO_ROOT}/lib/ProgressiveJpeg/ProgressiveJpeg.cpp
  ${REPO_ROOT}/lib/PngToBmpConverter/PngToBmpConverter.cpp
  ${REPO_ROOT}/lib/PngDecoder/PngStreamDecoder.cpp
  ${REPO_ROOT}/lib/TJpgDec/src/tjpgd.c
)

set(PIPELINE_INCLUDES
  # Our stubs must shadow the real device headers: GfxRenderer.h, HalDisplay.h,
  # I18n.h, esp_*.h.
  ${CMAKE_CURRENT_SOURCE_DIR}
  # Real stdio-backed FsFile/Storage (shadows test/shims/HalStorage.h).
  ${REPO_ROOT}/test/zip_entry_reader
  # Arduino shims: Arduino.h, Print.h, HardwareSerial.h, WString.h.
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/EpdFont
  # For Bitmap.h/BitmapHelpers.h only — our stub dir precedes this, so the
  # real GfxRenderer.h in the same directory stays shadowed.
  ${REPO_ROOT}/lib/GfxRenderer
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/ZipFile
  ${REPO_ROOT}/lib/InflateReader
  ${REPO_ROOT}/lib/uzlib/src
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/lib/CooperativeAbort
  ${REPO_ROOT}/lib/JpegToBmpConverter
  ${REPO_ROOT}/lib/PngToBmpConverter
  ${REPO_ROOT}/lib/PngDecoder
  ${REPO_ROOT}/lib/ProgressiveJpegDc
  ${REPO_ROOT}/lib/ProgressiveJpeg
  ${REPO_ROOT}/lib/TJpgDec/src
)

add_executable(EpubPipelineTest
  EpubPipelineTest.cpp
  CjkRulesTest.cpp
  FingerprintTest.cpp
  LostTocRetryTest.cpp
  TocSourceTest.cpp
  PageListSinkTest.cpp
  MetadataSidecarTest.cpp
  SidecarFilesTest.cpp
  SectionAbortTest.cpp
  SpinePageIndexTest.cpp
  StreamedParseTest.cpp
  FootnotePreviewStoreTest.cpp
  FootnoteResolveSliceTest.cpp
  ParagraphAnchorTest.cpp
  ParsedTextOffsetTest.cpp
  LowHeapLoadTest.cpp
  LongChapterPageTablesTest.cpp
  TableRowFallbackArenaTest.cpp
  AnchorMapTest.cpp
  AnchorPageAccuracyTest.cpp
  TocBoundariesAfterBuildTest.cpp
  SpanAnchorTest.cpp
  ImageExtractionTest.cpp
  ImageManifestTest.cpp
  ImageWrapperWidthTest.cpp
  HeadingBlockLayoutTest.cpp
  BlockEmphasisTest.cpp
  PageBreakAfterTest.cpp
  KeepTogetherTest.cpp
  NativeSizeLayoutTest.cpp
  RootFontSizeTest.cpp
  FontFamilyLayoutTest.cpp
  SpineCacheLayoutTest.cpp
  SaxStateInArenaTest.cpp
  JpegDownscaleTest.cpp
  CompanionCacheTest.cpp
  ImageParkTest.cpp
  StoredEntryDecodeTest.cpp
  PlaceholderOnlyScopeTest.cpp
  PixelCacheWriteTest.cpp
  BufferedPrintTest.cpp
  CoverPipelineTest.cpp
  CoverWrapperTest.cpp
  ProgressMapperTest.cpp
  ProgressMapperRoundTripTest.cpp
  XPathExtractionTest.cpp
  VisibleTextTest.cpp
  VisibleTextOffsetLutTest.cpp
  TextNodeRulesTest.cpp
  BookFontsTest.cpp
  FirstOpenIndexingTest.cpp
  # A book's embedded body font (Publisher Fonts): the probe and the extraction. Its SHA-1 comes from
  # test/shims/wolfssl, standing in for wolfCrypt.
  ${REPO_ROOT}/lib/Epub/Epub/BookFonts.cpp
  # lib/KOReaderSync position mapping: needs only the Epub and the SAX parser, not the network.
  ${REPO_ROOT}/lib/KOReaderSync/ProgressMapper.cpp
  ${REPO_ROOT}/lib/KOReaderSync/ChapterXPathIndexer.cpp
  ${REPO_ROOT}/lib/KOReaderSync/ChapterXPathIndexerInternal.cpp
  ${REPO_ROOT}/lib/KOReaderSync/ChapterXPathForwardMapper.cpp
  ${REPO_ROOT}/lib/KOReaderSync/ChapterXPathReverseMapper.cpp
  # No-op HeapTrack: PipelineRunner brackets the dump read-back with pause/resume, and the gtest
  # binary must NOT get the real one (it overrides malloc/free process-wide). The two dump
  # targets pick their own implementation, so this cannot be moved into PIPELINE_SOURCES.
  HeapTrackStub.cpp
  ${PIPELINE_SOURCES}
)
target_include_directories(EpubPipelineTest PRIVATE ${PIPELINE_INCLUDES})
target_compile_definitions(EpubPipelineTest PRIVATE
  CORPUS_DIR="${REPO_ROOT}/test/epubs"
  GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/goldens"
  JPEG_FIXTURE_DIR="${REPO_ROOT}/test/jpeg_decoder/fixtures"
  FONT_FIXTURE_DIR="${REPO_ROOT}/lib/EpdFont/builtinFonts/source"
  ENABLE_SERIAL_LOG=1
  LOG_LEVEL=2
  # LongChapterPageTablesTest refuses build-table chunks on purpose (see ChunkedVector.h). The
  # hook is inert until a test arms it; firmware and the dump tools never define it.
  CHUNKED_VECTOR_FAULT_INJECTION
  # Central-directory walk counters (ZipFile.h), for the tests that bound them. Inert otherwise.
  ZIP_FILE_SCAN_STATS
)
target_link_libraries(EpubPipelineTest PRIVATE
  crosspoint_test_common
  SaxParser
  GTest::gtest_main
)
gtest_discover_tests(EpubPipelineTest PROPERTIES TIMEOUT 300)

# HeapTrack overrides malloc/free process-wide — CLI tool only, never the gtest.
add_executable(epub_pipeline_dump
  DumpMain.cpp
  HeapTrack.cpp
  ${PIPELINE_SOURCES}
)
target_include_directories(epub_pipeline_dump PRIVATE ${PIPELINE_INCLUDES})
target_compile_definitions(epub_pipeline_dump PRIVATE
  ENABLE_SERIAL_LOG=1
  LOG_LEVEL=2
)
target_link_libraries(epub_pipeline_dump PRIVATE
  crosspoint_test_common
  SaxParser
)

# Heap-peak regression gate (memory audit 2026-09, R5): the dump tool's --bench peak, per
# fixture and build mode, against test/epub_pipeline/heap_peak_baseline.txt. See heap_peak_check.py.
# Not registered under ASan: HeapTrack's malloc interposer segfaults against ASan's own before
# main(), and a sanitizer build's peaks are not comparable to the baseline anyway.
if(NOT CMAKE_CXX_FLAGS MATCHES "-fsanitize=[^ ]*address")
  add_test(NAME HeapPeakRegression
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/heap_peak_check.py
            $<TARGET_FILE:epub_pipeline_dump> ${REPO_ROOT}/test
            ${CMAKE_CURRENT_SOURCE_DIR}/heap_peak_baseline.txt
  )
  set_tests_properties(HeapPeakRegression PROPERTIES TIMEOUT 600)
endif()

# Exact allocation inventory of section builds (memory audit 2026-09): the heap profiler's
# per-build window plus BuildArena's host-only trace hooks, with debug info and frame pointers so
# every site symbolizes to file:line (inline chains included). See InventoryMain.cpp and
# inventory_report.py. Not a test; run it by hand.
add_executable(epub_build_inventory
  InventoryMain.cpp
  HeapTrack.cpp
  ${PIPELINE_SOURCES}
)
target_include_directories(epub_build_inventory PRIVATE ${PIPELINE_INCLUDES})
target_compile_definitions(epub_build_inventory PRIVATE
  ENABLE_SERIAL_LOG=1
  LOG_LEVEL=1
  BUILD_ARENA_TRACE=1
)
target_compile_options(epub_build_inventory PRIVATE -O2 -g -fno-omit-frame-pointer -fno-ipa-icf)
target_link_libraries(epub_build_inventory PRIVATE
  crosspoint_test_common
  SaxParser
  ${CMAKE_DL_LIBS}
)

# Same dump tool without the process-wide malloc/free override, which deadlocks
# under the MinGW runtime. --bench heap numbers read 0 here; everything else is
# identical, so this is the variant to use when diagnosing layout on Windows.
add_executable(epub_pipeline_dump_noheap
  DumpMain.cpp
  HeapTrackStub.cpp
  ${PIPELINE_SOURCES}
)
target_include_directories(epub_pipeline_dump_noheap PRIVATE ${PIPELINE_INCLUDES})
target_compile_definitions(epub_pipeline_dump_noheap PRIVATE
  ENABLE_SERIAL_LOG=1
  LOG_LEVEL=2
)
target_link_libraries(epub_pipeline_dump_noheap PRIVATE
  crosspoint_test_common
  SaxParser
)
