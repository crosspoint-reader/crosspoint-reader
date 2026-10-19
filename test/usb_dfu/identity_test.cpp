#include <cassert>
#include <string>

#include "UsbIdentity.h"
int main() {
  using namespace usb_update;
  assert(stringDescriptor(0, 0)[1] == 0x0409);
  assert(!stringDescriptor(3, 0x0409));
  assert(!stringDescriptor(2, 0));
  for (bool x3 : {true, false}) {
    identityIsX3 = x3;
    const auto* d = stringDescriptor(2, 0x0409);
    std::string name;
    for (unsigned i = 1; i < (d[0] & 255) / 2; ++i) name += char(d[i]);
    assert(name == (x3 ? "CrossPoint X3 USB Update" : "CrossPoint X4 USB Update"));
    assert((d[0] >> 8) == 3);
  }
}
