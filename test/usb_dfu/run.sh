#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
IMAGE=${1:?Path to an X3 application image required}
TEST_TMP=$(mktemp -d)
trap 'rm -rf "$TEST_TMP"' EXIT
c++ -DCROSSPOINT_VERSION=\"host-test\" -std=c++20 -fsanitize=address,undefined -Ivendor/x3-usb-dfu/src test/usb_dfu/identity_test.cpp -o "$TEST_TMP/identity"
"$TEST_TMP/identity"
c++ -DCROSSPOINT_VERSION=\"host-test\" -std=c++20 -g -O1 -fsanitize=address,undefined -Ilib/hal test/usb_dfu/gate_test.cpp -o "$TEST_TMP/gate"
"$TEST_TMP/gate"
c++ -DCROSSPOINT_VERSION=\"host-test\" -std=c++20 -g -O1 -fsanitize=address,undefined -Itest/usb_dfu/detector-stubs -Ilib/hal -Ivendor/x3-usb-dfu/src \
  test/usb_dfu/detector_test.cpp lib/hal/HalUsbDfu.cpp -o "$TEST_TMP/detector"
"$TEST_TMP/detector"
c++ -DCROSSPOINT_VERSION=\"host-test\" -std=c++20 -g -O1 -fsanitize=address,undefined -Wno-deprecated-declarations \
  -Itest/usb_dfu/stubs -Ilib/Memory -Isrc/network -Ivendor/x3-usb-dfu/src \
  test/usb_dfu/direct_flash_test.cpp vendor/x3-usb-dfu/src/DfuSession.cpp src/network/UsbFlashWriter.cpp \
  src/network/FirmwareFlasher.cpp src/network/FirmwareBoardTag.cpp -o "$TEST_TMP/flash"
"$TEST_TMP/flash" "$IMAGE"

c++ -DCROSSPOINT_VERSION=\"host-test\" -std=c++20 -pthread -g -O1 -fsanitize=address,undefined \
  -Itest/usb_dfu/service-stubs -Itest/usb_dfu/stubs -Ilib/hal -Isrc/network \
  test/usb_dfu/service_test.cpp -o "$TEST_TMP/service"
"$TEST_TMP/service"

c++ -DCROSSPOINT_VERSION=\"host-test\" -std=c++20 -fsanitize=address,undefined -Ivendor/x3-usb-dfu/src test/usb_dfu/listen_policy_test.cpp -o "$TEST_TMP/listen"
"$TEST_TMP/listen"

c++ -DCROSSPOINT_VERSION=\"host-test\" -std=c++20 -g -O1 -fsanitize=address,undefined \
  -Itest/usb_dfu/layout-stubs -Itest/usb_dfu/stubs -Isrc \
  test/usb_dfu/layout_test.cpp -o "$TEST_TMP/layout"
"$TEST_TMP/layout" "${IMAGE%.bin}-layout.svg"

clang -std=c11 -g -O1 -U__ARM_FEATURE_UNALIGNED -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Wall -Wextra -Werror -Itest/usb_dfu/tinyusb-stubs -Ivendor/x3-usb-dfu/src \
  -Ivendor/x3-usb-dfu/config -Ivendor/x3-usb-dfu/tinyusb/src \
  test/usb_dfu/tinyusb_buffer_test.c vendor/x3-usb-dfu/tinyusb/src/tusb.c \
  vendor/x3-usb-dfu/tinyusb/src/device/usbd.c vendor/x3-usb-dfu/tinyusb/src/common/tusb_fifo.c \
  vendor/x3-usb-dfu/src/tinyusb_dfu.c -o "$TEST_TMP/tinyusb-buffer"
UBSAN_OPTIONS=halt_on_error=1 "$TEST_TMP/tinyusb-buffer"

for source in dcd device codec; do
  clang -std=c11 -g -O1 -U__ARM_FEATURE_UNALIGNED -fsanitize=address,undefined \
    -Itest/usb_dfu/tinyusb-stubs -Ivendor/x3-usb-dfu/src -Ivendor/x3-usb-dfu/tinyusb/src \
    -c "vendor/x3-usb-dfu/src/$source.c" -o "$TEST_TMP/$source.o"
done
c++ -DCROSSPOINT_VERSION=\"host-test\" -std=c++20 -g -O1 -U__ARM_FEATURE_UNALIGNED -fsanitize=address,undefined \
  -Itest/usb_dfu/tinyusb-stubs -Ilib/Memory -Ivendor/x3-usb-dfu/src -Ivendor/x3-usb-dfu/tinyusb/src \
  test/usb_dfu/workspace_test.cpp "$TEST_TMP/dcd.o" "$TEST_TMP/device.o" "$TEST_TMP/codec.o" \
  -o "$TEST_TMP/workspace"
UBSAN_OPTIONS=halt_on_error=1 "$TEST_TMP/workspace"
