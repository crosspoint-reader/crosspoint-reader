// Weak fallback definition of `_btLibraryInUse` for the BLE-controller-only build.
//
// The arduino-esp32 core header cores/esp32/esp32-hal-bt-mem.h emits a global
// constructor (`_setBtLibraryInUse`) that references `_btLibraryInUse` whenever BLE
// hardware is present, but the core only DEFINES that symbol (in esp32-hal-bt.c)
// when an IDF host stack — CONFIG_BLUEDROID_ENABLED or CONFIG_NIMBLE_ENABLED — is
// enabled. CrossPoint runs the ESP-IDF BLE *controller* only, with NimBLE-Arduino
// supplying the host (both IDF host stacks are disabled in custom_sdkconfig), so the
// core never defines it and both the core lib and NimBLEDevice.cpp.o fail the final
// firmware link with "undefined reference to `_btLibraryInUse'".
//
// scripts/patch_bt_mem.py turns the header's declaration into a weak definition, but
// that framework file gets reset by package reinstalls and the custom_sdkconfig core
// rebuild, so the patch does not always survive to compile time. This weak definition
// lives in our own always-linked source and resolves the reference in firmware.elf
// regardless of the header's state. It is `weak`, so if a host stack is ever enabled
// (giving the core its own strong definition), that strong symbol wins with no clash;
// and it coexists with the header's weak def when the patch did apply.
extern "C" __attribute__((weak)) bool _btLibraryInUse = false;
