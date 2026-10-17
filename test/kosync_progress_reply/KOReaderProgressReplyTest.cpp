// How a KOSync GET /syncs/progress reply is read (lib/KOReaderSync/KOReaderProgressReply).
//
// The client screens the transport first (status, a captive portal's HTML, a bodiless 2xx); what
// reaches parseProgressReply() is a 2xx body that looks like JSON. Servers disagree on how they
// say "nothing stored", and every one of those shapes has to read as NOT_FOUND, never as a record
// at 0 %: the sync screen would offer it, and smart sync would apply it.
#include <gtest/gtest.h>

#include <string>

#include "KOReaderSync/KOReaderProgressReply.h"

namespace {

constexpr char kHash[] = "0123456789abcdef0123456789abcdef";

KOReaderSyncClient::Error parse(const std::string& body, KOReaderProgress& out) {
  return parseProgressReply(body, kHash, out);
}

}  // namespace

// koreader-sync-server's reply for a stored position.
TEST(KOReaderProgressReply, LocatorRecordIsRead) {
  KOReaderProgress out;
  ASSERT_EQ(parse(R"({"percentage":0.4567,"progress":"/body/DocFragment[12]/body/div/p[3]/text().42",)"
                  R"("device":"KOReader","device_id":"A1B2","timestamp":1700000000,"document":"x"})",
                  out),
            KOReaderSyncClient::OK);
  EXPECT_EQ(out.document, kHash);
  EXPECT_EQ(out.progress, "/body/DocFragment[12]/body/div/p[3]/text().42");
  EXPECT_FLOAT_EQ(out.percentage, 0.4567f);
  EXPECT_EQ(out.device, "KOReader");
  EXPECT_EQ(out.deviceId, "A1B2");
  EXPECT_EQ(out.timestamp, 1700000000);
}

// "Nothing stored", as the reference server ({}) and others ([], "", null) serialize it.
TEST(KOReaderProgressReply, EmptyPayloadsAreNotFound) {
  for (const char* body : {"{}", "[]", "\"\"", "null"}) {
    KOReaderProgress out;
    EXPECT_EQ(parse(body, out), KOReaderSyncClient::NOT_FOUND) << body;
  }
}

TEST(KOReaderProgressReply, TruncatedJsonIsAJsonError) {
  KOReaderProgress out;
  EXPECT_EQ(parse(R"({"progress":"/body/DocFragment[2]/bo)", out), KOReaderSyncClient::JSON_ERROR);
}
