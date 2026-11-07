"""Exercise the actual peer-bound approval and expiry functions."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
s = (root / 'vendor/esp32drop/src/airdrop/port/ad_port_esp32.cpp').read_text()
functions = s[s.index('static bool sv_approve'):s.index('static void sv_upload')]
code = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
struct HttpServe {};
static std::atomic<bool> g_approved{false}, g_stop{false};
enum class Consent { Idle, Asking, Accept, Decline };
static std::atomic<Consent> g_consent{Consent::Idle};
static uint32_t g_request_started_at = 0;
static Consent answer = Consent::Asking;
static bool g_rx_active, peer_valid = true;
static uint32_t g_approved_at, clock_ms, g_img_err;
static uint8_t g_approved_peer[16] = {1}, current_peer[16] = {1};
static char g_img_errmsg[40];
uint32_t millis() { return clock_ms; }
#define pdMS_TO_TICKS(x) (x)
void vTaskDelay(uint32_t ms) { clock_ms += ms; if (clock_ms >= 100) g_consent = answer; }
bool peer_address(uint8_t* out) { memcpy(out, current_peer, 16); return peer_valid; }
#define strlcpy(dst, src, size) snprintf(dst, size, "%s", src)
''' + functions + r'''
int main() {
  g_approved = true;
  current_peer[0] = 2;
  assert(!sv_authorized(nullptr) && g_approved);
  current_peer[0] = 1;
  peer_valid = false;
  assert(!sv_authorized(nullptr) && g_approved);
  peer_valid = true;
  assert(sv_authorized(nullptr) && !g_approved);
  assert(!sv_authorized(nullptr));
  g_approved_at = 0xfffff000u;
  clock_ms = g_approved_at + 89999u;
  g_approved = true;
  assert(sv_authorized(nullptr));
  g_approved = true;
  clock_ms = g_approved_at + 90000u;
  assert(!sv_authorized(nullptr) && !g_approved && g_img_err == 1);
  expire_approval();
  assert(g_img_err == 1);
  g_approved = true;
  g_rx_active = true;
  sv_response_failed(nullptr);
  assert(!g_approved && !g_rx_active && g_img_err == 2);
  clock_ms = g_request_started_at = 0;
  g_img_err = 0;
  answer = Consent::Asking;
  assert(!sv_approve(nullptr) && g_img_err == 1);
  clock_ms = 0;
  answer = Consent::Decline;
  assert(!sv_approve(nullptr) && g_img_err == 1);
  clock_ms = 0;
  answer = Consent::Accept;
  assert(sv_approve(nullptr) && g_approved && g_img_err == 1);
  puts("Approval: wrong-peer preservation, one-shot consent, expiry, clock wrap and failed response passed");
}
'''
with tempfile.TemporaryDirectory(prefix='airdrop-approval-') as tmp:
    source, binary = Path(tmp) / 'test.cpp', Path(tmp) / 'test'
    source.write_text(code)
    subprocess.run(['c++', '-std=c++17', '-fsanitize=address,undefined', str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
