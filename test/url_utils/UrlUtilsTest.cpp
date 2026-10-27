#include <gtest/gtest.h>

#include "UrlUtils.h"

namespace {

using UrlUtils::isForeignHost;

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
