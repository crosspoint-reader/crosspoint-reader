#include <gtest/gtest.h>

#include "UrlUtils.h"

namespace {

using UrlUtils::isForeignHost;
using UrlUtils::resolveServerPath;

std::string resolved(std::string path, const std::string& requestUrl) {
  resolveServerPath(path, requestUrl);
  return path;
}

TEST(ResolveServerPath, ServerPathTakesTheRequestOrigin) {
  std::string path = "/api/files/12/download";
  EXPECT_TRUE(resolveServerPath(path, "https://books.example.com/api/books/7"));
  EXPECT_EQ(resolved("/api/files/12/download", "https://books.example.com/api/books/7"),
            "https://books.example.com/api/files/12/download");
  EXPECT_EQ(resolved("/f?a=1", "http://192.168.1.5:6060/api?x=/y"), "http://192.168.1.5:6060/f?a=1");
  EXPECT_EQ(resolved("/f", "https://h.example?q=1"), "https://h.example/f");
  EXPECT_EQ(resolved("/f", "https://h.example#top"), "https://h.example/f");
  EXPECT_EQ(resolved("/f", "https://h.example"), "https://h.example/f");
}

TEST(ResolveServerPath, OtherValuesAreLeftAlone) {
  std::string path = "https://cdn.example/f";
  EXPECT_FALSE(resolveServerPath(path, "https://h.example/a"));
  EXPECT_EQ(path, "https://cdn.example/f");
  EXPECT_EQ(resolved("//cdn.example/f", "https://h.example/a"), "//cdn.example/f");
  EXPECT_EQ(resolved("files/12", "https://h.example/a/"), "files/12");
  EXPECT_EQ(resolved("", "https://h.example/a"), "");
  EXPECT_EQ(resolved("/f", "h.example/a"), "/f");
}

TEST(IsForeignHost, CaptivePortalChecksAreForeign) {
  EXPECT_TRUE(isForeignHost("connectivitycheck.gstatic.com"));
  EXPECT_TRUE(isForeignHost("www.google.com"));
  EXPECT_TRUE(isForeignHost("captive.apple.com"));
  EXPECT_TRUE(isForeignHost("www.msftconnecttest.com"));
  EXPECT_TRUE(isForeignHost("detectportal.firefox.com:80"));
}

TEST(IsForeignHost, ReaderAddressesAreNotForeign) {
  EXPECT_FALSE(isForeignHost("192.168.4.1"));
  EXPECT_FALSE(isForeignHost("192.168.4.1:80"));
  EXPECT_FALSE(isForeignHost("crosspoint.local"));
  EXPECT_FALSE(isForeignHost("CrossPoint.LOCAL:8080"));
  EXPECT_FALSE(isForeignHost("[fe80::1]:80"));
}

TEST(IsForeignHost, MissingHostIsNotForeign) {
  EXPECT_FALSE(isForeignHost(nullptr));
  EXPECT_FALSE(isForeignHost(""));
  EXPECT_FALSE(isForeignHost(":80"));
}

TEST(IsForeignHost, SingleLabelNamesAreTheReader) {
  EXPECT_FALSE(isForeignHost("crosspoint"));
  EXPECT_FALSE(isForeignHost("crosspoint:80"));
}

TEST(IsForeignHost, LocalSuffixMustBeWhole) {
  EXPECT_TRUE(isForeignHost("notlocal.com"));
  EXPECT_TRUE(isForeignHost("example.localhost"));
  EXPECT_FALSE(isForeignHost(".local"));
}

}  // namespace
