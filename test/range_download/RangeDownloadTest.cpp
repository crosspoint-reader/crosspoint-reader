// Host tests for the download loop (lib/SecureNet RangeDownload) over a scripted network.
//
// The harness is ported from Free-Ink/freeink-sdk f80a99c (libs/network/SecureNet/test/host,
// Justin Mitchell): a fake WiFiClient with scripted server replies and a virtual clock (stubs/).
// Adapted here to SecureHttpClient's keep-alive and to https: the stub SecureClient below runs over
// the same fake transport, without TLS, and reports an out-of-memory read as wolfSSL's MEMORY_E.

#include <HttpRange.h>
#include <RangeDownload.h>
#include <SecureClient.h>
#include <SecureHttpClient.h>
#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

namespace crosspoint {
namespace {
constexpr int FAKE_MEMORY_E = -125;  // wolfSSL MEMORY_E
}  // namespace

SecureClient::~SecureClient() { stop(); }
bool SecureClient::tls13Available() { return false; }
int SecureClient::connect(IPAddress, uint16_t) { return 0; }
int SecureClient::connect(const char* host, uint16_t port) {
  _lastReadErr = 0;
  _connected = _transport.connect(host, port) == 1;
  return _connected ? 1 : 0;
}
size_t SecureClient::write(uint8_t b) { return write(&b, 1); }
size_t SecureClient::write(const uint8_t* buf, size_t size) { return _connected ? _transport.write(buf, size) : 0; }
int SecureClient::available() { return _connected ? _transport.available() : 0; }
int SecureClient::read() {
  uint8_t b;
  return read(&b, 1) == 1 ? b : -1;
}
// Like the real read(): data, 0 for "nothing yet" or a peer close (then disconnected), -1 for a hard
// failure, here only the scripted out-of-memory one.
int SecureClient::read(uint8_t* buf, size_t size) {
  if (!_connected) return -1;
  if (_transport.available() > 0) return _transport.read(buf, size);
  if (_transport.outOfMemoryNow()) {
    _lastReadErr = FAKE_MEMORY_E;
    _connected = false;
    return -1;
  }
  if (!_transport.connected()) _connected = false;
  return 0;
}
int SecureClient::peek() { return -1; }
void SecureClient::flush() {}
void SecureClient::stop() {
  _transport.stop();
  _connected = false;
}
uint8_t SecureClient::connected() { return _connected && _transport.connected(); }
bool SecureClient::lastReadWasOutOfMemory() const { return _lastReadErr == FAKE_MEMORY_E; }
}  // namespace crosspoint

namespace hr = crosspoint::http_range;

namespace {

size_t g_largest = 16372;  // a largest free block the X3 showed after a handshake
size_t fakeLargest() { return g_largest; }

std::string makeResource(size_t size, char seed = 'a') {
  std::string s(size, '\0');
  for (size_t i = 0; i < size; ++i) s[i] = static_cast<char>(seed + (i * 7 + i / 251) % 26);
  return s;
}

// "bytes=a-b" of request i, or "" when it had no Range header.
std::string rangeOf(size_t i) {
  const std::string& r = FakeNet::requests().at(i);
  const size_t at = r.find("Range: ");
  if (at == std::string::npos) return "";
  return r.substr(at + 7, r.find("\r\n", at) - at - 7);
}

struct Fixture : ::testing::Test {
  crosspoint::SecureHttpClient http;
  crosspoint::ChunkSession session;
  std::string file;
  int rewinds = 0;
  std::pair<size_t, size_t> lastProgress{0, 0};

  void SetUp() override {
    FakeNet::reset();
    g_largest = 16372;
    http.setTimeout(5000);
  }

  void serve(const std::string& resource) {
    FakeNet::resource() = resource;
    FakeNet::serving() = true;
  }

  crosspoint::DownloadSink fileSink() {
    file.clear();
    crosspoint::DownloadSink sink;
    sink.write = [this](const uint8_t* d, size_t n) {
      file.append(reinterpret_cast<const char*>(d), n);
      return true;
    };
    sink.rewind = [this] {
      ++rewinds;
      file.clear();
      return true;
    };
    sink.progress = [this](size_t done, size_t total) {
      lastProgress = {done, total};
      return true;
    };
    return sink;
  }

  int download(const std::string& url = "https://raw.example/fonts/f.cpfont") {
    crosspoint::DownloadSink sink = fileSink();
    return crosspoint::downloadToSink(http, url, sink, session, &fakeLargest);
  }

  // The size the next chunked request will use, given the session's ceiling and g_largest.
  size_t firstChunk() const { return hr::ChunkSizer(session.ceiling).next(g_largest); }
};

TEST_F(Fixture, ChunksAFileOverOneKeptAliveConnection) {
  const std::string resource = makeResource(20000);
  serve(resource);
  const size_t c = firstChunk();

  EXPECT_EQ(download(), 200);
  EXPECT_EQ(file, resource);
  EXPECT_EQ(FakeNet::connects(), 1);
  const size_t expectedRequests = (resource.size() + c - 1) / c;
  ASSERT_EQ(FakeNet::requests().size(), expectedRequests);
  for (size_t i = 0; i < expectedRequests; ++i) {
    const size_t first = i * c;
    const size_t last = std::min(first + c, resource.size()) - 1;
    EXPECT_EQ(rangeOf(i), "bytes=" + std::to_string(first) + "-" + std::to_string(last)) << i;
  }
  EXPECT_EQ(lastProgress, std::make_pair(resource.size(), resource.size()));  // whole-file progress
}

TEST_F(Fixture, StreamsWhenTheLargestBlockIsLarge) {
  const std::string resource = makeResource(20000);
  serve(resource);
  g_largest = 64 * 1024;
  EXPECT_EQ(download(), 200);
  EXPECT_EQ(file, resource);
  ASSERT_EQ(FakeNet::requests().size(), 1u);
  EXPECT_EQ(rangeOf(0), "");
}

TEST_F(Fixture, StreamsPlainHttp) {
  const std::string resource = makeResource(20000);
  serve(resource);
  EXPECT_EQ(download("http://opds.example/book.epub"), 200);
  EXPECT_EQ(file, resource);
  ASSERT_EQ(FakeNet::requests().size(), 1u);
  EXPECT_EQ(rangeOf(0), "");
}

TEST_F(Fixture, AnOutOfMemoryReadReconnectsAndResumesWithASmallerChunk) {
  const std::string resource = makeResource(30000);
  serve(resource);
  const size_t c = firstChunk();
  hr::ChunkSizer probe(session.ceiling);
  probe.next(g_largest);
  probe.onOutOfMemory();
  const size_t lower = probe.ceiling();
  ASSERT_LT(lower, c);
  // The first chunk delivers 1000 bytes, then its next record cannot be allocated.
  FakeNet::faults()[0] = {1000, FakeReply::End::OutOfMemory};

  EXPECT_EQ(download(), 200);
  EXPECT_EQ(file, resource);
  EXPECT_EQ(FakeNet::connects(), 2);  // one reconnect
  // Resumed at exactly the bytes written, one size smaller, and the session remembers it.
  EXPECT_EQ(rangeOf(1), "bytes=1000-" + std::to_string(1000 + lower - 1));
  EXPECT_EQ(session.ceiling, lower);
}

TEST_F(Fixture, TheCeilingCarriesToTheNextFileOfTheSession) {
  serve(makeResource(20000));
  FakeNet::faults()[0] = {1000, FakeReply::End::OutOfMemory};
  ASSERT_EQ(download(), 200);
  const size_t lowered = session.ceiling;
  ASSERT_LT(lowered, hr::ChunkSizer().next(g_largest));

  // The second file, on the same session and connection, starts at the lowered size although the
  // largest block reads as before.
  const size_t before = FakeNet::requests().size();
  const std::string second = makeResource(15000, 'k');
  FakeNet::resource() = second;
  EXPECT_EQ(download("https://raw.example/fonts/g.cpfont"), 200);
  EXPECT_EQ(file, second);
  EXPECT_EQ(rangeOf(before), "bytes=0-" + std::to_string(lowered - 1));
}

TEST_F(Fixture, StallsInARowEndTheDownload) {
  serve(makeResource(20000));
  // Every request gets its headers and then the connection drops before any body byte.
  for (size_t i = 0; i < 50; ++i) FakeNet::faults()[i] = {0, FakeReply::End::Close};

  EXPECT_LT(download(), 0);
  // The first failure and MAX_STALLED_RETRIES retries, then it gives up.
  EXPECT_EQ(FakeNet::requests().size(), 1u + hr::MAX_STALLED_RETRIES);
  EXPECT_TRUE(file.empty());
}

TEST_F(Fixture, FailuresThatMakeProgressKeepGoing) {
  const std::string resource = makeResource(30000);
  serve(resource);
  // Every second request drops after 500 body bytes: progress each time, so no stall is counted.
  for (size_t i = 0; i < 40; i += 2) FakeNet::faults()[i] = {500, FakeReply::End::Close};

  EXPECT_EQ(download(), 200);
  EXPECT_EQ(file, resource);
}

// A 206 with headers and Content-Length but no body.
FakeReply emptyPartial(size_t first, size_t last, const std::string& total) {
  return {"HTTP/1.1 206 Partial Content\r\nContent-Range: bytes " + std::to_string(first) + "-" + std::to_string(last) +
              "/" + total + "\r\nContent-Length: 0\r\n\r\n",
          FakeReply::End::KeepOpen};
}

TEST_F(Fixture, AnEmpty206WithAKnownTotalIsAStallNotALoop) {
  // Review I1: an empty 206 was taken for progress and the identical request repeated for ever.
  for (int i = 0; i < 50; ++i) FakeNet::replies().push_back(emptyPartial(0, 6143, "20000"));
  EXPECT_LT(download(), 0);
  EXPECT_EQ(FakeNet::requests().size(), 1u + hr::MAX_STALLED_RETRIES);
  EXPECT_TRUE(file.empty());
}

TEST_F(Fixture, AnEmpty206WithAnUnknownTotalIsNotTheEnd) {
  // Review I1: with "/*", an empty (or short) 206 used to end the transfer and report success.
  for (int i = 0; i < 50; ++i) FakeNet::replies().push_back(emptyPartial(0, 6143, "*"));
  EXPECT_LT(download(), 0);
  EXPECT_TRUE(file.empty());
}

TEST_F(Fixture, AChunkWhoseBodyNeverComesIsAskedForAgain) {
  const std::string resource = makeResource(20000);
  serve(resource);
  const size_t c = firstChunk();
  FakeNet::faults()[1] = {0, FakeReply::End::KeepOpen};  // headers, then silence until the timeout
  EXPECT_EQ(download(), 200);
  EXPECT_EQ(file, resource);
  EXPECT_EQ(rangeOf(2), rangeOf(1));  // the same range asked again, on a new connection
  EXPECT_EQ(rangeOf(1).rfind("bytes=" + std::to_string(c) + "-", 0), 0u);
  EXPECT_EQ(FakeNet::connects(), 2);
}

TEST_F(Fixture, AnUnknownTotalEndsOnlyAtA416ForTheNextByte) {
  const std::string resource = makeResource(9000);
  serve(resource);
  FakeNet::totalUnknown() = true;
  EXPECT_EQ(download(), 200);
  EXPECT_EQ(file, resource);
  // The short chunk that brought the last bytes did not end it; the 416 for byte 9000 did.
  const size_t n = FakeNet::requests().size();
  ASSERT_GE(n, 2u);
  EXPECT_EQ(rangeOf(n - 1).rfind("bytes=9000-", 0), 0u);
}

TEST_F(Fixture, AnUnknownTotalOfAWholeNumberOfChunksEndsAtA416) {
  const size_t c = firstChunk();
  const std::string resource = makeResource(2 * c);
  serve(resource);
  FakeNet::totalUnknown() = true;
  EXPECT_EQ(download(), 200);
  EXPECT_EQ(file, resource);
  EXPECT_EQ(FakeNet::requests().size(), 3u);  // two full chunks, then the 416
}

TEST_F(Fixture, A416ThatDisagreesWithTheBytesWrittenFails) {
  // Review M3: "bytes */N" must match what was written.
  FakeNet::replies().push_back(
      {"HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 0-99/*\r\nContent-Length: "
       "100\r\n\r\n" +
           makeResource(100),
       FakeReply::End::KeepOpen});
  FakeNet::replies().push_back(
      {"HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */5000\r\nContent-Length: 0\r\n\r\n",
       FakeReply::End::KeepOpen});
  EXPECT_EQ(download(), 416);
}

TEST_F(Fixture, A200FallbackGetsOneAttemptAndTheSessionThenStreams) {
  // Review I2: after a 200 the loop used to reconnect, ask for a range again, get the whole file
  // again and start over, up to the reconnect cap.
  const std::string resource = makeResource(30000);
  serve(resource);
  FakeNet::ignoreRangeFrom() = 1;                        // the first chunk is a 206, then 200s
  FakeNet::faults()[1] = {3000, FakeReply::End::Close};  // and the whole-file reply drops

  EXPECT_LT(download(), 0);
  EXPECT_EQ(FakeNet::requests().size(), 2u);  // no second whole-file attempt
  EXPECT_EQ(rewinds, 1);                      // the first chunk was dropped before the 200's body
  EXPECT_EQ(file, resource.substr(0, 3000));  // what the 200 delivered, from byte 0
  EXPECT_TRUE(session.rangeUnsupported);

  // The next file on this session streams without a Range header.
  FakeNet::faults().clear();
  const size_t before = FakeNet::requests().size();
  EXPECT_EQ(download("https://raw.example/fonts/g.cpfont"), 200);
  EXPECT_EQ(file, resource);
  EXPECT_EQ(FakeNet::requests().size(), before + 1);
  EXPECT_EQ(rangeOf(before), "");
}

TEST_F(Fixture, A200ForTheFirstChunkIsTheWholeFile) {
  const std::string resource = makeResource(30000);
  serve(resource);
  FakeNet::ignoreRangeFrom() = 0;
  EXPECT_EQ(download(), 200);
  EXPECT_EQ(file, resource);
  EXPECT_EQ(FakeNet::requests().size(), 1u);
  EXPECT_EQ(rewinds, 0);  // nothing was written before it
  EXPECT_TRUE(session.rangeUnsupported);
}

TEST_F(Fixture, AnEmpty200MidFileRewindsTheFile) {
  // Review M2: an empty 200 after earlier chunks used to leave the old partial file in place.
  const std::string resource = makeResource(20000);
  serve(resource);
  const size_t c = firstChunk();
  FakeNet::replies().push_back({"HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 0-" + std::to_string(c - 1) +
                                    "/20000\r\nContent-Length: " + std::to_string(c) + "\r\n\r\n" +
                                    resource.substr(0, c),
                                FakeReply::End::KeepOpen});
  FakeNet::replies().push_back({"HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", FakeReply::End::KeepOpen});
  download();
  EXPECT_EQ(rewinds, 1);
  EXPECT_TRUE(file.empty());
  EXPECT_TRUE(session.rangeUnsupported);
}

TEST_F(Fixture, AFileThatChangesSizeMidDownloadFailsUnspliced) {
  // Review M1: a new version of the file between two chunks would be spliced onto the old one.
  const std::string oldVersion = makeResource(20000);
  const size_t c = firstChunk();
  FakeNet::replies().push_back({"HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 0-" + std::to_string(c - 1) +
                                    "/20000\r\nContent-Length: " + std::to_string(c) + "\r\n\r\n" +
                                    oldVersion.substr(0, c),
                                FakeReply::End::KeepOpen});
  serve(makeResource(25000, 'b'));  // the next chunk comes from the new version
  EXPECT_EQ(download(), crosspoint::ERR_RESOURCE_CHANGED);
  EXPECT_EQ(file, oldVersion.substr(0, c));  // nothing of the new version was written
  EXPECT_EQ(FakeNet::requests().size(), 2u);
}

// Review M4: chunks after the first go to the redirect target, and a presigned CDN URL (GitHub's
// release assets) can expire mid-download. The original URL hands out a fresh one.
TEST_F(Fixture, AnExpiredRedirectTargetIsRefreshedThroughTheOriginalUrl) {
  const std::string resource = makeResource(20000);
  const size_t c = firstChunk();
  const std::string redirect =
      "HTTP/1.1 302 Found\r\nLocation: https://cdn.example/signed\r\nContent-Length: 0\r\n\r\n";
  FakeNet::replies().push_back({redirect, FakeReply::End::KeepOpen});
  FakeNet::replies().push_back({"HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 0-" + std::to_string(c - 1) +
                                    "/20000\r\nContent-Length: " + std::to_string(c) + "\r\n\r\n" +
                                    resource.substr(0, c),
                                FakeReply::End::KeepOpen});
  FakeNet::replies().push_back(
      {"HTTP/1.1 403 Forbidden\r\nContent-Length: 9\r\n\r\nForbidden", FakeReply::End::KeepOpen});
  FakeNet::replies().push_back({redirect, FakeReply::End::KeepOpen});
  serve(resource);  // the rest comes from the refreshed target

  EXPECT_EQ(download(), 200);
  EXPECT_EQ(file, resource);
  ASSERT_GE(FakeNet::requests().size(), 5u);
  EXPECT_EQ(FakeNet::requests()[2].rfind("GET /signed ", 0), 0u);
  EXPECT_EQ(FakeNet::requests()[3].rfind("GET /fonts/f.cpfont ", 0), 0u);  // back through the original
  EXPECT_EQ(rangeOf(3), "bytes=" + std::to_string(c) + "-" + std::to_string(2 * c - 1));
}

TEST_F(Fixture, ARedirectTargetThatStillRefusesFails) {
  const size_t c = firstChunk();
  const std::string redirect =
      "HTTP/1.1 302 Found\r\nLocation: https://cdn.example/signed\r\nContent-Length: 0\r\n\r\n";
  const FakeReply forbidden{"HTTP/1.1 403 Forbidden\r\nContent-Length: 9\r\n\r\nForbidden", FakeReply::End::KeepOpen};
  FakeNet::replies().push_back({redirect, FakeReply::End::KeepOpen});
  FakeNet::replies().push_back({"HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 0-" + std::to_string(c - 1) +
                                    "/20000\r\nContent-Length: " + std::to_string(c) + "\r\n\r\n" + makeResource(c),
                                FakeReply::End::KeepOpen});
  FakeNet::replies().push_back(forbidden);
  FakeNet::replies().push_back({redirect, FakeReply::End::KeepOpen});
  FakeNet::replies().push_back(forbidden);  // the fresh target refuses too
  EXPECT_EQ(download(), 403);
  EXPECT_EQ(FakeNet::requests().size(), 5u);
}

// The chunks after the first go straight to the redirect target, and the credentials given for the
// original URL went there with every one of them.
TEST_F(Fixture, ChunksSentToAnotherHostCarryNoCredentials) {
  http.setBasicAuth("reader", "secret");
  const std::string resource = makeResource(20000);
  const size_t c = firstChunk();
  const std::string redirect =
      "HTTP/1.1 302 Found\r\nLocation: https://cdn.example/signed\r\nContent-Length: 0\r\n\r\n";
  FakeNet::replies().push_back({redirect, FakeReply::End::KeepOpen});
  FakeNet::replies().push_back({"HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 0-" + std::to_string(c - 1) +
                                    "/20000\r\nContent-Length: " + std::to_string(c) + "\r\n\r\n" +
                                    resource.substr(0, c),
                                FakeReply::End::KeepOpen});
  FakeNet::replies().push_back(
      {"HTTP/1.1 403 Forbidden\r\nContent-Length: 9\r\n\r\nForbidden", FakeReply::End::KeepOpen});
  FakeNet::replies().push_back({redirect, FakeReply::End::KeepOpen});
  serve(resource);

  EXPECT_EQ(download(), 200);
  EXPECT_EQ(file, resource);
  const std::vector<std::string>& requests = FakeNet::requests();
  ASSERT_GE(requests.size(), 5u);
  for (size_t i = 0; i < requests.size(); ++i) {
    // Only the original URL, asked first and again when the target refused, gets them.
    const bool original = requests[i].rfind("GET /fonts/f.cpfont ", 0) == 0;
    EXPECT_EQ(requests[i].find("\r\nAuthorization: ") != std::string::npos, original) << i << ":\n" << requests[i];
  }
  EXPECT_EQ(requests[2].rfind("GET /signed ", 0), 0u);  // a chunk straight to the target, without them

  // The download's scope ends with it: a request of the caller's own to that host is the caller's.
  FakeNet::replies().push_back({"HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", FakeReply::End::KeepOpen});
  EXPECT_EQ(http.get("https://cdn.example/other", nullptr), 200);
  EXPECT_NE(FakeNet::requests().back().find("\r\nAuthorization: "), std::string::npos);
}

TEST_F(Fixture, AFailedRewindIsAFileError) {
  const std::string resource = makeResource(20000);
  serve(resource);
  FakeNet::ignoreRangeFrom() = 1;
  crosspoint::DownloadSink sink = fileSink();
  sink.rewind = [] { return false; };
  EXPECT_EQ(crosspoint::downloadToSink(http, "https://raw.example/fonts/f.cpfont", sink, session, &fakeLargest),
            crosspoint::ERR_REWIND);
}

TEST_F(Fixture, AnOutOfMemoryReadInAClosedDelimited206IsNotTheEnd) {
  // Review I1: a body without Content-Length ends at the peer's close; a TLS read that failed
  // (here out of memory) used to pass for that close and the short chunk for the whole range.
  const std::string resource = makeResource(20000);
  FakeNet::replies().push_back(
      {"HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 0-6143/20000\r\nConnection: "
       "close\r\n\r\n" +
           resource.substr(0, 1000),
       FakeReply::End::OutOfMemory});
  serve(resource);  // after the scripted reply, the server answers ranges itself
  EXPECT_EQ(download(), 200);
  EXPECT_EQ(file, resource);
  EXPECT_EQ(rangeOf(1).rfind("bytes=1000-", 0), 0u);             // resumed after the bytes written
  EXPECT_LT(session.ceiling, hr::ChunkSizer().next(g_largest));  // and treated as out of memory
}

}  // namespace
