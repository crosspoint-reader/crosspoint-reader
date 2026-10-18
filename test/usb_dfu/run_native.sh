#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
IMAGE=${1:?Path to an X4 Pro application image required}
TEST_TMP=$(mktemp -d)
trap 'rm -rf "$TEST_TMP"' EXIT
c++ -DCROSSPOINT_VERSION=\"host-test\" -std=c++20 -pthread -g -O1 -fsanitize=address,undefined \
  -Itest/usb_dfu/native-stubs -Itest/usb_dfu/service-stubs -Ivendor/x3-usb-dfu/src \
  test/usb_dfu/native_transport_test.cpp vendor/x3-usb-dfu/src/DfuSession.cpp -o "$TEST_TMP/native"
"$TEST_TMP/native"
c++ -DCROSSPOINT_VERSION=\"host-test\" -std=c++20 -g -O1 -fsanitize=address,undefined -Wno-deprecated-declarations \
  -DFREEINK_DEVICE_X4PRO=1 -Itest/usb_dfu/stubs -Ilib/Memory -Isrc/network -Ivendor/x3-usb-dfu/src \
  test/usb_dfu/direct_flash_test.cpp vendor/x3-usb-dfu/src/DfuSession.cpp src/network/UsbFlashWriter.cpp \
  src/network/FirmwareFlasher.cpp src/network/FirmwareBoardTag.cpp -o "$TEST_TMP/flash"
"$TEST_TMP/flash" "$IMAGE"
