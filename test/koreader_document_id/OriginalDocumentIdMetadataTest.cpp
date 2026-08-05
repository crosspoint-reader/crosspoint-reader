#include <gtest/gtest.h>

#include <string>

#include "OriginalDocumentIdMetadata.h"

TEST(OriginalDocumentIdMetadata, ReadsOptimizerMetadata) {
  const char* attributes[] = {
      "name", "crosspoint:original-koreader-document-id", "content", "0123456789abcdef0123456789abcdef", nullptr,
  };

  std::string documentId;
  EXPECT_TRUE(extractOriginalDocumentIdMetadata(attributes, documentId));
  EXPECT_EQ(documentId, "0123456789abcdef0123456789abcdef");
}

TEST(OriginalDocumentIdMetadata, HandlesAttributeOrderAndNormalizesUppercase) {
  const char* attributes[] = {
      "content", "ABCDEF0123456789ABCDEF0123456789", "name", "crosspoint:original-koreader-document-id", nullptr,
  };

  std::string documentId;
  EXPECT_TRUE(extractOriginalDocumentIdMetadata(attributes, documentId));
  EXPECT_EQ(documentId, "abcdef0123456789abcdef0123456789");
}

TEST(OriginalDocumentIdMetadata, RejectsMalformedDocumentIds) {
  for (const char* value : {
           "short",
           "0123456789abcdef0123456789abcdeg",
           " 0123456789abcdef0123456789abcdef",
       }) {
    const char* attributes[] = {
        "name", "crosspoint:original-koreader-document-id", "content", value, nullptr,
    };

    std::string documentId = "unchanged";
    EXPECT_FALSE(extractOriginalDocumentIdMetadata(attributes, documentId)) << value;
    EXPECT_EQ(documentId, "unchanged") << value;
  }
}

TEST(OriginalDocumentIdMetadata, IgnoresUnrelatedMetadata) {
  const char* attributes[] = {
      "name", "cover", "content", "0123456789abcdef0123456789abcdef", nullptr,
  };

  std::string documentId;
  EXPECT_FALSE(extractOriginalDocumentIdMetadata(attributes, documentId));
  EXPECT_TRUE(documentId.empty());
}

TEST(OriginalDocumentIdMetadata, RequiresContentAttribute) {
  const char* attributes[] = {
      "name",
      "crosspoint:original-koreader-document-id",
      nullptr,
  };

  std::string documentId;
  EXPECT_FALSE(extractOriginalDocumentIdMetadata(attributes, documentId));
  EXPECT_TRUE(documentId.empty());
}
