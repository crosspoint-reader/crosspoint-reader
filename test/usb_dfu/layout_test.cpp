#include <cassert>
#include <fstream>

#include "activities/network/UsbDfuActivity.cpp"
namespace usb_dfu {
Snapshot testState;
Snapshot snapshot() { return testState; }
void allowTransfer() {}
void releaseScreen() {}
}  // namespace usb_dfu
static bool overlaps(Rect a, Rect b) {
  return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
}
int main(int argc, char** argv) {
  GfxRenderer r;
  MappedInputManager input;
  UsbDfuActivity activity(r, input);
  unsigned scenarios = 0;
  for (auto dims : {std::pair{600, 800}, std::pair{800, 600}, std::pair{480, 800}, std::pair{800, 480}})
    for (int line : {20, 24, 28})
      for (int header : {80, 100, 120})
        for (int mode = 0; mode < 5; mode++) {
          r.width = dims.first;
          r.height = dims.second;
          r.line = line;
          GUI.metrics.headerHeight = header;
          usb_dfu::testState = {};
          auto& s = usb_dfu::testState;
          s.running = true;
          s.total = 5628144;
          s.bytes = 1441792;
          s.elapsedMs = 36000;
          s.stage = usb_dfu::Stage::Receiving;
          if (mode == 1) {
            s.total = s.bytes = 0;
          }
          if (mode >= 2) {
            s.bytes = s.total;
          }
          if (mode == 3) s.stage = usb_dfu::Stage::Verifying;
          if (mode == 4) s.installed = true;
          const auto beforeAsync = r.asyncPaints;
          activity.render(RenderLock{});
          assert(r.asyncPaints == beforeAsync);
          unsigned percentages = 0;
          for (size_t i = 0; i < r.ops.size(); i++) {
            auto& a = r.ops[i];
            assert(a.rect.x >= 10 && a.rect.x + a.rect.width <= r.width - 10);
            assert(a.rect.y >= header + 10 && a.rect.y + a.rect.height <= r.height - 10);
            if (a.text.find('%') != std::string::npos) percentages++;
            for (size_t j = i + 1; j < r.ops.size(); j++)
              if (!a.text.empty() || !r.ops[j].text.empty()) assert(!overlaps(a.rect, r.ops[j].rect));
          }
          assert(percentages == 0);
          assert(r.ops.size() == 2);
          assert(r.ops[0].text == (mode == 4   ? STR_UPDATE_COMPLETE
                                   : mode == 3 ? STR_VALIDATING_FIRMWARE
                                               : STR_USB_DFU_RECEIVING));
          if (argc == 2 && dims.first == 600 && dims.second == 800 && line == 24 && header == 100 && mode == 0) {
            std::ofstream f(argv[1]);
            f << "<svg xmlns='http://www.w3.org/2000/svg' width='600' height='800' viewBox='0 0 600 800'><rect "
                 "width='600' height='800' fill='#f5f3e9'/><text x='20' y='70' font-family='sans-serif' font-size='24' "
                 "font-weight='bold'>USB Firmware Update</text><path d='M20 110H580' stroke='black'/>";
            for (auto& a : r.ops) {
              if (a.text.empty())
                f << "<rect x='" << a.rect.x << "' y='" << a.rect.y << "' width='" << a.rect.width << "' height='"
                  << a.rect.height << "' fill='" << (a.fill ? "black" : "none") << "' stroke='black'/>";
              else
                f << "<text x='300' y='" << a.rect.y + 20
                  << "' text-anchor='middle' font-family='sans-serif' font-size='24'>" << a.text << "</text>";
            }
            f << "</svg>";
          }
          scenarios++;
        }
  usb_dfu::testState.stage = usb_dfu::Stage::Receiving;
  usb_dfu::testState.installed = false;
  usb_dfu::testState.bytes = 1;
  activity.render(RenderLock{});
  assert(!r.pending);
  activity.onExit();
  assert(!r.pending);
  printf(
      "PASS: production DFU renderer, %u portrait/landscape/font/header/stage combinations; no overlaps, no "
      "progress, all rows inside content.\n",
      scenarios);
}
