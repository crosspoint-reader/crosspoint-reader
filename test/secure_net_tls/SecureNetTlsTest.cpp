#include <SecureClient.h>
#include <wolfssl/ssl.h>

#include <cassert>

void reset() {
  assert(FakeTls::contexts == 0 && FakeTls::sessions == 0);
  FakeNet::replies() = {"server", "server"};
  FakeNet::requests().clear();
  FakeTls::caResult = FakeTls::domainResult = FakeTls::handshakeResult = 1;
  FakeTls::verifyMode = -1;
  FakeTls::handshakes = FakeTls::writes = 0;
  FakeTls::hostname.clear();
}

int main() {
  reset();
  {
    freeink::SecureClient client;
    client.setInsecure();
    client.setCACert("root");
    assert(client.connect("updates.example", 443) == 1);
    assert(FakeTls::verifyMode == WOLFSSL_VERIFY_PEER && FakeTls::hostname == "updates.example");
    assert(FakeTls::handshakes == 1);
  }
  for (int failure = 0; failure < 4; ++failure) {
    reset();
    freeink::SecureClient client;
    if (failure != 0) client.setCACert("root");
    if (failure == 1) FakeTls::caResult = 0;
    if (failure == 2) FakeTls::domainResult = 0;
    if (failure == 3) FakeTls::handshakeResult = -1;
    assert(client.connect("updates.example", 443) == 0);
    assert(!client.connected() && client.write(static_cast<uint8_t>('x')) == 0);
    assert(FakeTls::contexts == 0 && FakeTls::sessions == 0 && FakeTls::writes == 0);
    assert(FakeTls::handshakes == (failure == 3 ? 2 : 0));
  }
  reset();
  {
    freeink::SecureClient client;
    client.setInsecure();
    assert(client.connect("self-hosted.example", 443) == 1);
    assert(FakeTls::verifyMode == WOLFSSL_VERIFY_NONE && FakeTls::hostname.empty());
  }
  assert(FakeTls::contexts == 0 && FakeTls::sessions == 0);
}
