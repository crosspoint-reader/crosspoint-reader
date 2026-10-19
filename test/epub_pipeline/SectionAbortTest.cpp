// Regression cover for what abortSectionBuild() keeps on disk.
//
// A sliced build runs in two phases: (a) inflate the spine's XHTML to a book-keyed HTML cache,
// then (b) parse it. Phase (a) is the expensive half on a big spine, and the whole
// BG_BUILD_MAX_PREEMPTIONS design assumes a preempted attempt BANKS it — attempt 1 pays for the
// inflate, attempt 2 skips it and gets a real shot at the parse before Background-B gives up.
//
// It did not. abortSectionBuild() deleted the cache whenever `!reusedHtml`, i.e. whenever THIS
// build produced the file — true whether or not the inflate had finished. So every preempted
// attempt threw away a complete extraction, attempt 2 re-inflated from scratch, lost the same
// race, and B abandoned the spine. Device-observed on X3 (2026-08-11): spine 2 extracted 14054
// bytes, was preempted with phase (a) complete, and the cache was removed anyway.
//
// There was no fixture, which is why it survived. This is that fixture: it fails on the old
// `!reusedHtml` condition and passes on `!extractDone`.
#include <Arduino.h>
#include <GlyphProvider.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "Epub.h"
#include "Epub/Section.h"
#include "GfxRenderer.h"
#include "PipelineRunner.h"

namespace fs = std::filesystem;

namespace {

constexpr int kSpineIndex = 0;

std::string freshCacheDir(const std::string& tag) {
  const auto dir = fs::temp_directory_path() / "section_abort_test" / tag;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir.string();
}

Section::BuildParams defaultParams() {
  Section::BuildParams p;
  p.fontId = 0;
  p.lineCompression = 1.0f;
  p.extraParagraphSpacing = false;
  p.paragraphAlignment = 0;
  p.viewportWidth = 480;
  p.viewportHeight = 800;
  p.hyphenationEnabled = false;
  p.fontSizeNormalization = false;
  p.embeddedStyle = false;
  p.bionicReadingEnabled = false;
  p.inlineFootnotePreviews = false;
  p.imageRendering = 0;
  return p;
}

// The book-keyed HTML cache is keyed on the spine alone, not on render properties
// (Section::sectionHtmlCachePath), so a test can name it without running a build. Note the
// base is the EPUB's own cache directory (cacheDir/epub_<hash>), not the cacheDir handed to the
// Epub constructor.
std::string htmlCachePath(const Epub& epub) { return Section::sectionHtmlCachePath(epub.getCachePath(), kSpineIndex); }

}  // namespace

// Drive a sliced build only as far as the end of phase (a), abort, and require the banked
// XHTML to survive at its full size — the exact predicate the reuse path checks on the next
// attempt (`st.tempFile.size() == st.inflatedSize`).
TEST(SectionAbort, KeepsCompletedHtmlExtractSoTheRetryCanReuseIt) {
  const std::string cacheDir = freshCacheDir("keeps_completed_extract");
  // Needs a spine whose phase (b) spans several PARSE_CHUNK_BYTES reads, so the build is still
  // live once phase (a) has landed. A tiny fixture extracts and parses inside one slice and
  // never enters the state under test.
  const std::string epubPath = std::string(CORPUS_DIR) + "/test_tables.epub";

  GfxRenderer renderer;
  auto epub = std::make_shared<Epub>(epubPath, cacheDir);
  ASSERT_TRUE(epub->load(true));

  size_t inflatedSize = 0;
  ASSERT_TRUE(epub->getSpineItemInflatedSize(kSpineIndex, &inflatedSize));
  ASSERT_GT(inflatedSize, 0u);

  Section section(epub, kSpineIndex, renderer);
  const Section::BuildParams p = defaultParams();
  const std::string html = htmlCachePath(*epub);

  // Without a ticking clock the host's millis() is frozen, overBudget() is never true, and the
  // build runs to Done in one call — the mid-build state this test needs would be unreachable.
  const host_clock::Ticking tick(1);

  bool extractComplete = false;
  for (int i = 0; i < 20000; ++i) {
    const Section::BuildStep step = section.stepSectionBuild(p, /*budgetMs=*/1);
    // Only a state where the build is STILL LIVE exercises the abort path; if the build reaches
    // Done in the same slice that finished the extract, this is not the case under test.
    if (section.hasActiveBuild() && fs::exists(html) && fs::file_size(html) == inflatedSize) {
      extractComplete = true;
      break;
    }
    if (step == Section::BuildStep::Done || step == Section::BuildStep::Failed) break;
  }
  ASSERT_TRUE(extractComplete) << "phase (a) never produced a complete " << html;
  ASSERT_TRUE(section.hasActiveBuild()) << "build finished before it could be preempted; "
                                           "the abort path under test was never entered";

  section.abortSectionBuild();

  EXPECT_TRUE(fs::exists(html)) << "a completed extract was discarded on abort — the retry must "
                                   "now re-inflate, which is what starves Background-B";
  EXPECT_EQ(fs::file_size(html), inflatedSize) << "banked XHTML is the wrong size, so the reuse "
                                                  "check will reject it and re-inflate anyway";
}

// The complement: an abort with no build in flight must not touch a cache an EARLIER build
// legitimately banked. Guards the no-op early return in abortSectionBuild().
TEST(SectionAbort, NoOpAbortLeavesAnExistingCacheAlone) {
  const std::string cacheDir = freshCacheDir("noop_abort");
  const std::string epubPath = std::string(CORPUS_DIR) + "/test_headings.epub";

  GfxRenderer renderer;
  auto epub = std::make_shared<Epub>(epubPath, cacheDir);
  ASSERT_TRUE(epub->load(true));

  Section section(epub, kSpineIndex, renderer);
  ASSERT_TRUE(section.createSectionFile(defaultParams(), {}, /*skipEviction=*/true));

  const std::string html = htmlCachePath(*epub);
  ASSERT_TRUE(fs::exists(html));
  const auto sizeBefore = fs::file_size(html);

  ASSERT_FALSE(section.hasActiveBuild());
  section.abortSectionBuild();  // no build in flight — must be a no-op

  EXPECT_TRUE(fs::exists(html));
  EXPECT_EQ(fs::file_size(html), sizeBefore);
}

// A glyph laid out while no memory could be had to draw it (a TrueType miss, glyph_miss) was measured as missing: the
// build fails rather than keep that layout, and leaves no section behind; with the memory there, it builds. A miss
// that went without outside the build's own slices (a page drawn between them) is none of its business.
TEST(SectionAbort, ABuildThatMeasuredAGlyphWithoutMemoryIsNotKept) {
  const std::string cacheDir = freshCacheDir("starved_glyph");
  const std::string epubPath = std::string(CORPUS_DIR) + "/test_headings.epub";

  GfxRenderer renderer;
  auto epub = std::make_shared<Epub>(epubPath, cacheDir);
  ASSERT_TRUE(epub->load(true));
  Section section(epub, kSpineIndex, renderer);

  GfxRenderer::starvedCodepoint = 'e';
  EXPECT_FALSE(section.createSectionFile(defaultParams(), {}, /*skipEviction=*/true));
  GfxRenderer::starvedCodepoint = 0;
  EXPECT_FALSE(section.hasActiveBuild());
  EXPECT_FALSE(section.loadSectionFile(defaultParams()));

  glyph_miss::noteStarved();
  ASSERT_TRUE(section.createSectionFile(defaultParams(), {}, /*skipEviction=*/true));
  EXPECT_TRUE(section.loadSectionFile(defaultParams()));
  EXPECT_GT(section.pageCount, 0);
}
