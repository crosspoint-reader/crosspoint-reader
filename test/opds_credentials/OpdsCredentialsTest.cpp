#include <HttpOrigin.h>
#include <OpdsClient.h>
#include <OpdsImplicitAuth.h>
#include <SecureHttpClient.h>
#include <gtest/gtest.h>

#include <deque>
#include <string>
#include <vector>

// Exercise the real implicit-login HTTP state machine with scripted transport replies.
namespace freeink {
SecureClient::~SecureClient() { stop(); }
void SecureClient::setCACert(const char*) {}
void SecureClient::setInsecure() {}
int SecureClient::connect(IPAddress ip, uint16_t port) { return _transport.connect(ip, port); }
int SecureClient::connect(const char* host, uint16_t port) { return _transport.connect(host, port); }
size_t SecureClient::write(uint8_t b) { return _transport.write(b); }
size_t SecureClient::write(const uint8_t* data, size_t length) { return _transport.write(data, length); }
int SecureClient::available() { return _transport.available(); }
int SecureClient::read() { return _transport.read(); }
int SecureClient::read(uint8_t* data, size_t length) { return _transport.read(data, length); }
int SecureClient::peek() { return _transport.peek(); }
void SecureClient::flush() {}
void SecureClient::stop() { _transport.stop(); }
uint8_t SecureClient::connected() { return _transport.connected(); }
bool SecureClient::tls13Available() { return false; }
}  // namespace freeink

namespace {
using namespace freeink::opds;
const std::string feed = R"({"metadata":{"title":"Catalog"},"publications":[]})";
struct Reply {
  int status;
  std::string body;
};
struct Request {
  std::string url;
  HttpAuth auth;
};
class Transport : public OpdsTransport {
 public:
  std::deque<Reply> replies;
  std::vector<Request> requests;
  std::vector<std::string> posts;
  HttpResult get(const std::string& url, const HttpAuth& auth, const char*, const char*, bool,
                 const OpdsDataSink& sink) override {
    requests.push_back({url, auth});
    Reply reply{200, feed};
    if (!replies.empty()) {
      reply = std::move(replies.front());
      replies.pop_front();
    }
    sink(reinterpret_cast<const uint8_t*>(reply.body.data()), reply.body.size());
    return {reply.status >= 200 && reply.status < 300, reply.status};
  }
  bool postForm(const std::string& url, const std::string&, std::string& response, int& status) override {
    posts.push_back(url);
    response = R"({"access_token":"new-token","refresh_token":"new-refresh"})";
    status = 200;
    return true;
  }
  std::string resolveUrl(const std::string& base, const std::string& ref) override {
    std::string result;
    freeink::SecureHttpClient::resolveUrl(base, ref, result);
    return result;
  }
};
void expectEmpty(const HttpAuth& auth) {
  EXPECT_TRUE(auth.username.empty());
  EXPECT_TRUE(auth.password.empty());
  EXPECT_TRUE(auth.bearer.empty());
  EXPECT_TRUE(auth.headers.empty());
}
std::string authDoc(const std::string& kind, const std::string& endpoint) {
  return "{\"authentication\":[{\"type\":\"http://opds-spec.org/auth/oauth/" + kind +
         "\",\"links\":[{\"rel\":\"authenticate\",\"href\":\"" + endpoint + "\"}]}]}";
}
void resetNet(std::initializer_list<std::string> replies) {
  FakeNet::replies().assign(replies.begin(), replies.end());
  FakeNet::requests().clear();
  FakeNet::hosts().clear();
  fakeNow() = 0;
}
std::string htmlResponse(const std::string& html) {
  return "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(html.size()) + "\r\n\r\n" + html;
}
}  // namespace

TEST(OpdsCredentials, AllInitialRequestKindsScopeBearerAndCustomHeaders) {
  for (const std::string& host : {"https://catalog.example:443/", "https://other.example/"}) {
    Transport transport;
    OpdsClient client(transport);
    client.setServer("HTTPS://CATALOG.EXAMPLE/books", "user", "secret", {{"X-Key", "key"}});
    client.setTokens("token", "refresh", "https://catalog.example/token");
    OpdsFeedParser parser;
    EXPECT_EQ(client.fetchFeed(host, parser), OpdsClient::FetchStatus::Ok);
    OpdsPublication publication;
    client.fetchPublication(host, publication);
    std::string output;
    bool epub = false;
    client.resolveIndirect(host, output, epub);
    client.fetchSearchTemplate(host, output);
    ASSERT_EQ(transport.requests.size(), 4u);
    for (const auto& request : transport.requests) {
      if (host.find("other") != std::string::npos)
        expectEmpty(request.auth);
      else {
        EXPECT_EQ(request.auth.bearer, "token");
        ASSERT_EQ(request.auth.headers.size(), 1u);
        EXPECT_EQ(request.auth.headers[0].second, "key");
      }
    }
  }
}

TEST(OpdsCredentials, BasicAndDownloadCredentialsFollowTheDestination) {
  Transport transport;
  transport.replies = {{401, ""}, {401, ""}, {200, feed}};
  OpdsClient client(transport);
  client.setServer("catalog.example/books", "user", "secret", {{"X-Key", "key"}});
  OpdsFeedParser parser;
  ASSERT_EQ(client.fetchFeed("http://catalog.example/feed", parser), OpdsClient::FetchStatus::Ok);
  ASSERT_TRUE(client.useBasicAuth());
  const auto auth = client.downloadAuth("http://CATALOG.example:80/book");
  EXPECT_EQ(auth.username, "user");
  EXPECT_EQ(auth.password, "secret");
  ASSERT_EQ(auth.headers.size(), 1u);
  for (const char* url : {"http://other.example/book", "https://catalog.example/book", "http://catalog.example:81/book",
                          "http://catalog.example@other.example/book", "http://catalog.example:99999/book"})
    expectEmpty(client.downloadAuth(url));
}

TEST(OpdsCredentials, AnExternalChallengeDoesNotMutateOrRefreshSavedTokens) {
  Transport transport;
  transport.replies = {{401, authDoc("password", "https://other.example/token")}};
  OpdsClient client(transport);
  client.setServer("https://catalog.example/", "user", "secret", {{"X-Key", "key"}});
  client.setTokens("saved", "refresh", "https://catalog.example/token");
  OpdsFeedParser parser;
  EXPECT_EQ(client.fetchFeed("https://other.example/feed", parser), OpdsClient::FetchStatus::ExternalAuthRequired);
  ASSERT_EQ(transport.requests.size(), 1u);
  expectEmpty(transport.requests[0].auth);
  EXPECT_TRUE(transport.posts.empty());
  EXPECT_FALSE(client.tokensDirty());
  EXPECT_FALSE(client.credentialsMissing());
  EXPECT_EQ(client.accessToken(), "saved");
  EXPECT_EQ(client.refreshToken(), "refresh");
}

TEST(OpdsCredentials, PasswordAndImplicitEndpointsCannotLeaveTheSavedOrigin) {
  for (const char* kind : {"password", "implicit"})
    for (const char* url : {"https://other.example/token", "http://catalog.example/token",
                            "https://catalog.example:444/token", "https://catalog.example@other.example/token"}) {
      Transport transport;
      transport.replies = {{401, ""}, {401, authDoc(kind, url)}};
      OpdsClient client(transport);
      client.setServer("https://catalog.example/", "user", "secret");
      OpdsFeedParser parser;
      EXPECT_EQ(client.fetchFeed("https://catalog.example/feed", parser), OpdsClient::FetchStatus::AuthFailed);
      EXPECT_TRUE(transport.posts.empty());
      ASSERT_EQ(transport.requests.size(), 2u);
    }
}

TEST(OpdsCredentials, SameOriginPasswordGrantStillSucceeds) {
  Transport transport;
  transport.replies = {{401, ""}, {401, authDoc("password", "/token")}, {200, feed}};
  OpdsClient client(transport);
  client.setServer("https://catalog.example/", "user", "secret");
  OpdsFeedParser parser;
  ASSERT_EQ(client.fetchFeed("https://catalog.example/feed", parser), OpdsClient::FetchStatus::Ok);
  ASSERT_EQ(transport.posts.size(), 1u);
  EXPECT_EQ(transport.posts[0], "https://catalog.example/token");
  EXPECT_EQ(client.accessToken(), "new-token");
}

TEST(OpdsCredentials, PersistedExternalRefreshEndpointsReceiveNoToken) {
  for (const char* url :
       {"https://other.example/token", "http://catalog.example/token", "https://catalog.example:444/token"}) {
    Transport transport;
    transport.replies = {{401, ""}, {401, ""}, {200, feed}};
    OpdsClient client(transport);
    client.setServer("https://catalog.example/", "user", "secret");
    client.setTokens("expired", "refresh", url);
    OpdsFeedParser parser;
    EXPECT_EQ(client.fetchFeed("https://catalog.example/feed", parser), OpdsClient::FetchStatus::Ok);
    EXPECT_TRUE(transport.posts.empty());
  }
}

TEST(OpdsCredentials, ImplicitRedirectsAndFormsStopBeforeExternalRequests) {
  for (const std::string& response :
       {std::string("HTTP/1.1 302 Found\r\nLocation: http://other.example/login\r\nContent-Length: 0\r\n\r\n"),
        htmlResponse("<form method='post' action='http://other.example/login'><input name='username'><input "
                     "type='password' name='password'></form>")}) {
    resetNet({response});
    std::string token;
    EXPECT_FALSE(opdsImplicitAuthenticate("http://catalog.example/auth", "user", "secret", token));
    ASSERT_EQ(FakeNet::requests().size(), 1u);
    EXPECT_EQ(FakeNet::hosts()[0], "catalog.example");
    EXPECT_EQ(FakeNet::requests()[0].find("secret"), std::string::npos);
  }
}

TEST(OpdsCredentials, SameOriginIdentifierFirstImplicitLoginStillSucceeds) {
  resetNet({htmlResponse("<form method='post' action='/email'><input type='text' name='username'></form>"),
            htmlResponse("<form method='post' action='/password'><input type='password' name='password'></form>"),
            "HTTP/1.1 302 Found\r\nLocation: opds://authorize/#access_token=token\r\nContent-Length: 0\r\n\r\n"});
  std::string token;
  ASSERT_TRUE(opdsImplicitAuthenticate("http://catalog.example/auth", "user", "secret", token));
  EXPECT_EQ(token, "token");
  ASSERT_EQ(FakeNet::requests().size(), 3u);
  EXPECT_NE(FakeNet::requests()[1].find("username=user"), std::string::npos);
  EXPECT_NE(FakeNet::requests()[2].find("password=secret"), std::string::npos);
  for (const auto& host : FakeNet::hosts()) EXPECT_EQ(host, "catalog.example");
}
