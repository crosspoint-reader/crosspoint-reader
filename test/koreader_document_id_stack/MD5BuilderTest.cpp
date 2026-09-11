#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

#include <array>
#include <string>

#include "Fixture.h"
#include "KOReaderDocumentId.h"
#include "MD5Builder.h"

namespace {

using documentIdFixture::Md5Operation;

class Md5AdapterTest : public testing::TestWithParam<Md5Operation> {
 protected:
  void SetUp() override { documentIdFixture::state = {}; }
};

TEST_P(Md5AdapterTest, DocumentIdReportsAdapterFailureAndClosesResources) {
  auto& fixture = documentIdFixture::state;
  fixture.fileSize = 1025;
  fixture.failedMd5Operation = GetParam();
  std::string result;
  EXPECT_NONFATAL_FAILURE(result = KOReaderDocumentId::calculate("/fixture/book.epub"), "MD5Builder adapter: EVP_");
  EXPECT_TRUE(result.empty());
  EXPECT_EQ(fixture.closedFiles, 1);
  const unsigned contexts = GetParam() == Md5Operation::Context ? 0 : 1;
  EXPECT_EQ(fixture.md5ContextsCreated, contexts);
  EXPECT_EQ(fixture.md5ContextsFreed, contexts);
  constexpr std::array<unsigned, 4> expectedCalls[] = {{1, 0, 0, 0}, {1, 1, 0, 0}, {1, 1, 1, 0}, {1, 1, 2, 1}};
  EXPECT_EQ(fixture.md5Calls, expectedCalls[static_cast<size_t>(GetParam())]);
}

TEST_P(Md5AdapterTest, FailureStaysReportedOnceAndClearsAnyDigest) {
  auto& fixture = documentIdFixture::state;
  if (GetParam() == Md5Operation::Context) fixture.failedMd5Operation = GetParam();
  EXPECT_NONFATAL_FAILURE(
      {
        MD5Builder md5;
        if (GetParam() != Md5Operation::Context) {
          md5.begin();
          md5.add("abc");
          md5.calculate();
          EXPECT_EQ(md5.toString(), "900150983cd24fb0d6963f7d28e17f72");
          fixture.failedMd5Operation = GetParam();
          md5.begin();
          md5.add("abc");
          md5.calculate();
        }
        EXPECT_TRUE(md5.toString().empty());
        const auto callsAfterFailure = fixture.md5Calls;
        fixture.failedMd5Operation.reset();
        md5.begin();
        md5.add("abc");
        md5.calculate();
        EXPECT_TRUE(md5.toString().empty());
        EXPECT_EQ(fixture.md5Calls, callsAfterFailure);
      },
      "MD5Builder adapter: EVP_");
  const unsigned contexts = GetParam() == Md5Operation::Context ? 0 : 1;
  EXPECT_EQ(fixture.md5ContextsCreated, contexts);
  EXPECT_EQ(fixture.md5ContextsFreed, contexts);
}

std::string operationName(const testing::TestParamInfo<Md5Operation>& info) {
  constexpr const char* names[] = {"Context", "Initialize", "Update", "Finalize"};
  return names[static_cast<size_t>(info.param)];
}

INSTANTIATE_TEST_SUITE_P(DocumentIdStack, Md5AdapterTest,
                         testing::Values(Md5Operation::Context, Md5Operation::Initialize, Md5Operation::Update,
                                         Md5Operation::Finalize),
                         operationName);

}  // namespace
