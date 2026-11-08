"""Exercise production loadMetadata and book.bin I/O without the reader's graphics dependencies."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "lib/Epub/Epub.cpp").read_text()
constructor = source[source.index("Epub::Epub("):source.index("bool Epub::findContentOpfFile(")]
method = source[source.index("bool Epub::loadMetadata("):source.index("bool Epub::loadSyncMetadata(")]
harness = r'''
#include <cassert>
#include <filesystem>
#include <Memory.h>
#include <Utf8.h>
#include <Logging.h>
#include <ZipFile.h>
#include "Epub/BookMetadataCache.h"
#include "Epub/PackageGroupFields.h"
namespace Txt { bool isTxtOrMd(const std::string&) { return false; } }
namespace FsHelpers { std::string getFileNameWithoutExtension(const std::string&) { return {}; } }
class Epub {
  std::string filepath, cachePath;
  bool parseContentOpf(BookMetadataCache::BookMetadata&, bool, bool, ZipFile*, PackageGroupFields*) {
    assert(false && "cached metadata must not parse the package");
    return false;
  }
 public:
  Epub(std::string filepath, const std::string& cacheDir);
  bool loadMetadata(std::string&, std::string&, PackageGroupFields* = nullptr);
};
''' + constructor + method + r'''
int main(int argc, char** argv) {
  assert(argc == 2);
  const std::string root = argv[1];
  const std::string book = root + "/missing.epub";
  const std::string cachePath = root + "/epub_" + std::to_string(std::hash<std::string>{}(book));
  std::filesystem::create_directories(cachePath);
  {
    BookMetadataCache cache(cachePath);
    assert(cache.beginWrite() && cache.beginContentOpfPass());
    assert(cache.endContentOpfPass() && cache.beginTocPass());
    assert(cache.endTocPass() && cache.endWrite());
    BookMetadataCache::BookMetadata metadata;
    metadata.title = "Cached title";
    metadata.author = "Cached author";
    assert(cache.buildBookBin(book, metadata));
  }
  Epub epub(book, root);
  std::string title = "old", author = "old";
  zipOpenCalls = 0;
  assert(epub.loadMetadata(title, author));
  assert(title == "Cached title" && author == "Cached author");
  assert(zipOpenCalls == 0);
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    cpp = path / "metadata.cpp"
    binary = path / "metadata"
    cpp.write_text(harness)
    includes = ["test/book_metadata_cache/stubs", "lib/Epub", "lib/Memory",
                "lib/Serialization", "lib/Utf8", "test/chapter_html_slim_parser/stubs"]
    subprocess.run(["c++", "-std=c++20", *[f"-I{ROOT / p}" for p in includes],
                    str(cpp), str(ROOT / "lib/Epub/Epub/BookMetadataCache.cpp"),
                    str(ROOT / "lib/Utf8/Utf8.cpp"), "-o", str(binary)], check=True)
    subprocess.run([str(binary), directory], check=True)
print("EPUB cached metadata regression passed")
