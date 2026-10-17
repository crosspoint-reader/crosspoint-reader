// Host tests: SecureHttpClient's credentials (basic auth and the addHeader() headers, which carry
// KOSync's x-auth-user and x-auth-key) go only to the origin a request starts at, never past a
// redirect to another one. Over the scripted network of RangeDownloadTest (stubs/WiFiClient.h); the
// stub SecureClient is defined there.

#include <SecureHttpClient.h>
#include <WiFiClient.h>
#include <gtest/gtest.h>

#include <initializer_list>
#include <string>

namespace {

bool hasHeader(size_t request, const std::string& name) {
  return FakeNet::requests().at(request).find("\r\n" + name + ": ") != std::string::npos;
}

// All three credentials on request i, or none of them: basic auth and the custom headers are one
// set and go, or stay, together.
::testing::AssertionResult credentialsSent(size_t i) {
  const bool auth = hasHeader(i, "Authorization");
  const bool user = hasHeader(i, "x-auth-user");
  const bool key = hasHeader(i, "x-auth-key");
  if (auth && user && key) return ::testing::AssertionSuccess();
  return ::testing::AssertionFailure() << "request " << i << " lacks credentials:\n" << FakeNet::requests().at(i);
}
::testing::AssertionResult noCredentialsSent(size_t i) {
  const bool any = hasHeader(i, "Authorization") || hasHeader(i, "x-auth-user") || hasHeader(i, "x-auth-key");
  if (!any) return ::testing::AssertionSuccess();
  return ::testing::AssertionFailure() << "request " << i << " carries credentials:\n" << FakeNet::requests().at(i);
}

FakeReply redirectTo(const std::string& location, int status = 302) {
  return {"HTTP/1.1 " + std::to_string(status) + " Moved\r\nLocation: " + location + "\r\nContent-Length: 0\r\n\r\n",
          FakeReply::End::KeepOpen};
}
FakeReply ok() { return {"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok", FakeReply::End::KeepOpen}; }

struct CredentialRedirect : ::testing::Test {
  crosspoint::SecureHttpClient http;

  void SetUp() override {
    FakeNet::reset();
    http.setTimeout(5000);
    http.setBasicAuth("reader", "secret");
    http.addHeader("x-auth-user", "reader");
    http.addHeader("x-auth-key", "5ebe2294ecd0e0f08eab7690d2a6ee69");
  }

  // GET url through redirects to each of locations in turn, then a 200.
  int getThrough(const std::string& url, std::initializer_list<const char*> locations) {
    for (const char* location : locations) FakeNet::replies().push_back(redirectTo(location));
    FakeNet::replies().push_back(ok());
    return http.get(url, [](const uint8_t*, size_t) { return true; });
  }
};

TEST_F(CredentialRedirect, ARedirectOnTheSameOriginKeepsThem) {
  EXPECT_EQ(getThrough("https://opds.example/feed", {"/books/1.epub"}), 200);
  ASSERT_EQ(FakeNet::requests().size(), 2u);
  EXPECT_TRUE(credentialsSent(0));
  EXPECT_TRUE(credentialsSent(1));
}

TEST_F(CredentialRedirect, ARedirectToAnotherHostDropsThem) {
  EXPECT_EQ(getThrough("https://opds.example/books/1", {"https://cdn.example/1.epub"}), 200);
  ASSERT_EQ(FakeNet::requests().size(), 2u);
  EXPECT_TRUE(credentialsSent(0));
  EXPECT_TRUE(noCredentialsSent(1));
  EXPECT_EQ(FakeNet::requests()[1].rfind("GET /1.epub ", 0), 0u);  // the redirect is still followed
}

TEST_F(CredentialRedirect, HostCaseAndAnExplicitDefaultPortAreTheSameOrigin) {
  EXPECT_EQ(getThrough("https://opds.example/feed", {"https://OPDS.Example:443/books/1.epub"}), 200);
  EXPECT_TRUE(credentialsSent(1));
}

TEST_F(CredentialRedirect, AnotherPortIsAnotherOrigin) {
  EXPECT_EQ(getThrough("https://opds.example/feed", {"https://opds.example:8443/feed"}), 200);
  EXPECT_TRUE(noCredentialsSent(1));
}

// A sync URL typed without a scheme starts as http; its server upgrading to https keeps the login.
TEST_F(CredentialRedirect, AnUpgradeToHttpsOnTheSameHostKeepsThem) {
  EXPECT_EQ(getThrough("http://sync.example/users/auth", {"https://sync.example/users/auth"}), 200);
  EXPECT_TRUE(credentialsSent(1));
}

TEST_F(CredentialRedirect, AnUpgradeOffTheDefaultPortsDropsThem) {
  EXPECT_EQ(getThrough("http://sync.example/users/auth", {"https://sync.example:8443/users/auth"}), 200);
  EXPECT_TRUE(noCredentialsSent(1));
  FakeNet::reset();
  EXPECT_EQ(getThrough("http://sync.example:8080/users/auth", {"https://sync.example/users/auth"}), 200);
  EXPECT_TRUE(noCredentialsSent(1));
}

TEST_F(CredentialRedirect, AnUpgradeToAnotherHostDropsThem) {
  EXPECT_EQ(getThrough("http://sync.example/users/auth", {"https://other.example/users/auth"}), 200);
  EXPECT_TRUE(noCredentialsSent(1));
}

TEST_F(CredentialRedirect, ADowngradeToHttpDropsThem) {
  http.setAllowRedirectDowngrade(true);
  EXPECT_EQ(getThrough("https://opds.example/feed", {"http://opds.example/feed"}), 200);
  EXPECT_TRUE(noCredentialsSent(1));
}

TEST_F(CredentialRedirect, TheyStayOffWhenALaterHopComesBack) {
  EXPECT_EQ(getThrough("https://opds.example/a", {"https://cdn.example/b", "https://opds.example/c"}), 200);
  ASSERT_EQ(FakeNet::requests().size(), 3u);
  EXPECT_TRUE(credentialsSent(0));
  EXPECT_TRUE(noCredentialsSent(1));
  EXPECT_TRUE(noCredentialsSent(2));
}

TEST_F(CredentialRedirect, TheNextRequestStartsWithThemAgain) {
  EXPECT_EQ(getThrough("https://opds.example/books/1", {"https://cdn.example/1.epub"}), 200);
  EXPECT_EQ(getThrough("https://opds.example/feed", {}), 200);
  ASSERT_EQ(FakeNet::requests().size(), 3u);
  EXPECT_TRUE(credentialsSent(2));
}

TEST_F(CredentialRedirect, TheRangeFollowsWithoutThem) {
  http.setRange(0, 99);
  EXPECT_EQ(getThrough("https://opds.example/books/1", {"https://cdn.example/1.epub"}), 200);
  EXPECT_TRUE(noCredentialsSent(1));
  EXPECT_TRUE(hasHeader(1, "Range"));
  EXPECT_TRUE(hasHeader(1, "User-Agent"));
}

// KOSync's progress update: a 307 keeps the method and the body, never the login.
TEST_F(CredentialRedirect, ARedirectedPutKeepsItsBodyAndDropsThem) {
  FakeNet::replies().push_back(redirectTo("https://elsewhere.example/syncs/progress", 307));
  FakeNet::replies().push_back(ok());
  EXPECT_EQ(http.PUT("https://sync.example/syncs/progress", "{\"progress\":\"1\"}"), 200);
  ASSERT_EQ(FakeNet::requests().size(), 2u);
  EXPECT_TRUE(credentialsSent(0));
  EXPECT_EQ(FakeNet::requests()[1].rfind("PUT /syncs/progress ", 0), 0u);
  EXPECT_TRUE(hasHeader(1, "Content-Length"));
  EXPECT_TRUE(noCredentialsSent(1));
}

TEST_F(CredentialRedirect, APinnedScopeHoldsForARequestThatStartsElsewhere) {
  http.setCredentialScope("https://opds.example/books/1");
  EXPECT_EQ(getThrough("https://cdn.example/1.epub", {}), 200);
  EXPECT_EQ(getThrough("https://OPDS.example/books/2", {}), 200);
  http.clearCredentialScope();
  EXPECT_EQ(getThrough("https://cdn.example/1.epub", {}), 200);
  ASSERT_EQ(FakeNet::requests().size(), 3u);
  EXPECT_TRUE(noCredentialsSent(0));
  EXPECT_TRUE(credentialsSent(1));
  EXPECT_TRUE(credentialsSent(2));  // without the pin, a request's own URL is the scope
}

TEST_F(CredentialRedirect, AnUnparseablePinnedScopeSendsThemNowhere) {
  http.setCredentialScope("not a url");
  EXPECT_EQ(getThrough("https://opds.example/feed", {}), 200);
  EXPECT_TRUE(noCredentialsSent(0));
}

}  // namespace
