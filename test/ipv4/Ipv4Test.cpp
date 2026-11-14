#include <gtest/gtest.h>

#include "Ipv4.h"

namespace {

TEST(Ipv4ParseCidr, ValidInput) {
  uint8_t ip[4];
  uint8_t prefix;
  EXPECT_TRUE(Ipv4::parseCidr("192.168.4.1/24", ip, prefix));
  EXPECT_EQ(ip[0], 192);
  EXPECT_EQ(ip[1], 168);
  EXPECT_EQ(ip[2], 4);
  EXPECT_EQ(ip[3], 1);
  EXPECT_EQ(prefix, 24);

  EXPECT_TRUE(Ipv4::parseCidr("0.0.0.0/0", ip, prefix));
  EXPECT_EQ(prefix, 0);

  EXPECT_TRUE(Ipv4::parseCidr("255.255.255.255/32", ip, prefix));
  EXPECT_EQ(prefix, 32);
}

TEST(Ipv4ParseCidr, RejectsMalformedInput) {
  uint8_t ip[4];
  uint8_t prefix;
  EXPECT_FALSE(Ipv4::parseCidr(nullptr, ip, prefix));
  EXPECT_FALSE(Ipv4::parseCidr("", ip, prefix));
  EXPECT_FALSE(Ipv4::parseCidr("192.168.4.1", ip, prefix));    // prefix required
  EXPECT_FALSE(Ipv4::parseCidr("192.168.4.1/", ip, prefix));   // empty prefix
  EXPECT_FALSE(Ipv4::parseCidr("192.168.4/24", ip, prefix));   // missing octet
  EXPECT_FALSE(Ipv4::parseCidr("192.168.4.1.5/24", ip, prefix));
  EXPECT_FALSE(Ipv4::parseCidr("192.168.4.256/24", ip, prefix));
  EXPECT_FALSE(Ipv4::parseCidr("192.168.4.1/33", ip, prefix));
  EXPECT_FALSE(Ipv4::parseCidr("192.168.4.1/-1", ip, prefix));
  EXPECT_FALSE(Ipv4::parseCidr("192.168.4.1/24x", ip, prefix));
  EXPECT_FALSE(Ipv4::parseCidr("a.b.c.d/24", ip, prefix));
  EXPECT_FALSE(Ipv4::parseCidr("192.168.4.1 /24", ip, prefix));
}

TEST(Ipv4ParseCidr, LeavesOutputsUntouchedOnFailure) {
  uint8_t ip[4] = {1, 2, 3, 4};
  uint8_t prefix = 24;
  EXPECT_FALSE(Ipv4::parseCidr("10.0.0.1/99", ip, prefix));
  EXPECT_EQ(ip[0], 1);
  EXPECT_EQ(prefix, 24);
}

TEST(Ipv4UsableHostIp, AcceptsOrdinaryHosts) {
  const uint8_t a[4] = {10, 0, 0, 1};
  EXPECT_TRUE(Ipv4::isUsableHostIp(a, 24));
  const uint8_t b[4] = {192, 168, 50, 100};
  EXPECT_TRUE(Ipv4::isUsableHostIp(b, 28));
}

TEST(Ipv4UsableHostIp, RejectsUnusableAddresses) {
  const uint8_t network[4] = {10, 0, 0, 0};
  EXPECT_FALSE(Ipv4::isUsableHostIp(network, 24));         // network address
  const uint8_t broadcast[4] = {10, 0, 0, 255};
  EXPECT_FALSE(Ipv4::isUsableHostIp(broadcast, 24));       // broadcast address
  const uint8_t bcast28[4] = {10, 0, 0, 15};
  EXPECT_FALSE(Ipv4::isUsableHostIp(bcast28, 28));         // /28 broadcast
  const uint8_t zero[4] = {0, 0, 0, 0};
  EXPECT_FALSE(Ipv4::isUsableHostIp(zero, 24));            // 0.0.0.0
  const uint8_t loopback[4] = {127, 0, 0, 1};
  EXPECT_FALSE(Ipv4::isUsableHostIp(loopback, 24));        // loopback range
  const uint8_t multicast[4] = {224, 0, 0, 1};
  EXPECT_FALSE(Ipv4::isUsableHostIp(multicast, 24));       // multicast range
  const uint8_t reserved[4] = {240, 0, 0, 1};
  EXPECT_FALSE(Ipv4::isUsableHostIp(reserved, 24));        // reserved range
  const uint8_t a[4] = {10, 0, 0, 1};
  EXPECT_FALSE(Ipv4::isUsableHostIp(a, 32));               // no host bits in /32
  EXPECT_FALSE(Ipv4::isUsableHostIp(nullptr, 24));
}

}  // namespace
