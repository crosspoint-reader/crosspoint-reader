#include <cassert>
#include <cstdio>
#include <thread>

#include "../../vendor/native-usb-dfu/NativeDfu.cpp"

struct TestSink : usb_update::Sink {
  std::atomic<bool> entered{false}, proceed{false};
  unsigned aborts = 0;
  bool begin(uint32_t, const uint8_t*) override {
    entered = true;
    while (!proceed) delay(1);
    return true;
  }
  bool append(const uint8_t*, size_t) override { return true; }
  std::atomic<bool> finishing{false}, finishAllowed{false};
  usb_update::Error finish() override {
    finishing = true;
    while (!finishAllowed) delay(1);
    return usb_update::Error::None;
  }
  void abort() override { ++aborts; }
};
std::atomic<bool> stopTest{false};
bool cancelledTest() { return stopTest.load(); }
template <class P>
void until(P p) {
  const auto start = millis();
  while (!p()) {
    assert(millis() - start < 2000);
    delay(1);
  }
}
void sendHeader() {
  uint8_t header[64]{};
  memcpy(header, "X3DFU001", 8);
  header[10] = 1;  // 65536 bytes
  Lock lock;
  tud_dfu_download_cb(0, 0, header, sizeof(header));
}
int main() {
  uint8_t count = 99;
  assert(usbd_app_driver_get_cb(&count) == nullptr && count == 0);
  assert(native_dfu::begin());
  assert(usbd_app_driver_get_cb(&count) != nullptr && count == 1);
  assert(interfaceNumber == 1);
  TestSink sink;
  usb_update::Session session(sink, 0x640000, 65536, 10000, 1000, 4096);
  usb_update::Report report;
  std::thread worker([&] { report = usb_update::run(session, cancelledTest, 0, 30, true, nullptr); });
  until([] {
    Lock lock;
    return accepting;
  });
  sendHeader();
  until([&] { return sink.entered.load(); });
  // A host reset during flash work must not overwrite the worker's packet.
  reset(0);
  tusb_control_request_t request{{TUSB_REQ_TYPE_CLASS}, DFU_REQUEST_DNLOAD, 1};
  assert(!control(0, CONTROL_STAGE_SETUP, &request));
  sink.proceed = true;
  until([] {
    Lock lock;
    return !processing;
  });
  stopTest = true;
  worker.join();
  assert(!report.completed && sink.aborts > 0);
  // Mutations stay disabled after stop, including when USB is used for storage.
  assert(!control(0, CONTROL_STAGE_SETUP, &request));
  native_dfu::disconnect();
  assert(!native_dfu::connected());
  native_dfu::reconnectForStorage();
  assert(native_dfu::connected());
  assert(!control(0, CONTROL_STAGE_SETUP, &request));

  stopTest = false;
  TestSink good;
  good.proceed = true;
  usb_update::Session next(good, 0x640000, 65536, 10000, 1000, 4096);
  std::thread success([&] { report = usb_update::run(next, cancelledTest, 0, 30, true, nullptr); });
  until([] {
    Lock lock;
    return accepting;
  });
  unsigned done = completions;
  sendHeader();
  until([&] { return completions > done; });
  static uint8_t data[4096]{};
  for (uint16_t n = 1; n <= 16; ++n) {
    done = completions;
    {
      Lock lock;
      tud_dfu_download_cb(0, n, data, sizeof(data));
    }
    until([&] { return completions > done; });
  }
  done = completions;
  {
    Lock lock;
    tud_dfu_manifest_cb(0);
  }
  until([&] { return good.finishing.load(); });
  request.bRequest = DFU_REQUEST_GETSTATUS;
  assert(control(0, CONTROL_STAGE_SETUP, &request));  // This reply was still busy.
  good.finishAllowed = true;
  until([&] { return completions > done; });
  assert(control(0, CONTROL_STAGE_ACK, &request));
  {
    Lock lock;
    assert(!hostConfirmed);
  }
  assert(control(0, CONTROL_STAGE_SETUP, &request));  // A new terminal reply.
  reset(0);                                           // Invalidates its pending ACK.
  assert(control(0, CONTROL_STAGE_ACK, &request));
  {
    Lock lock;
    assert(!hostConfirmed);
  }
  assert(control(0, CONTROL_STAGE_SETUP, &request));
  assert(control(0, CONTROL_STAGE_ACK, &request));
  success.join();
  assert(report.completed && report.hostConfirmed && report.received == 65536);
  TestSink idleSink;
  usb_update::Session idleSession(idleSink, 0x640000, 65536, 10000, 1000, 4096);
  mounted = false;
  const auto idleReport = usb_update::run(idleSession, nullptr, 0, 10, true, nullptr);
  assert(!idleReport.completed && !idleSink.entered);
  puts("PASS: native worker reset during header, packet ownership, stop/storage exclusion, complete acknowledgement");
}
