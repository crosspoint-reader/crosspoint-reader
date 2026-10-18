#include <cassert>

#include "UsbDfuPolicy.h"
int main() {
  for (unsigned model = 0; model < 3; ++model) {
    for (unsigned bits = 0; bits < 8; ++bits) {
      const bool controller = bits & 1, usbRom = bits & 2, rom = bits & 4;
      assert(usb_dfu::needsFallback(model == 1, model == 2, controller, usbRom, rom) == (model != 0 && bits != 0));
    }
  }
}
