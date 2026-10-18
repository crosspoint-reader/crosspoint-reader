// Host-only integration test. Does not open USB devices or write flash.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "DfuBuffer.h"
#include "device/dcd.h"
#include "tusb.h"

static struct {
  bool active;
  uint8_t* buf;
  uint16_t len;
} pending[2];
static bool stalled;
static unsigned packets, downloads, manifests, aborts;
static uint8_t address, staged[8195];
static size_t staged_len;
static uint16_t expected_block;
static bool busy, manifest_pending;

static const tusb_desc_device_t device = {.bLength = 18,
                                          .bDescriptorType = TUSB_DESC_DEVICE,
                                          .bcdUSB = 0x0200,
                                          .bMaxPacketSize0 = 64,
                                          .idVendor = 0x1209,
                                          .idProduct = 1,
                                          .bcdDevice = 0x0100,
                                          .bNumConfigurations = 1};
static const uint8_t config[] = {TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUD_CONFIG_DESC_LEN + TUD_DFU_DESC_LEN(1), 0, 100),
                                 TUD_DFU_DESCRIPTOR(0, 1, 0, DFU_ATTR_CAN_DOWNLOAD | DFU_ATTR_MANIFESTATION_TOLERANT,
                                                    1000, CFG_TUD_DFU_XFER_BUFSIZE)};
const uint8_t* tud_descriptor_device_cb(void) { return (const uint8_t*)&device; }
const uint8_t* tud_descriptor_configuration_cb(uint8_t i) {
  (void)i;
  return config;
}
const uint16_t* tud_descriptor_string_cb(uint8_t i, uint16_t l) {
  (void)i;
  (void)l;
  return NULL;
}
uint32_t tusb_time_millis_api(void) {
  static uint32_t t;
  return t++;
}

bool dcd_init(uint8_t p, const tusb_rhport_init_t* i) {
  (void)p;
  (void)i;
  return true;
}
bool dcd_deinit(uint8_t p) {
  (void)p;
  return true;
}
void dcd_int_enable(uint8_t p) { (void)p; }
void dcd_int_disable(uint8_t p) { (void)p; }
void dcd_int_handler(uint8_t p) { (void)p; }
void dcd_remote_wakeup(uint8_t p) { (void)p; }
void dcd_sof_enable(uint8_t p, bool e) {
  (void)p;
  (void)e;
}
void dcd_edpt_close_all(uint8_t p) { (void)p; }
bool dcd_edpt_open(uint8_t p, const tusb_desc_endpoint_t* d) {
  (void)p;
  (void)d;
  return false;
}
bool dcd_edpt_iso_alloc(uint8_t p, uint8_t e, uint16_t s) {
  (void)p;
  (void)e;
  (void)s;
  return false;
}
bool dcd_edpt_iso_activate(uint8_t p, const tusb_desc_endpoint_t* d) {
  (void)p;
  (void)d;
  return false;
}
void dcd_edpt_stall(uint8_t p, uint8_t e) {
  (void)p;
  stalled = true;
  pending[e >> 7].active = false;
}
void dcd_edpt_clear_stall(uint8_t p, uint8_t e) {
  (void)p;
  (void)e;
  stalled = false;
}
bool dcd_edpt_xfer(uint8_t p, uint8_t e, uint8_t* b, uint16_t n, bool isr) {
  (void)p;
  (void)isr;
  assert((e & 0x7f) == 0 && n <= 64);
  unsigned dir = e >> 7;
  assert(!pending[dir].active);
  pending[dir].active = true;
  pending[dir].buf = b;
  pending[dir].len = n;
  return true;
}
void dcd_set_address(uint8_t p, uint8_t a) {
  address = a;
  assert(dcd_edpt_xfer(p, 0x80, NULL, 0, false));
}

uint32_t tud_dfu_get_timeout_cb(uint8_t alt, uint8_t state) {
  assert(alt == 0);
  return state == DFU_MANIFEST ? 250 : 100;
}
void tud_dfu_download_cb(uint8_t alt, uint16_t block, const uint8_t* data, uint16_t len) {
  assert(alt == 0 && !busy);
  downloads++;
  if (block != expected_block || staged_len + len > sizeof(staged)) {
    tud_dfu_finish_flashing(DFU_STATUS_ERR_ADDRESS);
    return;
  }
  memcpy(staged + staged_len, data, len);
  staged_len += len;
  expected_block++;
  busy = true;
  // Remain busy until the test explicitly completes the simulated write.
}
void tud_dfu_manifest_cb(uint8_t alt) {
  assert(alt == 0);
  manifests++;
  manifest_pending = true;
}
void tud_dfu_abort_cb(uint8_t alt) {
  assert(alt == 0);
  aborts++;
  staged_len = 0;
  expected_block = 0;
  busy = false;
  manifest_pending = false;
}
void tud_dfu_detach_cb(void) {}
uint16_t tud_dfu_upload_cb(uint8_t a, uint16_t b, uint8_t* d, uint16_t n) {
  (void)a;
  (void)b;
  (void)d;
  (void)n;
  assert(false);
  return 0;
}

// Simulate host control transactions at the DCD event boundary. This deliberately
// does not model electrical signals, CRCs, DATA toggles, retries or bus timing.
static size_t control(uint8_t type, uint8_t request, uint16_t value, uint16_t length, const uint8_t* out, uint8_t* in) {
  uint8_t setup[] = {type, request, value & 255, value >> 8, 0, 0, length & 255, length >> 8};
  memset(pending, 0, sizeof(pending));
  stalled = false;
  dcd_event_setup_received(0, setup, false);
  tud_task();
  size_t transferred = 0;
  unsigned guard = 0;
  while (!stalled && (pending[0].active || pending[1].active)) {
    assert(++guard < 1024);
    unsigned dir = pending[1].active ? 1 : 0;
    uint16_t n = pending[dir].len;
    uint8_t* buf = pending[dir].buf;
    pending[dir].active = false;
    if (n) {
      assert(transferred + n <= length);
      if (dir) {
        assert(in);
        memcpy(in + transferred, buf, n);
      } else {
        assert(out);
        memcpy(buf, out + transferred, n);
      }
      transferred += n;
      packets++;
    }
    dcd_event_xfer_complete(0, dir ? 0x80 : 0, n, XFER_RESULT_SUCCESS, false);
    tud_task();
  }
  return transferred;
}
static void status(uint8_t state, uint8_t error) {
  uint8_t b[6];
  assert(control(0xa1, DFU_REQUEST_GETSTATUS, 0, 6, NULL, b) == 6);
  assert(!stalled && b[4] == state && b[0] == error);
  if (state == DFU_DNBUSY) assert(b[1] == 100 && b[2] == 0 && b[3] == 0);
}
static void enumerate(uint8_t new_address) {
  uint8_t b[64];
  memset(pending, 0, sizeof(pending));
  dcd_event_bus_reset(0, TUSB_SPEED_FULL, false);
  tud_task();
  assert(!tud_mounted());
  assert(control(0x80, TUSB_REQ_GET_DESCRIPTOR, 0x100, 8, NULL, b) == 8 && b[7] == 64);
  control(0, TUSB_REQ_SET_ADDRESS, new_address, 0, NULL, NULL);
  assert(address == new_address);
  assert(control(0x80, TUSB_REQ_GET_DESCRIPTOR, 0x100, 18, NULL, b) == 18);
  assert(memcmp(b, &device, 18) == 0);
  assert(control(0x80, TUSB_REQ_GET_DESCRIPTOR, 0x200, 9, NULL, b) == 9);
  assert(control(0x80, TUSB_REQ_GET_DESCRIPTOR, 0x200, sizeof(config), NULL, b) == sizeof(config));
  assert(memcmp(b, config, sizeof(config)) == 0);
  control(0, TUSB_REQ_SET_CONFIGURATION, 1, 0, NULL, NULL);
  assert(tud_mounted());
}
int main(void) {
  tusb_rhport_init_t init = {.role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL};
  uint8_t* workspace = malloc(CFG_TUD_DFU_XFER_BUFSIZE + 4);
  assert(workspace);
  assert(!su_dfu_bind_buffer(NULL, 1));
  assert(!su_dfu_bind_buffer(workspace + 1, CFG_TUD_DFU_XFER_BUFSIZE));
  assert(!su_dfu_bind_buffer(workspace, CFG_TUD_DFU_XFER_BUFSIZE - 1));
  assert(su_dfu_bind_buffer(workspace, CFG_TUD_DFU_XFER_BUFSIZE));
  assert(tusb_rhport_init(0, &init));
  enumerate(37);
  status(DFU_IDLE, DFU_STATUS_OK);
  static uint8_t source[8195];
  uint32_t rng = 0x5833;
  for (size_t i = 0; i < sizeof(source); i++) {
    rng = rng * 1664525u + 1013904223u;
    source[i] = rng >> 24;
  }
  uint16_t block = 0;
  for (size_t offset = 0; offset < sizeof(source); offset += CFG_TUD_DFU_XFER_BUFSIZE, block++) {
    uint16_t len =
        (sizeof(source) - offset < CFG_TUD_DFU_XFER_BUFSIZE) ? sizeof(source) - offset : CFG_TUD_DFU_XFER_BUFSIZE;
    unsigned previous = downloads;
    assert(control(0x21, DFU_REQUEST_DNLOAD, block, len, source + offset, NULL) == len && !stalled);
    assert(downloads == previous);  // Writing begins only after GETSTATUS handshake.
    status(DFU_DNBUSY, DFU_STATUS_OK);
    assert(busy && downloads == previous + 1);
    uint8_t state;
    control(0xa1, DFU_REQUEST_GETSTATE, 0, 1, NULL, &state);
    assert(state == DFU_DNBUSY);
    busy = false;
    tud_dfu_finish_flashing(DFU_STATUS_OK);
    status(DFU_DNLOAD_IDLE, DFU_STATUS_OK);
  }
  assert(staged_len == sizeof(source) && memcmp(staged, source, sizeof(source)) == 0);
  control(0x21, DFU_REQUEST_DNLOAD, block, 0, NULL, NULL);
  status(DFU_MANIFEST, DFU_STATUS_OK);
  assert(manifests == 1 && manifest_pending);
  manifest_pending = false;
  tud_dfu_finish_flashing(DFU_STATUS_OK);
  status(DFU_IDLE, DFU_STATUS_OK);
  puts("PASS: arbitrary 8195-byte image, 4096-byte DFU blocks, EP0 packets <=64 bytes, async busy and manifestation");
  control(0x21, DFU_REQUEST_ABORT, 0, 0, NULL, NULL);
  assert(aborts == 1 && staged_len == 0);
  control(0x21, DFU_REQUEST_DNLOAD, 9, 8, source, NULL);
  status(DFU_DNBUSY, DFU_STATUS_OK);
  status(DFU_ERROR, DFU_STATUS_ERR_ADDRESS);
  control(0x21, DFU_REQUEST_CLRSTATUS, 0, 0, NULL, NULL);
  status(DFU_IDLE, DFU_STATUS_OK);
  control(0x21, DFU_REQUEST_DNLOAD, 0, CFG_TUD_DFU_XFER_BUFSIZE + 1, source, NULL);
  assert(stalled);
  control(0xa1, DFU_REQUEST_UPLOAD, 0, 8, NULL, source);
  assert(stalled);
  puts(
      "PASS: abort, application rejects out-of-order block, clear error, oversized download and unsupported upload "
      "stall");
  enumerate(5);
  status(DFU_IDLE, DFU_STATUS_OK);
  control(0x21, DFU_REQUEST_DNLOAD, 0, 8, source, NULL);
  status(DFU_DNBUSY, DFU_STATUS_OK);
  busy = false;
  tud_dfu_finish_flashing(DFU_STATUS_OK);
  status(DFU_DNLOAD_IDLE, DFU_STATUS_OK);
  control(0x21, DFU_REQUEST_DNLOAD, 1, 0, NULL, NULL);
  status(DFU_MANIFEST, DFU_STATUS_OK);
  tud_dfu_finish_flashing(DFU_STATUS_ERR_VERIFY);
  status(DFU_ERROR, DFU_STATUS_ERR_VERIFY);
  puts("PASS: bus reset and different address, application reports image verification failure");
  printf("PASS: %u data packets through real TinyUSB core/DFU; mock controller only, no hardware test\n", packets);
  assert(tusb_deinit(0));
  assert(su_dfu_bind_buffer(NULL, 0));
  free(workspace);
  // Repeated connections own distinct, initialized buffers and release them
  // only after TinyUSB stops using the pointer. ASan catches stale accesses.
  for (unsigned session = 0; session < 32; ++session) {
    workspace = malloc(CFG_TUD_DFU_XFER_BUFSIZE);
    assert(workspace);
    memset(workspace, 0xa5, CFG_TUD_DFU_XFER_BUFSIZE);
    assert(su_dfu_bind_buffer(workspace, CFG_TUD_DFU_XFER_BUFSIZE));
    assert(tusb_rhport_init(0, &init));
    enumerate(7);
    status(DFU_IDLE, DFU_STATUS_OK);
    staged_len = 0;
    expected_block = 0;
    busy = false;
    assert(control(0x21, DFU_REQUEST_DNLOAD, 0, CFG_TUD_DFU_XFER_BUFSIZE, source, NULL) == CFG_TUD_DFU_XFER_BUFSIZE);
    assert(memcmp(workspace, source, CFG_TUD_DFU_XFER_BUFSIZE) == 0);
    status(DFU_DNBUSY, DFU_STATUS_OK);
    busy = false;
    tud_dfu_finish_flashing(DFU_STATUS_OK);
    status(DFU_DNLOAD_IDLE, DFU_STATUS_OK);
    control(0x21, DFU_REQUEST_ABORT, 0, 0, NULL, NULL);
    assert(tusb_deinit(0));
    assert(su_dfu_bind_buffer(NULL, 0));
    free(workspace);
  }
  puts("PASS: buffer validation, 32 full-block reconnect/abort/deinit/free cycles with production DFU wrapper");
  return 0;
}
