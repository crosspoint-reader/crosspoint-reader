"""Check that a firmware retains USB MSC/CDC without unused TinyUSB RAM."""

import argparse
import subprocess


def check_symbols(symbols):
    names = {line.split()[-1] for line in symbols.splitlines() if line.split()}
    required = {"tud_msc_set_sense", "cdcd_init", "_mscd_itf"}
    forbidden = {
        "_audiod_fct", "ep_in_sw_buf", "ep_out_sw_buf", "_hidh_itf",
        "_midi_host", "_usbh_devices", "_dfu_ctx", "_netd_itf",
        "audiod_init", "hidh_init", "midih_init", "tuh_init", "tuh_rhport_init",
        "dfu_moded_init", "netd_init", "vendord_init", "videod_init", "hidd_init", "midid_init",
    }
    missing = required - names
    retained = forbidden & names
    if missing or retained:
        raise ValueError(f"TinyUSB link check: missing {sorted(missing)}, unwanted {sorted(retained)}")


def check_firmware(nm, firmware):
    result = subprocess.run([str(nm), "--defined-only", str(firmware)], check=True, capture_output=True, text=True)
    check_symbols(result.stdout)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("firmware")
    parser.add_argument("--nm", required=True)
    args = parser.parse_args()
    check_firmware(args.nm, args.firmware)
    print("TinyUSB link check passed (MSC/CDC only).")
