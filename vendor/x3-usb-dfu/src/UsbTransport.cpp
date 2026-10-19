#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>
#include <driver/dedic_gpio.h>
#include <esp32c3/rom/cache.h>
#include <esp_cpu.h>
#include <hal/dedic_gpio_cpu_ll.h>
#include <soc/gpio_reg.h>
#include <soc/io_mux_reg.h>
#include <soc/usb_serial_jtag_reg.h>

#include "DfuBuffer.h"
#include "UsbListenPolicy.h"
#include "UsbUpdater.h"
#include "UsbWorkspace.h"
#include "reply_policy.h"
#include "softusb_dcd.h"
#include "tusb.h"

using usb_update::PipeResult;
extern "C" unsigned su_pipe_receive(PipeResult*, unsigned, const uint16_t*, const uint8_t*, const uint16_t*,
                                    su_reply_policy*);
extern "C" void su_pipe_receive_end();
extern "C" {
uint32_t native_sof_count = 0;
}

// The assembly has already checked stuffing, EOP, token CRC5 and data CRC16
// before setting ready with error==0. Use its CRC residue instead of hashing
// each packet a second time while the host is sending the next one.
static bool IRAM_ATTR parsedPhysical(const PipeResult& r, su_packet& p) {
  if (r.error || !r.ready || r.count < 2 || r.count > SU_WIRE_MAX || r.wire[0] != 0x80) return false;
  p.pid = r.wire[1];
  p.len = 0;
  p.token = 0;
  if (((p.pid >> 4) ^ (p.pid & 15)) != 15) return false;
  if (p.pid == SU_DATA0 || p.pid == SU_DATA1) {
    if (r.count < 4 || r.crc != 0xb001) return false;
    p.len = r.count - 4;
    if (p.len) memcpy(p.data, r.wire + 2, p.len);
    return true;
  }
  if (p.pid == SU_OUT || p.pid == SU_SETUP || p.pid == SU_IN || p.pid == SU_SOF) {
    if (r.count != 4) return false;
    p.token = (r.wire[2] | (uint16_t(r.wire[3]) << 8)) & 2047;
    return true;
  }
  return r.count == 2 && (p.pid == SU_ACK || p.pid == SU_NAK || p.pid == SU_STALL);
}
static bool ready;
extern "C" bool su_phy_init(void) { return ready; }
extern "C" void su_phy_deinit(void) {}
extern "C" void su_phy_connect(bool connected) { (void)connected; }
extern "C" void su_phy_irq(bool enabled) { (void)enabled; }
extern "C" void su_phy_interrupt(void) {}
namespace usb_update {
extern Session* activeSession;
static UsbWorkspace* workspace;
static uint32_t cacheAddress, cacheLines;
static bool cacheLocked, cacheFailed;
static __attribute__((noinline, noclone)) bool IRAM_ATTR preload(uint32_t addr, uint32_t lines) {
  uint32_t saved = Cache_Start_ICache_Preload(addr, lines * 32, 0), begin = esp_cpu_get_cycle_count();
  while (!Cache_ICache_Preload_Done() && uint32_t(esp_cpu_get_cycle_count() - begin) < 1600000) {
  }
  bool done = Cache_ICache_Preload_Done();
  if (done) Cache_Lock_ICache_Items(addr, lines);
  Cache_End_ICache_Preload(saved);
  return done;
}
static __attribute__((noinline, noclone)) void IRAM_ATTR receive() {
  su_pipe_receive(workspace->results, 30000, workspace->crc, workspace->tokenCrc, workspace->pidMeta,
                  &workspace->policy);
}
static bool IRAM_ATTR continuousReset(bool keepMasked) {
  // Observe one continuous SE0 interval. A scheduler interruption between
  // samples could otherwise mistake two unrelated packet EOPs for a reset.
  bool observed = false;
  noInterrupts();
  if ((dedic_gpio_cpu_ll_read_in() & 3) == 0) {
    const uint32_t begin = esp_cpu_get_cycle_count();
    while ((dedic_gpio_cpu_ll_read_in() & 3) == 0) {
      if (uint32_t(esp_cpu_get_cycle_count() - begin) >= 1600) {
        observed = true;
        break;
      }
    }
  }
  if (!keepMasked) interrupts();
  return observed;
}
void suspendCache() {
  if (cacheLocked) {
    Cache_Unlock_ICache_Items(cacheAddress, cacheLines);
    cacheLocked = false;
  }
}
bool resumeCache() {
  if (cacheLocked) return true;
  noInterrupts();
  cacheLocked = cacheLines < 512 && preload(cacheAddress, cacheLines);
  interrupts();
  if (!cacheLocked) cacheFailed = true;
  return cacheLocked;
}
static __attribute__((noinline)) std::unique_ptr<UsbWorkspace> allocateWorkspace(Report& report) {
  if (workspace) {
    report.error = ESP_ERR_INVALID_STATE;
    return {};
  }
  // Keep large packet tables off the task stack and out of permanent BSS.
  // C3 has no PSRAM; this nothrow allocation is internal byte-accessible RAM.
  auto memory = makeUniqueNoThrow<UsbWorkspace>();
  if (!memory) {
    LOG_ERR("DFU", "USB workspace allocation failed: %u bytes", unsigned(sizeof(UsbWorkspace)));
    report.error = ESP_ERR_NO_MEM;
    return {};
  }
  static_assert(sizeof(memory->transfer) == CFG_TUD_DFU_XFER_BUFSIZE);
  if (!su_dfu_bind_buffer(memory->transfer, sizeof(memory->transfer))) {
    report.error = ESP_ERR_INVALID_ARG;
    return {};
  }
  workspace = memory.get();
  su_dcd_bind_device(&workspace->device);
  report.workspaceBytes = sizeof(UsbWorkspace);
  return memory;
}
Report IRAM_ATTR run(Session& session, bool (*cancelled)(), uint32_t durationMs, uint32_t hostIdleMs, bool background,
                     void (*linkState)(bool)) {
  Report report;
  auto memory = allocateWorkspace(report);
  if (!memory) return report;
  auto& crc = workspace->crc;
  auto& tokenCrc = workspace->tokenCrc;
  auto& pidMeta = workspace->pidMeta;
  auto& policy = workspace->policy;
  auto& results = workspace->results;
  bool reportedConfigured = false;
  ConfigurationAck configuration;
  if (linkState) linkState(false);
  native_sof_count = 0;
  activeSession = &session;
  cacheLocked = cacheFailed = false;
  for (unsigned i = 0; i < 256; i++) {
    uint16_t c = i;
    for (unsigned b = 0; b < 8; b++) c = (c >> 1) ^ ((c & 1) ? 0xa001 : 0);
    crc[i] = c;
  }
  for (unsigned i = 0; i < 2048; i++) tokenCrc[i] = su_crc5(i);
  memset(pidMeta, 0, sizeof(pidMeta));
  for (uint8_t p : {uint8_t(SU_DATA0), uint8_t(SU_DATA1)}) pidMeta[p & 15] = p | (1 << 8);
  for (uint8_t p : {uint8_t(SU_SETUP), uint8_t(SU_OUT)}) pidMeta[p & 15] = p | (2 << 8);
  for (uint8_t p : {uint8_t(SU_ACK), uint8_t(SU_NAK), uint8_t(SU_STALL)}) pidMeta[p & 15] = p | (4 << 8);
  pidMeta[SU_IN & 15] = SU_IN | (6 << 8);
  pidMeta[SU_SOF & 15] = SU_SOF | (10 << 8);

  const uint32_t regs[] = {IO_MUX_GPIO18_REG, IO_MUX_GPIO19_REG,           GPIO_PIN18_REG,
                           GPIO_PIN19_REG,    GPIO_FUNC18_OUT_SEL_CFG_REG, GPIO_FUNC19_OUT_SEL_CFG_REG};
  uint32_t saved[6];
  for (unsigned i = 0; i < 6; i++) saved[i] = REG_READ(regs[i]);
  const uint32_t mask = BIT(18) | BIT(19), en = REG_READ(GPIO_ENABLE_REG) & mask, out = REG_READ(GPIO_OUT_REG) & mask,
                 conf = REG_READ(USB_SERIAL_JTAG_CONF0_REG);
  const uint32_t pulls = USB_SERIAL_JTAG_PAD_PULL_OVERRIDE | USB_SERIAL_JTAG_DP_PULLUP | USB_SERIAL_JTAG_DM_PULLUP |
                         USB_SERIAL_JTAG_DP_PULLDOWN | USB_SERIAL_JTAG_DM_PULLDOWN | USB_SERIAL_JTAG_PULLUP_VALUE;
  const uint32_t off = (conf & ~(pulls | USB_SERIAL_JTAG_USB_PAD_ENABLE)) | USB_SERIAL_JTAG_PAD_PULL_OVERRIDE;
  REG_WRITE(USB_SERIAL_JTAG_CONF0_REG, off);
  delay(1000);
  dedic_gpio_bundle_handle_t bundle = nullptr;
  int pins[] = {18, 19};
  dedic_gpio_bundle_config_t bc{};
  bc.gpio_array = pins;
  bc.array_size = 2;
  bc.flags.in_en = 1;
  bc.flags.out_en = 1;
  esp_err_t error = dedic_gpio_new_bundle(&bc, &bundle);
  uint32_t im = 0, io = 0, om = 0, oo = 0;
  if (!error) error = dedic_gpio_get_in_mask(bundle, &im);
  if (!error) error = dedic_gpio_get_in_offset(bundle, &io);
  if (!error) error = dedic_gpio_get_out_mask(bundle, &om);
  if (!error) error = dedic_gpio_get_out_offset(bundle, &oo);
  if (!error && (im != 3 || io != 0 || om != 3 || oo != 0)) error = ESP_ERR_INVALID_STATE;
  cacheAddress = uint32_t(uintptr_t(su_pipe_receive)) & ~31u;
  cacheLines = (uint32_t(uintptr_t(su_pipe_receive_end)) - cacheAddress + 31) / 32;
  bool initialized = false;
  if (!error) {
    gpio_set_pull_mode(GPIO_NUM_18, GPIO_FLOATING);
    gpio_set_pull_mode(GPIO_NUM_19, GPIO_FLOATING);
    gpio_input_enable(GPIO_NUM_18);
    gpio_input_enable(GPIO_NUM_19);
    gpio_set_drive_capability(GPIO_NUM_18, GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability(GPIO_NUM_19, GPIO_DRIVE_CAP_3);
    dedic_gpio_cpu_ll_write_all(2);
    dedic_gpio_cpu_ll_enable_output(0);
    if (!resumeCache()) error = ESP_FAIL;
  }
  if (!error) {
    ready = true;
    tusb_rhport_init_t init{TUSB_ROLE_DEVICE, TUSB_SPEED_FULL};
    initialized = tusb_rhport_init(0, &init);
    if (!initialized) error = ESP_FAIL;
  }
  if (!error) {
    su_bus_reset(su_dcd_device());
    su_dcd_service();
    tud_task();
    su_reply_policy_prepare(&policy, su_dcd_device(), 0);
    REG_WRITE(USB_SERIAL_JTAG_CONF0_REG, off | USB_SERIAL_JTAG_DP_PULLUP);
#ifdef USB_UPDATER_DIAGNOSTICS
    uint32_t traceArmedAt = 0;
#endif
    uint32_t start = millis(), yielded = start, lastActivity = start, confirmedAt = 0, completedAt = 0;
    bool resetHeld = false, idleStatusAck = false;
    uint32_t burstAt = 0;
    uint32_t unconfiguredAt = start;
    uint32_t lastSof = 0, lastSofAt = start;
    while ((!durationMs || uint32_t(millis() - start) < durationMs) && !cacheFailed) {
      // Detect sustained SE0 independently of packet EOP; one event per reset.
      bool criticalBurst = burstAt && uint32_t(millis() - burstAt) < 50;
      if (continuousReset(criticalBurst)) {
        if (!resetHeld) {
          burstAt = 0;
          criticalBurst = false;
          interrupts();
          unconfiguredAt = millis();
          configuration.reset();
          if (reportedConfigured && linkState) linkState(false);
          reportedConfigured = false;
          report.lastResetBytes = session.bytes();
          if (session.started() && !session.complete()) report.resetsDuringDownload++;
          session.abort();  // Reset protocol and release an unfinished transfer together.
          su_bus_reset(su_dcd_device());
          su_dcd_service();
          tud_task();
          su_reply_policy_prepare(&policy, su_dcd_device(), 0);
          report.resets++;
          resetHeld = true;
        }
      } else
        resetHeld = false;
      memset(results, 0, sizeof(results));
#ifdef USB_UPDATER_DIAGNOSTICS
      auto endpointBits = []() {
        const auto& e = su_dcd_device()->ep[0];
        return uint32_t(e.queued) | (uint32_t(e.toggle) << 1) | (uint32_t(e.accepted) << 2) | (uint32_t(e.length) << 8);
      };
      uint32_t before = endpointBits();
      noInterrupts();
      uint32_t entryCycle = esp_cpu_get_cycle_count();
      receive();
      uint32_t exitCycle = esp_cpu_get_cycle_count();
      if (!criticalBurst) interrupts();
#else
      noInterrupts();
      receive();
      if (!criticalBurst) interrupts();
#endif
      report.loops++;
      su_packet first, second;
      const su_packet *a = nullptr, *b = nullptr;
      if (parsedPhysical(results[0], first)) {
        a = &first;
        report.valid++;
      } else if (results[0].error != 2)
        report.invalid++;
      if (parsedPhysical(results[1], second)) b = &second;
#ifdef USB_UPDATER_DIAGNOSTICS
      if (!traceArmedAt && a && b && a->pid == SU_SETUP && b->len == 8 && b->data[0] == 0x21 && b->data[1] == 1 &&
          (b->data[6] | (unsigned(b->data[7]) << 8)) > 64)
        traceArmedAt = millis();
      Trace* trace = nullptr;
      if (traceArmedAt && results[0].error != 2) {
        trace = &report.trace[(report.traceCount++) % traceCapacity];
        *trace = {entryCycle,
                  exitCycle,
                  0,
                  results[0].wire[1],
                  results[1].wire[1],
                  results[0].error,
                  results[1].error,
                  results[0].count,
                  results[1].count,
                  policy.response,
                  before,
                  0,
                  esp_cpu_get_cycle_count(),
                  0,
                  0,
                  0};
      }
#endif
      if (a && b && a->pid == SU_SETUP && b->len == 8) {
        const unsigned length = b->data[6] | (unsigned(b->data[7]) << 8);
        const bool standardData = (b->data[0] & 0x60) == 0 && length > 0;
        const bool downloadData = b->data[0] == 0x21 && b->data[1] == 1 && length > 64;
        burstAt = standardData || downloadData ? millis() : 0;
      }
      if (a && a->pid == SU_IN && policy.host_ack) burstAt = 0;
      // DFU does not run the storage callback until a later GETSTATUS request.
      // Keep a bounded multi-packet DNLOAD free of scheduler/ISR receive gaps.
      if (burstAt && uint32_t(millis() - burstAt) < 50)
        noInterrupts();
      else
        interrupts();
      // The assembly consumes token+DATA / IN+ACK pairs. An isolated DATA or
      // handshake cannot advance endpoint state here; return to listening without
      // rebuilding identical reply tables when a token was missed during service.
      const bool transaction = a && (a->pid == SU_SETUP || a->pid == SU_OUT || a->pid == SU_IN);
      if (transaction || policy.response) {
        lastActivity = millis();
        // Wait for the host to complete GETSTATUS(DFU_IDLE), including OUT status.
        if (session.complete() && a && a->pid == SU_IN && policy.host_ack && policy.response == 20) {
          const auto& p = su_dcd_device()->ep[1].packet;
          idleStatusAck = p.len == 6 && p.data[0] == 0 && p.data[4] == DFU_IDLE;
        }
        bool confirmed =
            idleStatusAck && a && a->pid == SU_OUT && b && b->len == 0 && (policy.response & 3) == SU_REPLY_ACK;

        const bool statusAcknowledged =
            a && a->pid == SU_IN && policy.host_ack && su_dcd_device()->ep[1].packet.len == 0;
        if (!su_reply_policy_commit(su_dcd_device(), &policy, a, b)) {
          report.mismatch++;
          break;
        }
        if (a && b && a->pid == SU_SETUP && b->len == 8)
          configuration.setup(b->data[0], b->data[1], uint16_t(b->data[2]) | (uint16_t(b->data[3]) << 8),
                              uint16_t(b->data[6]) | (uint16_t(b->data[7]) << 8));
        if (statusAcknowledged) configuration.statusAck();
        report.commits++;
        const uint32_t serviceStart = micros();
#ifdef USB_UPDATER_DIAGNOSTICS
        if (trace) trace->commitEnd = esp_cpu_get_cycle_count();
#endif
        su_dcd_service();
#ifdef USB_UPDATER_DIAGNOSTICS
        if (trace) trace->dcdEnd = esp_cpu_get_cycle_count();
#endif
        tud_task();
#ifdef USB_UPDATER_DIAGNOSTICS
        if (trace) trace->taskEnd = esp_cpu_get_cycle_count();
#endif
        if (a && a->pid == SU_OUT)
          su_reply_policy_after_out(&policy, su_dcd_device(), 0);
        else
          su_reply_policy_prepare(&policy, su_dcd_device(), 0);
        const uint32_t serviceUs = micros() - serviceStart;
        if (serviceUs > report.maxServiceUs) report.maxServiceUs = serviceUs;
        if (serviceUs >= 100000) lastSofAt = millis();
        if (confirmed) {
          report.hostConfirmed = 1;
          if (!confirmedAt) confirmedAt = millis();
        }
      }
#ifdef USB_UPDATER_DIAGNOSTICS
      if (trace) {
        trace->serviceEnd = esp_cpu_get_cycle_count();
        trace->epAfter = endpointBits();
      }
#ifdef USB_UPDATER_DIAGNOSTIC_STOP
      if (traceArmedAt && uint32_t(millis() - traceArmedAt) > 12000) break;
#endif
#endif
      const bool configured = configuration.configured(tud_mounted());
      if (configured) {
        report.mounted = 1;
        unconfiguredAt = millis();
      }
      if (configured != reportedConfigured) {
        reportedConfigured = configured;
        if (linkState) linkState(configured);
      }
      if (background && enumerationExpired(configured, uint32_t(millis() - unconfiguredAt))) {
        report.exitReason = 2;  // Host never completed SET_CONFIGURATION: force a clean reattach.
        break;
      }
      if (session.complete() && !completedAt) completedAt = millis();
      if ((confirmedAt && uint32_t(millis() - confirmedAt) > 2000) ||
          (completedAt && uint32_t(millis() - completedAt) > 180000))
        break;
      if (native_sof_count != lastSof) {
        lastSof = native_sof_count;
        lastSofAt = millis();
      }
      if (hostIdleMs && uint32_t(millis() - lastSofAt) > (lastSof ? hostIdleMs : 15000)) {
        report.exitReason = 1;  // No host frames: unplug or suspend.
        break;
      }
      if (!hostIdleMs && session.started() && uint32_t(millis() - lastActivity) > 300000) break;
      const uint32_t window =
          listenWindowMs(background, configured, session.started(), uint32_t(millis() - lastActivity));
      if (uint32_t(millis() - yielded) >= window && (!burstAt || uint32_t(millis() - burstAt) > 50)) {
        if (!session.complete() && cancelled && cancelled()) {
          report.cancelled = 1;
          break;
        }
        // Keep the PHY resident across scheduler yields. Repeated cache
        // unlocking/preloading created extra blind spots during enumeration.
        delay(1);
        yielded = millis();
      }
    }
    interrupts();
    report.address = su_dcd_device()->address;
    report.received = session.bytes();
    report.total = session.size();
    report.completed = session.complete();
    report.sof = native_sof_count;
  }
  if (cacheFailed && !error) error = ESP_FAIL;
  ready = false;
  if (linkState) linkState(false);
  report.error = error;
  dedic_gpio_cpu_ll_enable_output(0);
  REG_WRITE(USB_SERIAL_JTAG_CONF0_REG, off);
  delay(1000);
  if (initialized) tusb_deinit(0);
  suspendCache();
  if (bundle) dedic_gpio_del_bundle(bundle);
  REG_WRITE(GPIO_ENABLE_W1TC_REG, mask);
  REG_WRITE(GPIO_OUT_W1TC_REG, mask);
  REG_WRITE(GPIO_OUT_W1TS_REG, out);
  for (unsigned i = 0; i < 6; i++) REG_WRITE(regs[i], saved[i]);
  REG_WRITE(GPIO_ENABLE_W1TS_REG, en);
  REG_WRITE(USB_SERIAL_JTAG_CONF0_REG, conf);
  report.restored = (REG_READ(GPIO_OUT_REG) & mask) == out && (REG_READ(GPIO_ENABLE_REG) & mask) == en &&
                    REG_READ(USB_SERIAL_JTAG_CONF0_REG) == conf;
  for (unsigned i = 0; i < 6; i++)
    if (REG_READ(regs[i]) != saved[i]) report.restored = 0;
  report.sessionError = uint32_t(session.error());
  if (!session.complete()) session.abort();
  activeSession = nullptr;
  // The PHY is stopped and TinyUSB is deinitialized before releasing buffers.
  su_dfu_bind_buffer(nullptr, 0);
  su_dcd_bind_device(nullptr);
  workspace = nullptr;
  return report;
}
}  // namespace usb_update
