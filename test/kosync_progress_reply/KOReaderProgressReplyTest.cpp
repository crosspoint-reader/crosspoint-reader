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

// Calibre-Web NextGen's percentage-only record (its web reader or a Kobo wrote it), served to a
// client that asked for position_kinds=locator,percentage. ProgressMapper places it by percentage.
TEST(KOReaderProgressReply, PercentageOnlyRecordIsRead) {
  KOReaderProgress out;
  ASSERT_EQ(parse(R"({"document":"x","progress":null,"position_kind":"percentage","percentage":0.4567,)"
                  R"("device":"Web reader","device_id":"cwng-web","timestamp":1700000000})",
                  out),
            KOReaderSyncClient::OK);
  EXPECT_EQ(out.progress, "");  // not "null": that would reach the XPath mapper
  EXPECT_FLOAT_EQ(out.percentage, 0.4567f);
  EXPECT_EQ(out.device, "Web reader");
  EXPECT_EQ(out.deviceId, "cwng-web");
}

// The same position from a server that leaves the field out, or stores it as "".
TEST(KOReaderProgressReply, PercentageWithoutXPathIsRead) {
  for (const char* body : {R"({"percentage":0.25})", R"({"progress":"","percentage":0.25})"}) {
    KOReaderProgress out;
    ASSERT_EQ(parse(body, out), KOReaderSyncClient::OK) << body;
    EXPECT_EQ(out.progress, "") << body;
    EXPECT_FLOAT_EQ(out.percentage, 0.25f) << body;
  }
}

// A record at the very start is still a record.
TEST(KOReaderProgressReply, PercentageZeroIsARecord) {
  KOReaderProgress out;
  EXPECT_EQ(parse(R"({"progress":null,"percentage":0})", out), KOReaderSyncClient::OK);
}

// Calibre-Web NextGen's answer to a client that did not ask for the kind of position it holds:
// nothing to apply, so no record, and never a position at 0 %.
TEST(KOReaderProgressReply, WithheldPositionIsNotFound) {
  KOReaderProgress out;
  EXPECT_EQ(parse(R"({"position_kinds_available":["percentage"]})", out), KOReaderSyncClient::NOT_FOUND);
}

// Objects that hold neither an XPath nor a percentage.
TEST(KOReaderProgressReply, ObjectWithoutAPositionIsNotFound) {
  for (const char* body : {R"({"document":"x"})", R"({"progress":null})", R"({"progress":""})",
                           R"({"progress":null,"percentage":null,"device":"KOReader"})"}) {
    KOReaderProgress out;
    EXPECT_EQ(parse(body, out), KOReaderSyncClient::NOT_FOUND) << body;
  }
}

// koreader-sync-server leaves out a field it has no value for; that is an empty string here, not
// "null", which the sync screen would print as "from null".
TEST(KOReaderProgressReply, MissingDeviceFieldsReadEmpty) {
  KOReaderProgress out;
  ASSERT_EQ(parse(R"({"progress":"/body/DocFragment[3]/body/p[7]/text().0","percentage":0.1})", out),
            KOReaderSyncClient::OK);
  EXPECT_EQ(out.device, "");
  EXPECT_EQ(out.deviceId, "");
}

TEST(KOReaderProgressReply, TruncatedJsonIsAJsonError) {
  KOReaderProgress out;
  EXPECT_EQ(parse(R"({"progress":"/body/DocFragment[2]/bo)", out), KOReaderSyncClient::JSON_ERROR);
}
