// A book's embedded body font (BookFonts, the Publisher Fonts setting): which family the body text is set
// in, and the font files copied out of the EPUB as plain TrueType, obfuscation undone.
//
// The body-font cases are ported from crosspoint-reader PR #3934's PublisherBodyFontTest (Julia Nguyen),
// asked of the whole book here instead of one chapter's layout.
#include <gtest/gtest.h>
#include <wolfssl/wolfcrypt/hash.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "BuildArena.h"
#include "Epub.h"
#include "Epub/BookFonts.h"
#include "Epub/css/CssParser.h"
#include "SaxParser/SaxParser.h"
#include "StoredZipWriter.h"

namespace fs = std::filesystem;

namespace {

std::string readFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string hex(const uint8_t* bytes, const size_t n) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  for (size_t i = 0; i < n; ++i) {
    out += digits[bytes[i] >> 4];
    out += digits[bytes[i] & 15];
  }
  return out;
}

const std::string kTrueType = readFile(FONT_FIXTURE_DIR "/Ubuntu/Ubuntu-Regular.ttf");
const std::string kTrueTypeBold = readFile(FONT_FIXTURE_DIR "/Ubuntu/Ubuntu-Bold.ttf");
const std::string kCff = readFile(FONT_FIXTURE_DIR "/OpenDyslexic/OpenDyslexic-Regular.otf");

std::string obfuscated(std::string font, const uint8_t* key, const size_t keyLen, const size_t count) {
  for (size_t i = 0; i < count && i < font.size(); ++i) font[i] = static_cast<char>(font[i] ^ key[i % keyLen]);
  return font;
}

struct BookFontsTest : testing::Test {
  fs::path work;
  int books = 0;

  void SetUp() override {
    work = fs::temp_directory_path() /
           (std::string("book_fonts_") + testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
  }
  void TearDown() override { fs::remove_all(work); }

  struct File {
    std::string href;
    std::string mediaType;
    std::string bytes;
  };

  // One stylesheet, chapters in spine order (kMissing: listed, but not in the archive), any extra files;
  // identifiers as the OPF should carry them.
  std::shared_ptr<Epub> book(const std::string& css, const std::vector<std::string>& chapters,
                             const std::vector<File>& extra = {},
                             const std::string& identifiers =
                                 "<dc:identifier id=\"uid\">urn:isbn:978-0-00-000000-2"
                                 "</dc:identifier>") {
    test_zip::StoredZipWriter zip;
    zip.add("mimetype", "application/epub+zip");
    zip.add("META-INF/container.xml",
            "<?xml version=\"1.0\"?>\n<container version=\"1.0\" "
            "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\"><rootfiles><rootfile "
            "full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles></container>\n");
    std::string manifest = "<item id=\"css\" href=\"Styles/style.css\" media-type=\"text/css\"/>\n";
    std::string spine;
    for (size_t i = 0; i < chapters.size(); ++i) {
      const std::string id = "c" + std::to_string(i);
      manifest += "<item id=\"" + id + "\" href=\"Text/" + id + ".xhtml\" media-type=\"application/xhtml+xml\"/>\n";
      spine += "<itemref idref=\"" + id + "\"/>";
      if (chapters[i] == kMissing) continue;
      zip.add("OEBPS/Text/" + id + ".xhtml",
              "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\"><head>"
              "<title>T</title><link rel=\"stylesheet\" href=\"../Styles/style.css\" type=\"text/css\"/></head>" +
                  chapters[i] + "</html>\n");
    }
    for (size_t i = 0; i < extra.size(); ++i) {
      manifest += "<item id=\"x" + std::to_string(i) + "\" href=\"" + extra[i].href.substr(6) + "\" media-type=\"" +
                  extra[i].mediaType + "\"/>\n";
      zip.add(extra[i].href, extra[i].bytes);
    }
    zip.add("OEBPS/content.opf",
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<package xmlns=\"http://www.idpf.org/2007/opf\" "
            "version=\"3.0\" unique-identifier=\"uid\"><metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">" +
                identifiers + "<dc:title>Fonts</dc:title><dc:language>en</dc:language></metadata><manifest>" +
                manifest + "</manifest><spine>" + spine + "</spine></package>\n");
    zip.add("OEBPS/Styles/style.css", css);
    const std::string path = (work / ("book" + std::to_string(books++) + ".epub")).string();
    zip.write(path);
    auto epub = std::make_shared<Epub>(path, (work / "cache").string());
    EXPECT_TRUE(epub->load(true));
    epub->setupCacheDir();
    return epub;
  }

  static constexpr const char* kMissing = "(missing)";
  static uint32_t family(const char* name) { return CssParser::fontFamilyHash(name); }

  // The body family's identity, as Stage A compared it.
  static uint32_t detect(const Epub& epub) { return CssFontFamily::identity(found(epub).body); }
  static BookFonts::Found found(const Epub& epub, const std::string& records = {}) {
    bool failed = true;
    const BookFonts::Found found = BookFonts::detectFamilies(epub, nullptr, &failed, records);
    EXPECT_FALSE(failed);
    return found;
  }
};

const char* kProbeCss =
    "@font-face { font-family: EBGaramond; src: url(../Fonts/g.ttf); }\n"
    ".indent, .noindent { font-family: EBGaramond, serif; }\n"
    ".date, h1 { font-family: YatraOne, sans-serif; }\n"
    ".block { font-family: Roboto, sans-serif; }\n";

}  // namespace

TEST(BookFontsKeys, Sha1MatchesTheFipsVectors) {
  uint8_t digest[20];
  ASSERT_EQ(0, wc_ShaHash(reinterpret_cast<const byte*>("abc"), 3, digest));
  EXPECT_EQ("a9993e364706816aba3e25717850c26c9cd0d89d", hex(digest, 20));
  ASSERT_EQ(0, wc_ShaHash(reinterpret_cast<const byte*>(""), 0, digest));
  EXPECT_EQ("da39a3ee5e6b4b0d3255bfef95601890afd80709", hex(digest, 20));
  const char* twoBlocks = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  ASSERT_EQ(0, wc_ShaHash(reinterpret_cast<const byte*>(twoBlocks), 56, digest));
  EXPECT_EQ("84983e441c3bd26ebaae4aa1f95129e5e54670f1", hex(digest, 20));
}

TEST(BookFontsKeys, TheIdpfKeyHashesTheIdentifierWithoutWhiteSpace) {
  uint8_t key[20];
  ASSERT_TRUE(BookFonts::idpfKey(" a b\tc\r\n", key));
  EXPECT_EQ("a9993e364706816aba3e25717850c26c9cd0d89d", hex(key, 20));  // SHA-1("abc")
  EXPECT_FALSE(BookFonts::idpfKey(" \n", key));
}

TEST(BookFontsKeys, TheAdobeKeyIsTheUuidsSixteenBytes) {
  uint8_t key[16];
  ASSERT_TRUE(BookFonts::adobeKey("URN:UUID:01234567-89ab-cdef-0123-456789ABCDEF", key));
  EXPECT_EQ("0123456789abcdef0123456789abcdef", hex(key, 16));
  EXPECT_FALSE(BookFonts::adobeKey("urn:isbn:9780000000002", key));
  EXPECT_FALSE(BookFonts::adobeKey("urn:uuid:0123", key));
  EXPECT_FALSE(BookFonts::adobeKey("urn:uuid:01234567-89ab-cdef-0123-456789abcdefaa", key));
}

TEST_F(BookFontsTest, ParagraphBodyFontDoesNotComeFromALeadingDateOrHeading) {
  const auto epub =
      book(kProbeCss, {"<body><h1>Heading heading heading</h1><p class='date'>A long date line, a long date</p>"
                       "<p class='noindent'>Ordinary body paragraph with enough narrative text.</p>"
                       "<p class='indent'>More narrative with an <span class='block'>inline</span> accent.</p>"
                       "<p class='block'>Quotation</p></body>"});
  EXPECT_EQ(family("EBGaramond"), detect(*epub));
}

TEST_F(BookFontsTest, TheFamilyWithTheMostTextWinsOverTheBodysOwn) {
  // The body names one family, the paragraphs set most of the text in another: the text's family is the body
  // font. (A book's sheets are merged, so a `body` rule can come from a page no chapter is styled by.)
  const auto epub =
      book(kProbeCss, {"<body style='font-family: EBGaramond'><p>Body</p>"
                       "<p class='block'>A much longer paragraph in an intentionally different font.</p></body>"});
  EXPECT_EQ(family("Roboto"), detect(*epub));
}

TEST_F(BookFontsTest, TheBodysFamilyCountsForTheTextThatInheritsIt) {
  const auto epub = book(kProbeCss, {"<body class='indent'><h1 class='date'>Title</h1><p>Narrative prose that "
                                     "inherits the body's family.</p><p class='block'>Short aside</p></body>"});
  EXPECT_EQ(family("EBGaramond"), detect(*epub));
}

TEST_F(BookFontsTest, AnInlineAccentDoesNotChooseTheParagraphFamily) {
  const auto epub = book(
      kProbeCss, {"<body><p class='indent'><span class='block'>A much longer inline accent</span> Body</p></body>"});
  EXPECT_EQ(family("EBGaramond"), detect(*epub));
}

TEST_F(BookFontsTest, ReaderFontParagraphsGiveNoFamily) {
  const auto epub =
      book(kProbeCss, {"<body><p>Ordinary prose stays in the selected reader font without an embedded family.</p>"
                       "<p class='block'>Quotation</p></body>"});
  EXPECT_EQ(0u, detect(*epub));
}

TEST_F(BookFontsTest, DivParagraphsCountAndTheCountRunsAcrossChapters) {
  const auto epub = book(kProbeCss, {"<body><div class='block'>Short</div></body>",
                                     "<body><div class='noindent'>Much longer narrative in the second chapter, which "
                                     "is where the book's body text starts.</div></body>"});
  EXPECT_EQ(family("EBGaramond"), detect(*epub));
}

TEST_F(BookFontsTest, OnlyTheFirst64KbOfAChapterIsRead) {
  std::string chapter = "<body><p class='block'>First ";
  for (size_t i = 0; i < 12000; ++i) chapter += "prose ";  // ~72 KB
  chapter += "</p>";
  // ~100 KB more, in another family: it would win if the probe read on.
  for (int i = 0; i < 3000; ++i) chapter += "<p class='indent'>Late text the probe never reaches.</p>";
  chapter += "</body>";
  const auto epub = book(kProbeCss, {chapter});
  EXPECT_EQ(family("Roboto"), detect(*epub));
}

TEST_F(BookFontsTest, AnEmbeddedTrueTypeBodyIsCopiedWithItsBoldFace) {
  const auto epub = book(
      "@font-face { font-family: 'Ubuntu Book'; src: url(../Fonts/u.woff2) format('woff2'), url(../Fonts/u.ttf); }\n"
      "@font-face { font-family: 'Ubuntu Book'; font-weight: bold; src: url(../Fonts/ub.ttf); }\n"
      "@font-face { font-family: 'Ubuntu Book'; font-weight: 600; src: url(../Fonts/semi.ttf); }\n"
      "body { font-family: 'Ubuntu Book', serif }\n",
      {"<body><p>Body text.</p></body>"},
      {{"OEBPS/Fonts/u.ttf", "font/ttf", kTrueType},
       {"OEBPS/Fonts/ub.ttf", "font/ttf", kTrueTypeBold},
       {"OEBPS/Fonts/semi.ttf", "font/ttf", kTrueType}});
  ASSERT_EQ(BookFonts::State::Family, BookFonts::prepare(*epub, nullptr));
  std::string name;
  ASSERT_EQ(BookFonts::State::Family, BookFonts::read(epub->getCachePath(), &name));
  EXPECT_EQ("Ubuntu Book", name);
  const fs::path dir = BookFonts::familyDir(epub->getCachePath());
  EXPECT_EQ(kTrueType, readFile(dir / "0.ttf"));
  EXPECT_EQ(kTrueTypeBold, readFile(dir / "1.ttf"));  // weight 700 nearer than 600, whose file is not copied
  EXPECT_FALSE(fs::exists(dir / "2.ttf"));
  EXPECT_FALSE(fs::exists(dir / "3.ttf"));
}

TEST_F(BookFontsTest, AnIdpfObfuscatedFaceIsUndoneWithTheUniqueIdentifier) {
  const std::string uid = "urn:uuid:9f1b2c3d-4e5f-6071-8293-a4b5c6d7e8f9";
  uint8_t key[20];
  ASSERT_TRUE(BookFonts::idpfKey(uid, key));
  const auto epub = book("@font-face { font-family: Body; src: url(../Fonts/b.ttf); }\np { font-family: Body }\n",
                         {"<body><p>Body text.</p></body>"},
                         {{"OEBPS/Fonts/b.ttf", "application/vnd.ms-opentype", obfuscated(kTrueType, key, 20, 1040)}},
                         "<dc:identifier id=\"other\">urn:isbn:978-0-00-000000-2</dc:identifier>"
                         "<dc:identifier id=\"uid\">  " +
                             uid + "\n</dc:identifier>");
  ASSERT_EQ(BookFonts::State::Family, BookFonts::prepare(*epub, nullptr));
  EXPECT_EQ(kTrueType, readFile(fs::path(BookFonts::familyDir(epub->getCachePath())) / "0.ttf"));
}

TEST_F(BookFontsTest, AnAdobeObfuscatedFaceIsUndoneWithTheUuid) {
  uint8_t key[16];
  ASSERT_TRUE(BookFonts::adobeKey("urn:uuid:0123456789abcdef0123456789abcdef", key));
  BuildArena arena(52 * 1024);  // the lent framebuffer's share: rings and parser state come from it
  ASSERT_TRUE(arena.valid());
  const auto epub = book("@font-face { font-family: Body; src: url(../Fonts/b.ttf); }\np { font-family: Body }\n",
                         {"<body><p>Body text.</p></body>"},
                         {{"OEBPS/Fonts/b.ttf", "font/ttf", obfuscated(kTrueType, key, 16, 1024)}},
                         "<dc:identifier id=\"uid\">urn:uuid:0123456789abcdef0123456789abcdef</dc:identifier>");
  ASSERT_EQ(BookFonts::State::Family, BookFonts::prepare(*epub, &arena));
  EXPECT_GE(arena.highWater(), SaxParser::stateBytes());  // the parser state came from it
  EXPECT_EQ(0u, arena.used());                            // and everything was given back
  EXPECT_EQ(kTrueType, readFile(fs::path(BookFonts::familyDir(epub->getCachePath())) / "0.ttf"));
}

TEST_F(BookFontsTest, CffOutlinesAndUnknownBytesKeepTheReadersFont) {
  const auto cff = book("@font-face { font-family: Body; src: url(../Fonts/b.otf); }\np { font-family: Body }\n",
                        {"<body><p>Body text.</p></body>"}, {{"OEBPS/Fonts/b.otf", "font/otf", kCff}});
  EXPECT_EQ(BookFonts::State::None, BookFonts::prepare(*cff, nullptr));
  EXPECT_FALSE(fs::exists(fs::path(BookFonts::familyDir(cff->getCachePath())) / "0.ttf"));

  // Neither a font nor obfuscated with this book's keys: encrypted, so refused.
  const auto encrypted =
      book("@font-face { font-family: Body; src: url(../Fonts/b.ttf); }\np { font-family: Body }\n",
           {"<body><p>Body text.</p></body>"}, {{"OEBPS/Fonts/b.ttf", "font/ttf", std::string(4096, '\x5a')}});
  EXPECT_EQ(BookFonts::State::None, BookFonts::prepare(*encrypted, nullptr));
}

TEST_F(BookFontsTest, ABookWithoutAnEmbeddedBodyFamilyIsNone) {
  const auto plain = book("p { margin: 0 }\n", {"<body><p>Body text.</p></body>"});
  EXPECT_EQ(BookFonts::State::Pending, BookFonts::read(plain->getCachePath()));
  EXPECT_EQ(BookFonts::State::None, BookFonts::prepare(*plain, nullptr));
  EXPECT_EQ(BookFonts::State::None, BookFonts::read(plain->getCachePath()));

  // A face is declared, but the body is in another family.
  const auto other =
      book("@font-face { font-family: Display; src: url(../Fonts/d.ttf); }\nh1 { font-family: Display }\n",
           {"<body><h1>Title</h1><p>Body text.</p></body>"}, {{"OEBPS/Fonts/d.ttf", "font/ttf", kTrueType}});
  EXPECT_EQ(BookFonts::State::None, BookFonts::prepare(*other, nullptr));

  // A face whose file is missing from the book.
  const auto missing = book("@font-face { font-family: Body; src: url(../Fonts/gone.ttf); }\np { font-family: Body }\n",
                            {"<body><p>Body text.</p></body>"});
  EXPECT_EQ(BookFonts::State::None, BookFonts::prepare(*missing, nullptr));
}

TEST_F(BookFontsTest, AChapterThatCannotBeReadIsRetriedTwiceMore) {
  const auto epub = book("@font-face { font-family: Body; src: url(../Fonts/b.ttf); }\np { font-family: Body }\n",
                         {kMissing}, {{"OEBPS/Fonts/b.ttf", "font/ttf", kTrueType}});
  EXPECT_EQ(BookFonts::State::Pending, BookFonts::prepare(*epub, nullptr));
  EXPECT_EQ(BookFonts::State::Pending, BookFonts::prepare(*epub, nullptr));
  EXPECT_EQ(BookFonts::State::None, BookFonts::prepare(*epub, nullptr));  // given up on
  EXPECT_EQ(BookFonts::State::None, BookFonts::read(epub->getCachePath()));
}

// ---- Stage B phase 1: the body's generic family and the accent family -------------------------------------

namespace {

const std::string kInter = readFile(FONT_FIXTURE_DIR "/Inter/Inter-Regular.ttf");
const std::string kNotoSans = readFile(FONT_FIXTURE_DIR "/NotoSans/NotoSans-Regular.ttf");

// The body in Body, chapter titles and headings in Display, a dedication in Script.
const char* kAccentCss =
    "@font-face { font-family: Body; src: url(../Fonts/b.ttf); }\n"
    "@font-face { font-family: Display; src: url(../Fonts/d.ttf); }\n"
    "@font-face { font-family: Script; src: url(../Fonts/s.ttf); }\n"
    "p { font-family: Body, serif }\n"
    "h1, h2 { font-family: Display, sans-serif }\n"
    ".dedication, .aside { font-family: Script }\n";

const char* kAccentChapter =
    "<body><h1>One</h1><p class='dedication'>For someone</p>"
    "<p>Body prose with an <span class='aside'>inline aside</span> and more body prose, long enough to be the "
    "body text by far, as body text is.</p><p>More body prose.</p>"
    "<h2><span class='aside'>A heading set in its span</span></h2><h2>Two</h2><h2>Three</h2></body>";

}  // namespace

TEST_F(BookFontsTest, TheBodysGenericFamilyIsKept) {
  const auto epub = book("body { font-family: serif }\nh1 { font-family: sans-serif }\n",
                         {"<body><h1>Title</h1><p>Prose in the generic serif the body names.</p></body>"});
  const BookFonts::Found families = found(*epub);
  EXPECT_EQ(CssFontFamily::kSerif, families.body);
  EXPECT_EQ(0u, families.accent);  // a generic family is no accent
  // Probed although the book embeds no font, so the reader knows what its body is set in.
  ASSERT_EQ(BookFonts::State::None, BookFonts::prepare(*epub, nullptr));
  BookFonts::Families read;
  EXPECT_EQ(BookFonts::State::None, BookFonts::read(epub->getCachePath(), nullptr, &read));
  EXPECT_EQ(CssFontFamily::kSerif, read.body);
  EXPECT_EQ(0u, read.accent);
}

TEST_F(BookFontsTest, TheGenericOfMostOfTheBodyTextIsTheBodys) {
  // A family counts as one with and without its fallback; its most-used list gives the generic.
  const auto epub = book("p { font-family: Body, sans-serif }\n.short { font-family: Body }\n",
                         {"<body><p>The longer run of body text, long enough to count.</p>"
                          "<p class='short'>Short.</p></body>"});
  EXPECT_EQ(family("Body") | CssFontFamily::kSansSerif, found(*epub).body);
}

TEST_F(BookFontsTest, TheAccentIsTheFamilyTheMostOtherBlocksAreSetIn) {
  const auto epub = book(kAccentCss, {kAccentChapter},
                         {{"OEBPS/Fonts/b.ttf", "font/ttf", kTrueType},
                          {"OEBPS/Fonts/d.ttf", "font/ttf", kInter},
                          {"OEBPS/Fonts/s.ttf", "font/ttf", kNotoSans}});
  std::string records;
  ASSERT_TRUE(CssParser::loadFontFaces(epub->getCachePath(), records));
  // Display: three headings. Script: the dedication and the heading set in its span; the inline aside is its
  // body paragraph's.
  const BookFonts::Found families = found(*epub, records);
  EXPECT_EQ(family("Body") | CssFontFamily::kSerif, families.body);
  EXPECT_EQ(family("Display"), families.accent);

  ASSERT_EQ(BookFonts::State::Family, BookFonts::prepare(*epub, nullptr));
  std::string body;
  std::string accent;
  BookFonts::Families read;
  ASSERT_EQ(BookFonts::State::Family, BookFonts::read(epub->getCachePath(), &body, &read));
  EXPECT_EQ(BookFonts::State::Family, BookFonts::read(epub->getCachePath(), &accent, nullptr, /*body=*/false));
  EXPECT_EQ("Body", body);
  EXPECT_EQ("Display", accent);
  EXPECT_EQ(families.body, read.body);
  EXPECT_EQ(family("Display"), read.accent);
  EXPECT_EQ(kTrueType, readFile(fs::path(BookFonts::familyDir(epub->getCachePath())) / "0.ttf"));
  EXPECT_EQ(kInter, readFile(fs::path(BookFonts::familyDir(epub->getCachePath(), true)) / "0.ttf"));
}

TEST_F(BookFontsTest, AHeadingSetInASpanCountsForTheSpansFamily) {
  // Script has two blocks only because the span heading counts for it: with one more Display heading gone, it wins.
  std::string chapter = kAccentChapter;
  chapter.replace(chapter.find("<h2>Two</h2><h2>Three</h2>"), 26, "");
  const auto epub = book(kAccentCss, {chapter},
                         {{"OEBPS/Fonts/b.ttf", "font/ttf", kTrueType},
                          {"OEBPS/Fonts/d.ttf", "font/ttf", kInter},
                          {"OEBPS/Fonts/s.ttf", "font/ttf", kNotoSans}});
  std::string records;
  ASSERT_TRUE(CssParser::loadFontFaces(epub->getCachePath(), records));
  EXPECT_EQ(family("Script"), found(*epub, records).accent);
}

TEST_F(BookFontsTest, AnAccentWithoutARegularFaceIsPassedOver) {
  std::string css = kAccentCss;
  css.insert(css.find("src: url(../Fonts/d.ttf)"), "font-weight: bold; ");  // Display has only a bold face
  const auto epub = book(css, {kAccentChapter},
                         {{"OEBPS/Fonts/b.ttf", "font/ttf", kTrueType},
                          {"OEBPS/Fonts/d.ttf", "font/ttf", kInter},
                          {"OEBPS/Fonts/s.ttf", "font/ttf", kNotoSans}});
  std::string records;
  ASSERT_TRUE(CssParser::loadFontFaces(epub->getCachePath(), records));
  EXPECT_EQ(family("Script"), found(*epub, records).accent);
}

TEST_F(BookFontsTest, ARefusedAccentLeavesTheBodyFamily) {
  const auto epub = book(kAccentCss, {kAccentChapter},
                         {{"OEBPS/Fonts/b.ttf", "font/ttf", kTrueType},
                          {"OEBPS/Fonts/d.ttf", "font/otf", kCff},
                          {"OEBPS/Fonts/s.ttf", "font/ttf", kNotoSans}});
  ASSERT_EQ(BookFonts::State::Family, BookFonts::prepare(*epub, nullptr));
  BookFonts::Families read;
  std::string accent = "stale";
  EXPECT_EQ(BookFonts::State::Family, BookFonts::read(epub->getCachePath(), &accent, &read, /*body=*/false));
  EXPECT_EQ(0u, read.accent);
  EXPECT_EQ("", accent);
  EXPECT_FALSE(fs::exists(BookFonts::familyDir(epub->getCachePath(), true)));
  EXPECT_EQ(kTrueType, readFile(fs::path(BookFonts::familyDir(epub->getCachePath())) / "0.ttf"));
}

TEST_F(BookFontsTest, AnAccentIsCopiedWhenTheBodyIsNotEmbedded) {
  const auto epub = book(
      "@font-face { font-family: Display; src: url(../Fonts/d.ttf); }\nh1, h2 { font-family: Display }\n",
      {"<body><h1>Title</h1><p>Body prose in the reader's font, long enough to be the body.</p><h2>Two</h2></body>"},
      {{"OEBPS/Fonts/d.ttf", "font/ttf", kInter}});
  ASSERT_EQ(BookFonts::State::None, BookFonts::prepare(*epub, nullptr));
  std::string accent;
  BookFonts::Families read;
  EXPECT_EQ(BookFonts::State::None, BookFonts::read(epub->getCachePath(), &accent, &read, /*body=*/false));
  EXPECT_EQ("Display", accent);
  EXPECT_EQ(family("Display"), read.accent);
  EXPECT_EQ(0u, read.body);
  EXPECT_EQ(kInter, readFile(fs::path(BookFonts::familyDir(epub->getCachePath(), true)) / "0.ttf"));
}
