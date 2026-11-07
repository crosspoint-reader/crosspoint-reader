"""Optional native check of the firmware TLS adapter against Python/OpenSSL.

Run after PlatformIO has downloaded and patched Arduino-wolfSSL:
  python3 test/airdrop/tls_test.py --wolfssl-source .pio/libdeps/x4c-gh_release/Arduino-wolfSSL/src

Requires a native C/C++ toolchain and permission to bind a loopback socket.
This checks TLS compatibility, not ESP32 memory use or AWDL interoperability.
"""
from pathlib import Path
import argparse
import concurrent.futures
import socket
import ssl
import struct
import subprocess
import tempfile
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--payload-file', type=Path)
parser.add_argument('--wolfssl-source', required=True, type=Path)
args = parser.parse_args()
repo = Path(__file__).resolve().parents[2]
lib = args.wolfssl_source.resolve()
if not (lib / 'wolfssl/ssl.h').is_file():
    parser.error('wolfSSL source directory does not contain wolfssl/ssl.h')
s = (repo / 'vendor/esp32drop/src/airdrop/port/ad_port_esp32.cpp').read_text()
a = s.index('// wolfSSL is already')
b = s.index('static void http_set_timeout', a)
prefix = r'''

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <wolfssl/ssl.h>

#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include "airdrop/core/ad_http.h"
#include "airdrop/core/airdrop_cert.h"
static WOLFSSL_CTX* g_tlsconf = nullptr;
static WOLFSSL* g_ssl = nullptr;
static int g_tls_fd = -1, g_ec_crt_err, g_ec_key_err;
static const char* g_tls_alg;
static bool g_tls_ready = false;
static std::atomic<bool> g_stop{false}, g_listener_running{false};
static std::atomic<uint32_t> g_socket_rx_bytes{0};
static std::atomic<int> g_last_tls_error{0};
uint32_t millis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
#define pdMS_TO_TICKS(n) (n)
void vTaskDelay(unsigned ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
'''
main = r'''

int main() {
  assert(ad_prepare_tls());
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(!bind(fd, (sockaddr*)&addr, sizeof(addr)));
  assert(!listen(fd, 4));
  socklen_t alen = sizeof(addr);
  assert(!getsockname(fd, (sockaddr*)&addr, &alen));
  printf("%u\n", ntohs(addr.sin_port));
  fflush(stdout);
  for (int i = 0; i < 4; i++) {
    g_tls_fd = accept(fd, nullptr, nullptr);
    assert(g_tls_fd >= 0);
    int flags = fcntl(g_tls_fd, F_GETFL, 0);
    assert(!fcntl(g_tls_fd, F_SETFL, flags | O_NONBLOCK));
    if (!g_ssl) g_ssl = wolfSSL_new(g_tlsconf);
    assert(g_ssl);
    uint32_t start = millis();
    int ret;
    while ((ret = wolfSSL_accept(g_ssl)) != WOLFSSL_SUCCESS) {
      int e = wolfSSL_get_error(g_ssl, ret);
      if (!tls_want_io(e)) {
        fprintf(stderr, "handshake error %d\n", e);
        return 2;
      }
      assert(millis() - start < 10000);
      vTaskDelay(1);
    }
    assert(!fcntl(g_tls_fd, F_SETFL, flags));
    set_rd_timeout(2000);
    HttpRdr r;
    http_rdr_init(&r, g_ssl, http_src_tls);
    unsigned char header[4];
    assert(hr_read(&r, header, 4) == 4);
    uint32_t n = (header[0] << 24) | (header[1] << 16) | (header[2] << 8) | header[3];
    uint32_t hash = 2166136261u;
    unsigned char buffer[4096];
    while (n) {
      int k = n < sizeof(buffer) ? n : sizeof(buffer);
      assert(hr_read(&r, buffer, k) == k);
      for (int j = 0; j < k; j++) hash = (hash ^ buffer[j]) * 16777619u;
      n -= k;
    }
    uint32_t network = htonl(hash);
    assert(ssl_write_all(g_ssl, (unsigned char*)&network, 4) == 4);
    wolfSSL_shutdown(g_ssl);
    close(g_tls_fd);
    g_tls_fd = -1;
    tls_reset_session();
  }
  close(fd);
  airdrop_tls_release();
  puts("TLS adapter handshake, records and reconnect passed");
}
'''
flags = ['-DWOLFSSL_USER_SETTINGS', '-DWOLFSSL_OPTIONS_H', '-DWOLFSSL_CLIENT_EXAMPLE', '-DCROSSPOINT_AIRDROP=1', '-DHAVE_SERVER_RENEGOTIATION_INFO', '-DWOLFSSL_TLS13', '-DWOLFSSL_HAVE_SP_ECC', '-DWOLFSSL_SP_SMALL', '-DHAVE_CURVE25519', '-DHAVE_SNI', '-DWOLFSSL_KEY_GEN', '-DWC_RC2']
with tempfile.TemporaryDirectory(prefix='crosspoint-tls-') as directory:
    root = Path(directory)
    files = list((lib / 'src').glob('*.c')) + list((lib / 'wolfcrypt/src').glob('*.c'))

    def build(path):
        output = root / (path.parent.name + '_' + path.stem + '.o')
        subprocess.run(['cc', '-Os', '-I' + str(lib), *flags, '-c', str(path), '-o', str(output)], check=True, capture_output=True)
        return str(output)
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        objects = list(pool.map(build, files))
    subprocess.run(['ar', 'rcs', str(root / 'libwolfssl.a'), *objects], check=True)
    source = root / 'adapter_test.cpp'
    source.write_text(prefix + s[a:b] + main)
    subprocess.run(['c++', '-std=c++17', '-I' + str(lib), '-I' + str(repo / 'vendor/esp32drop/src'), *flags, str(source), str(root / 'libwolfssl.a'), '-o', str(root / 'adapter_test')], check=True)
    p = subprocess.Popen([str(root / 'adapter_test')], stdout=subprocess.PIPE, text=True)
    try:
        import select
        ready, _, _ = select.select([p.stdout], [], [], 15)
        assert ready, 'TLS startup timed out'
        portline = p.stdout.readline().strip()
        assert portline, 'TLS initialization failed'
        port = int(portline)
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        ctx.minimum_version = ctx.maximum_version = ssl.TLSVersion.TLSv1_2
        session = None
        for i in range(4):
            with socket.create_connection(('127.0.0.1', port), timeout=10) as sock:
                with ctx.wrap_socket(sock, server_hostname='airdrop', session=session if i == 1 else None) as tls:
                    data = bytes(range(256)) * (1024 if i != 2 else 1)
                    if i == 3:
                        data = args.payload_file.read_bytes() if args.payload_file else bytes(range(256)) * 24576
                    tls.sendall(struct.pack('!I', len(data)) + data)
                    response = b''
                    while len(response) < 4:
                        block = tls.recv(4 - len(response))
                        assert block, 'TLS closed before the checksum response'
                        response += block
                    expected = 2166136261
                    for byte in data:
                        expected = (expected ^ byte) * 16777619 & 4294967295
                    assert struct.unpack('!I', response)[0] == expected
                    if i == 1:
                        assert tls.session_reused, 'TLS session resumption failed'
                    print('Connection', i + 1, 'cipher', tls.cipher()[0], 'session reused', tls.session_reused, 'bytes', len(data))
                    if i == 0:
                        session = tls.session
        assert p.wait(timeout=10) == 0
        print(p.stdout.read().strip())
    finally:
        if p.poll() is None:
            p.kill()
            p.wait()

    # Verify the same patched library still supports TLS 1.3 as an HTTPS client.
    client = root / 'client_test.c'
    client.write_text(r'''
#include <wolfssl/ssl.h>
#include <wolfssl/error-ssl.h>
#include "airdrop/core/airdrop_cert.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
int main(int argc, char** argv) {
    assert(argc == 2);
    assert(wolfSSL_Init() == WOLFSSL_SUCCESS);
    WOLFSSL_CTX* ctx = wolfSSL_CTX_new(wolfTLSv1_3_client_method());
    assert(ctx);
    wolfSSL_CTX_set_verify(ctx, WOLFSSL_VERIFY_NONE, NULL);
    WOLFSSL* tls = wolfSSL_new(ctx);
    assert(tls);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(atoi(argv[1]));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(!connect(fd, (struct sockaddr*)&addr, sizeof(addr)));
    assert(wolfSSL_set_fd(tls, fd) == WOLFSSL_SUCCESS);
    assert(wolfSSL_connect(tls) == WOLFSSL_SUCCESS);
    assert(!strcmp(wolfSSL_get_version(tls), "TLSv1.3"));
    assert(wolfSSL_write(tls, "ping", 4) == 4);
    char reply[4];
    assert(wolfSSL_read(tls, reply, 4) == 4);
    assert(!memcmp(reply, "pong", 4));
    wolfSSL_free(tls);
    wolfSSL_CTX_free(ctx);
    close(fd);
#ifdef CROSSPOINT_TLS12_SERVER_ONLY
    ctx = wolfSSL_CTX_new(wolfTLSv1_3_server_method());
    assert(ctx);
    assert(wolfSSL_CTX_use_certificate_buffer(ctx, AIRDROP_CRT_EC_DER, AIRDROP_CRT_EC_DER_LEN, WOLFSSL_FILETYPE_ASN1) == WOLFSSL_SUCCESS);
    assert(wolfSSL_CTX_use_PrivateKey_buffer(ctx, AIRDROP_KEY_EC_DER, AIRDROP_KEY_EC_DER_LEN, WOLFSSL_FILETYPE_ASN1) == WOLFSSL_SUCCESS);
    tls = wolfSSL_new(ctx);
    assert(tls);
    extern int wolfSSL_accept_TLSv13(WOLFSSL*);
    int result = wolfSSL_accept_TLSv13(tls);
    assert(result == WOLFSSL_FATAL_ERROR);
    assert(wolfSSL_get_error(tls, result) == VERSION_ERROR);
    wolfSSL_free(tls);
    wolfSSL_CTX_free(ctx);
#endif
    wolfSSL_Cleanup();
}
''')
    executable = root / 'client_test'
    subprocess.run(['cc', '-I' + str(lib), '-I' + str(repo / 'vendor/esp32drop/src'), *flags, str(client), str(root / 'libwolfssl.a'), '-o', str(executable)], check=True)
    cert, key = root / 'cert.pem', root / 'key.pem'
    subprocess.run(['openssl', 'req', '-x509', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:prime256v1', '-nodes', '-keyout', str(key), '-out', str(cert), '-subj', '/CN=localhost', '-days', '1'], check=True, capture_output=True)
    server = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    server.minimum_version = server.maximum_version = ssl.TLSVersion.TLSv1_3
    server.load_cert_chain(cert, key)
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        listener.listen(1)
        listener.settimeout(10)
        def serve():
            with listener.accept()[0] as connection:
                connection.settimeout(10)
                with server.wrap_socket(connection, server_side=True) as tls:
                    assert tls.version() == 'TLSv1.3'
                    assert tls.recv(4) == b'ping'
                    tls.sendall(b'pong')
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
            future = pool.submit(serve)
            subprocess.run([str(executable), str(listener.getsockname()[1])], check=True, timeout=15)
            future.result(timeout=15)
    print('TLS 1.3 client negotiation and TLS 1.3 server rejection passed')
