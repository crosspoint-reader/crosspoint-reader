#include <gtest/gtest.h>

#include "CrossPointSettings.h"
#include "ExternalServices.h"
#include "Network.h"
#include "WiFi.h"
#include "esp_sntp.h"

namespace {

void resetNetwork() {
  SETTINGS.externalServicesEnabled = 0;
  WiFi = FakeWiFiClass{};
  Network = FakeNetworkClass{};
  fakeSntpRunning = false;
  fakeSntpStops = 0;
  WiFi.status_ = WL_CONNECTED;
  WiFi.mode_ = WIFI_MODE_STA;
  WiFi.local_ = IPAddress(192, 168, 50, 20);
  WiFi.mask_ = IPAddress(255, 255, 255, 0);
  WiFi.dns_ = IPAddress(192, 168, 50, 1);
}

void expectAddress(const IPAddress& address, int a, int b, int c, int d) {
  EXPECT_EQ(address[0], a);
  EXPECT_EQ(address[1], b);
  EXPECT_EQ(address[2], c);
  EXPECT_EQ(address[3], d);
}

class ExternalServicesTest : public testing::Test {
 protected:
  void SetUp() override { resetNetwork(); }
};

TEST_F(ExternalServicesTest, DefaultsOffAndAllowsOnlyCurrentLanLiterals) {
  IPAddress address;
  EXPECT_FALSE(externalservices::enabled());
  EXPECT_TRUE(externalservices::resolveDestination("192.168.50.44", address));
  expectAddress(address, 192, 168, 50, 44);

  EXPECT_FALSE(externalservices::resolveDestination("192.168.51.44", address));
  EXPECT_FALSE(externalservices::resolveDestination("8.8.8.8", address));
  EXPECT_TRUE(externalservices::resolveDestination("127.0.0.1", address));
  EXPECT_TRUE(externalservices::resolveDestination("169.254.2.3", address));
  EXPECT_EQ(Network.lookups, 0);
}

TEST_F(ExternalServicesTest, NormalizesMdnsAndResolvesLocalhostWithoutPublicDns) {
  WiFi.dns_ = IPAddress(8, 8, 8, 8);
  Network.answers["books.local"] = IPAddress(192, 168, 50, 77);
  IPAddress address;
  EXPECT_TRUE(externalservices::resolveDestination("Books.LOCAL", address));
  expectAddress(address, 192, 168, 50, 77);
  EXPECT_EQ(Network.lookups, 1);

  EXPECT_TRUE(externalservices::resolveDestination("LOCALHOST", address));
  expectAddress(address, 127, 0, 0, 1);
  EXPECT_EQ(Network.lookups, 1);
}

TEST_F(ExternalServicesTest, RejectsNonMdnsHostnamesWithoutDns) {
  Network.answers["remote.example"] = IPAddress(192, 168, 50, 77);
  IPAddress address;
  EXPECT_FALSE(externalservices::resolveDestination("remote.example", address));
  EXPECT_EQ(Network.lookups, 0);

  WiFi.dns_ = IPAddress(8, 8, 8, 8);
  EXPECT_FALSE(externalservices::resolveDestination("another.example", address));
  EXPECT_EQ(Network.lookups, 0);
}

TEST_F(ExternalServicesTest, OptInAllowsPublicResolution) {
  SETTINGS.externalServicesEnabled = 1;
  Network.answers["remote.example"] = IPAddress(203, 0, 113, 9);
  IPAddress address;
  EXPECT_TRUE(externalservices::enabled());
  EXPECT_TRUE(externalservices::resolveDestination("remote.example", address));
  expectAddress(address, 203, 0, 113, 9);
}

TEST_F(ExternalServicesTest, TurningOffStopsAnActiveSntpClient) {
  SETTINGS.externalServicesEnabled = 1;
  fakeSntpRunning = true;
  SETTINGS.externalServicesEnabled = 0;
  externalservices::settingChanged();
  EXPECT_FALSE(fakeSntpRunning);
  EXPECT_EQ(fakeSntpStops, 1);
}

TEST_F(ExternalServicesTest, HotspotPeersRemainLocal) {
  WiFi.status_ = WL_DISCONNECTED;
  WiFi.mode_ = WIFI_MODE_AP;
  WiFi.ap_ = IPAddress(192, 168, 4, 1);
  IPAddress address;
  EXPECT_TRUE(externalservices::resolveDestination("192.168.4.2", address));
  EXPECT_FALSE(externalservices::resolveDestination("192.168.5.2", address));
}

}  // namespace
