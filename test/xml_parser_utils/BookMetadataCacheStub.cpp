#include "BookMetadataCacheStub.h"

#include "Epub/BookMetadataCache.h"

namespace opf_test {
std::vector<std::string>& spineHrefs() {
  static std::vector<std::string> hrefs;
  return hrefs;
}
}  // namespace opf_test

// Host-test stand-in for BookMetadataCache::createSpineEntry. The real
// implementation streams each entry to an SD temp file; the host test has no SD,
// so we record the resolved href and bump the (private) spineCount that
// getSpineCount() reports.
void BookMetadataCache::createSpineEntry(const std::string& href) {
  opf_test::spineHrefs().push_back(href);
  spineCount++;
}
