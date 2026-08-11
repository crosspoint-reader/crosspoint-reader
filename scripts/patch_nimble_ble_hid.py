Import("env")  # PlatformIO injects this global.
from pathlib import Path


def patch_file(path: Path, replacements):
    if not path.exists():
        return False
    text = path.read_text()
    changed = False
    for old, new in replacements:
        if old in text:
            text = text.replace(old, new, 1)
            changed = True
    if changed:
        path.write_text(text)
    return changed


def disable_entire_c_file(path: Path, marker: str):
    if not path.exists():
        return False
    text = path.read_text()
    if marker in text:
        return False
    path.write_text(f"#if 0  /* {marker} */\n" + text + f"\n#endif  /* {marker} */\n")
    return True


def patch_nimble_for_crosspoint_ble_hid():
    project_dir = Path(env.subst("$PROJECT_DIR"))

    # The current pioarduino/ESP-IDF core already provides NimBLE's FreeRTOS NPL
    # symbols. NimBLE-Arduino also ships them, causing duplicate linker symbols.
    # The C++ API can use the core-provided NPL, so do not compile the duplicate
    # NimBLE-Arduino source body.
    npl = project_dir / ".pio/libdeps/default/NimBLE-Arduino/src/nimble/porting/npl/freertos/src/npl_os_freertos.c"
    if disable_entire_c_file(npl, "CrossPoint: ESP-IDF core already provides NimBLE FreeRTOS NPL symbols"):
        print("Patched duplicate NimBLE-Arduino FreeRTOS NPL out")

    # The X4 BLE keyboard host only needs normal HID scanning/connection. Periodic
    # advertising sync is unused and does not build cleanly against the current
    # pioarduino NimBLE port headers.
    periodic = project_dir / ".pio/libdeps/default/NimBLE-Arduino/src/nimble/nimble/host/src/ble_hs_periodic_sync.c"
    if patch_file(periodic, [
        (
            "#if MYNEWT_VAL(BLE_PERIODIC_ADV)\n",
            "#if 0  /* CrossPoint: BLE HID keyboard host does not use periodic advertising sync. */\n",
        )
    ]):
        print("Patched NimBLE periodic sync out for CrossPoint BLE HID host")


patch_nimble_for_crosspoint_ble_hid()
