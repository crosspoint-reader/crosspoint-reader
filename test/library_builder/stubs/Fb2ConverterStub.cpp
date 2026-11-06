// Host-test stub for the Fb2Converter surface LibraryBuilder.cpp references.
// The real implementation reads files through HalStorage (firmware-only); the
// library fixtures in this suite are EPUBs, so the FB2 path never runs.
#include <Fb2Converter.h>
#include <string>

bool Fb2Converter::readMetadata(const std::string&, fb2passes::Metadata&, std::string&) {
  return false;
}

const char* Fb2Converter::imageExtensionForContentType(const char*) {
  return "";
}
