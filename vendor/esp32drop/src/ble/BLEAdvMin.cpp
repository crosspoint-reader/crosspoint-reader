/* BLEAdvMin.cpp -- bundled with ESP32Drop, same 0BSD terms as the rest of the library.
 *
 * ⚠️ THE WHOLE FILE IS BEHIND ESP32DROP_SENDER, and that is not tidiness.
 *
 * Arduino compiles every .cpp under a library's src/ whatever the sketch defines, so a
 * guard around the declarations would not be enough: this file defines
 *
 *     extern "C" bool btInUse(void) { return true; }
 *
 * which overrides a weak symbol in the Arduino core and stops it releasing the BT
 * controller's memory before setup(). That is exactly right for a sender and exactly wrong
 * for everyone else -- a sketch that only RECEIVES AirDrop would silently give up about
 * 36 KB of internal DRAM to a radio it never turns on, in a library whose whole argument is
 * that the application must be left real memory. Compiling to nothing is the only way a
 * receiving build carries none of it.
 *
 * Define ESP32DROP_SENDER (or -DESP32DROP_SENDER) to build the sender. The GreetingCard
 * example does.
 */
#if defined(ESP32DROP_SENDER)

#include "BLEAdvMin.h"

#include <Arduino.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* ---- Keeping the BLE memory alive -------------------------------------------------
 *
 * THIS IS THE PART THAT MAKES OR BREAKS THE WHOLE FILE. The Arduino core frees the BT
 * controller's ~36 KB back to the general heap BEFORE setup() runs, unless something in
 * the image declares that a BLE library is linked (esp32-hal-misc.c, initArduino()).
 * Calling esp_bt_controller_init() after that release does not return an error — it
 * initialises a controller on top of memory the heap has already handed out, and on an
 * S3 that HANGS. Three separate hypotheses were measured and refuted before this was
 * found.
 *
 * Core 3.x provides the intended mechanism: a header whose constructor sets
 * _bleLibraryInUse, which the core's weak bleInUse() returns. Older cores only had the
 * now-deprecated btInUse() weak symbol, so that is the fallback.
 *
 * ⚠️ This must live in a translation unit the linker actually pulls in. It is in the same
 *    file as bleadv_begin() for exactly that reason — put it in a file of its own and an
 *    archived object providing nothing else could be dropped, silently, with the only
 *    symptom being a hang. */
#if __has_include("esp32-hal-alloc-ble-mem.h")
#include "esp32-hal-alloc-ble-mem.h"
#else
extern "C" bool btInUse(void) { return true; }
#endif

/* ---- State. Everything static; this library allocates nothing of its own. ---------- */
static bool s_started = false;
static int s_ram_cost = 0;
static int s_ram_init = 0;             /* the esp_bt_controller_init() share */
static uint8_t s_own_addr_type = 0x00; /* 0 = public, 1 = random */
static uint16_t s_interval = 30;
static SemaphoreHandle_t s_cmd_done = nullptr;
static volatile uint16_t s_ack_opcode = 0;
static volatile uint8_t s_ack_status = 0xFF;
static volatile uint8_t s_ack_params[8]; /* return parameters after the status byte */
static volatile uint8_t s_ack_plen = 0;

/* ---- The HCI transport ------------------------------------------------------------
 *
 * Four commands, one direction, plus the Command Complete that comes back. Packets are
 * H4-framed: a type byte, then the standard HCI structure.
 *
 * The event callback runs on the controller's task. It does the minimum — pick the status
 * out of a Command Complete and post it — because everything else is the caller's job. */
static int hci_evt_cb(uint8_t* d, uint16_t n) {
  /* 04 | evt | plen | num_pkts | opcode_lo | opcode_hi | status ... */
  if (n >= 7 && d[0] == 0x04 && d[1] == 0x0E) {
    s_ack_opcode = (uint16_t)d[4] | ((uint16_t)d[5] << 8);
    s_ack_status = d[6];
    /* Commands that answer with data (Read BD_ADDR) put it after the status byte. */
    uint8_t n_ret = (uint8_t)((n - 7 > (int)sizeof s_ack_params) ? sizeof s_ack_params : n - 7);
    for (uint8_t i = 0; i < n_ret; i++) s_ack_params[i] = d[7 + i];
    s_ack_plen = n_ret;
    if (s_cmd_done) {
      BaseType_t hp = pdFALSE;
      xSemaphoreGiveFromISR(s_cmd_done, &hp);
      if (hp) portYIELD_FROM_ISR();
    }
  }
  return 0;
}
static void hci_rdy_cb(void) {}
static const esp_vhci_host_callback_t s_vhci_cb = {hci_rdy_cb, hci_evt_cb};

/* Send one HCI command and wait for its Command Complete.
 *
 * Synchronous on purpose. Fire-and-forget with a delay() after each command also works
 * and is what the original probe did, but then "the controller rejected the advertising
 * parameters" and "the controller is advertising" look identical from the outside. The
 * cost of knowing is one semaphore. */
static int hci_cmd(uint16_t opcode, const uint8_t* p, uint8_t plen) {
  uint8_t buf[4 + 255];
  buf[0] = 0x01; /* H4: command packet */
  buf[1] = (uint8_t)(opcode & 0xFF);
  buf[2] = (uint8_t)(opcode >> 8);
  buf[3] = plen;
  if (plen) memcpy(&buf[4], p, plen);

  /* Bounded, not a bare while(): a controller that never reports itself ready must fail
     loudly rather than park the calling task for ever. */
  for (int i = 0; i < 200 && !esp_vhci_host_check_send_available(); i++) delay(1);
  if (!esp_vhci_host_check_send_available()) return BLEADV_E_VHCI_BUSY;

  s_ack_opcode = 0;
  s_ack_status = 0xFF;
  esp_vhci_host_send_packet(buf, 4 + plen);

  /* Take whatever Command Completes arrive until ours does, or time out. The controller
     may emit unrelated events; matching on the opcode is what makes this reliable. */
  for (int i = 0; i < 10; i++) {
    if (xSemaphoreTake(s_cmd_done, pdMS_TO_TICKS(200)) != pdTRUE) break;
    if (s_ack_opcode == opcode) return (int)s_ack_status; /* 0 = HCI success */
  }
  return BLEADV_E_VHCI_BUSY;
}

/* ---- Advertising parameters (0x2006) ---------------------------------------------- */
static int set_adv_params(void) {
  const uint16_t iv = (uint16_t)((uint32_t)s_interval * 1000 / 625); /* 0.625 ms units */
  const uint8_t par[15] = {
      (uint8_t)(iv & 0xFF),
      (uint8_t)(iv >> 8), /* min interval                  */
      (uint8_t)(iv & 0xFF),
      (uint8_t)(iv >> 8), /* max interval                  */
      0x03,               /* ADV_NONCONN_IND               */
      s_own_addr_type,    /* own address type              */
      0x00,
      0,
      0,
      0,
      0,
      0,
      0,    /* peer address type + address   */
      0x07, /* channel map: 37 | 38 | 39     */
      0x00  /* filter policy: allow any      */
  };
  return hci_cmd(0x2006, par, sizeof par);
}

/* ---- Public API -------------------------------------------------------------------- */

int bleadv_begin(const uint8_t* adv, size_t len, const bleadv_config_t* cfg) {
  if (s_started) return BLEADV_E_ALREADY;
  if (len > 31) return BLEADV_E_TOOLONG;

  bleadv_config_t c = cfg ? *cfg : bleadv_config_default();
  s_interval = c.interval_ms;

  const int before = (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

  esp_bt_controller_config_t bc = BT_CONTROLLER_INIT_CONFIG_DEFAULT();

  /* TURN OFF EVERYTHING A BROADCASTER DOES NOT DO.
   *
   * The default config provisions a general-purpose BLE controller. Each field below is
   * documented in esp_bt.h as "Configurable in menuconfig", which is what makes it legal
   * to set here, before init. Measured effect of this block: 35,820 B -> 26,620 B.
   *
   * A non-connectable, non-scannable advertiser never listens: it answers no SCAN_REQ,
   * accepts no CONNECT_IND, and runs no link-layer control procedure. So every feature
   * that exists to look at the air is dead weight. */
  bc.connect_en = false;       /* never accepts a connection            */
  bc.scan_en = false;          /* never scans                           */
  bc.enc_en = false;           /* never pairs or encrypts               */
  bc.ble_max_act = 1;          /* one BLE instance: this advertiser     */
  bc.ble_adv_dup_filt_max = 1; /* scan duplicate filter; scan is off    */
  bc.normal_adv_size = 0;      /* scan duplicate lists, both unused     */
  bc.mesh_adv_size = 0;
  bc.ble_50_feat_supp = false; /* legacy ADV needs no BLE 5.0 features  */
  bc.ble_chan_ass_en = 0;      /* channel assessment: measures the band */
  bc.ble_aa_check = false;     /* checks an Access Address never seen   */
  bc.ble_ping_en = 0;          /* LE ping is connection-only            */
  bc.hw_recorrect_en = 0;      /* coded-PHY correction, PHY unused      */

  /* ⚠️ ble_cca_mode = 0 means transmitting without listening first. That is what
   *    ADV_NONCONN_IND on the three advertising channels does by default anyway; this is
   *    not politeness being discarded, it is a feature that was never in the path. */
  bc.ble_cca_mode = 0;

  bc.controller_task_stack_size = c.task_stack;
  bc.run_in_flash = c.run_in_flash;

  esp_err_t e = esp_bt_controller_init(&bc);
  if (e != ESP_OK) return (int)e;
  /* Split the two calls. Which one holds the memory decides where a saving could come
     from at all: init is where the controller sizes its world from the config struct,
     enable is where it turns the radio on. */
  const int after_init = (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  s_ram_init = before - after_init;

  e = esp_bt_controller_enable(ESP_BT_MODE_BLE);
  if (e != ESP_OK) {
    esp_bt_controller_deinit();
    return (int)e;
  }

  if (!s_cmd_done) s_cmd_done = xSemaphoreCreateBinary();
  esp_vhci_host_register_callback(&s_vhci_cb);

  s_started = true; /* hci_cmd() and the setters need this set */
  s_ram_cost = before - (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

  int rc = hci_cmd(0x0C03, nullptr, 0); /* HCI Reset */
  if (rc == 0) rc = set_adv_params();
  if (rc == 0) rc = bleadv_set_data(adv, len);
  if (rc != 0) {
    bleadv_end();
    return rc;
  }

  if (c.tx_power != 0xFF) {
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, (esp_power_level_t)c.tx_power);
  }
  return BLEADV_OK;
}

int bleadv_set_data(const uint8_t* adv, size_t len) {
  if (!s_started) return BLEADV_E_NOT_STARTED;
  if (len > 31) return BLEADV_E_TOOLONG;

  /* 0x2008 always carries 32 bytes: a significant-length byte and a fixed 31-byte field.
     The trailing padding is not transmitted. */
  uint8_t d[32];
  memset(d, 0, sizeof d);
  d[0] = (uint8_t)len;
  if (len) memcpy(&d[1], adv, len);
  return hci_cmd(0x2008, d, sizeof d);
}

int bleadv_set_random_address(const uint8_t addr[6]) {
  if (!s_started) return BLEADV_E_NOT_STARTED;
  uint8_t a[6];
  memcpy(a, addr, 6);
  a[5] |= 0xC0;                   /* static random address: top two bits set */
  int rc = hci_cmd(0x2005, a, 6); /* LE Set Random Address */
  if (rc != 0) return rc;
  s_own_addr_type = 0x01;
  return set_adv_params(); /* the address type lives in the params, so resend */
}

int bleadv_enable(bool on) {
  if (!s_started) return BLEADV_E_NOT_STARTED;
  uint8_t en = on ? 1 : 0;
  return hci_cmd(0x200A, &en, 1);
}

int bleadv_end(void) {
  if (!s_started) return BLEADV_E_NOT_STARTED;
  uint8_t en = 0;
  hci_cmd(0x200A, &en, 1);
  delay(50); /* let the current advertising event finish */
  esp_bt_controller_disable();
  esp_bt_controller_deinit();
  /* Our own semaphore was one of the blocks that survived teardown. A library whose
     end() does not undo its begin() has no standing to complain about the controller's
     residue. Recreated by the next begin(). */
  if (s_cmd_done) {
    vSemaphoreDelete(s_cmd_done);
    s_cmd_done = nullptr;
  }
  s_started = false;
  s_ram_cost = 0;
  s_ram_init = 0;
  s_own_addr_type = 0x00;
  return BLEADV_OK;
}

/* Ask the controller what address it is actually advertising from. Ground truth: this is
 * what a scanner will show, and matching it is how a beacon is identified in a crowded
 * room. Uses the same synchronous command path, so a failure is a return code. */
int bleadv_read_bd_addr(uint8_t out[6]) {
  if (!s_started) return BLEADV_E_NOT_STARTED;
  int rc = hci_cmd(0x1009, nullptr, 0); /* Read BD_ADDR */
  if (rc != 0) return rc;
  if (s_ack_plen < 6) return BLEADV_E_VHCI_BUSY;
  for (int i = 0; i < 6; i++) out[i] = s_ack_params[i];
  return BLEADV_OK;
}

int bleadv_release_forever(void) {
  if (s_started) bleadv_end();
  /* esp_bt_mem_release() is a superset of esp_bt_controller_mem_release(): controller
     memory plus host memory. The Arduino core --wraps both and tracks the release, so a
     second call is a no-op rather than corruption. */
  esp_err_t e = esp_bt_mem_release(ESP_BT_MODE_BLE);
  return (e == ESP_OK) ? BLEADV_OK : (int)e;
}

int bleadv_ram_cost(void) { return s_ram_cost; }
int bleadv_ram_cost_init(void) { return s_ram_init; }
bool bleadv_active(void) { return s_started; }

#else
/* Not a sender build: this translation unit is deliberately empty. No BLE controller, no
   btInUse() override, no memory surrendered. */
typedef int ad_ble_not_built;
#endif /* ESP32DROP_SENDER */
