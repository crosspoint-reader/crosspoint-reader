"""Host checks for the USB configuration overlay and firmware link guard."""

import importlib.util
from pathlib import Path
import runpy
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("check_tinyusb", ROOT / "scripts/check_tinyusb.py")
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


class TinyUsbTests(unittest.TestCase):
    def test_overlay_edits_change_the_compiler_cache_key(self):
        class BuildEnvironment(dict):
            def subst(self, value):
                return self["PROJECT_DIR"]

            def Append(self, **values):
                for key, value in values.items():
                    self.setdefault(key, []).extend(value)

        with tempfile.TemporaryDirectory() as folder:
            header = Path(folder) / "scripts/tinyusb/crosspoint_tusb_config.h"
            header.parent.mkdir(parents=True)

            def compiler_defines(content):
                header.write_text(content)
                env = BuildEnvironment(PROJECT_DIR=folder)
                runpy.run_path(
                    str(ROOT / "scripts/configure_tinyusb.py"),
                    init_globals={"env": env, "Import": lambda name: None},
                )
                return env["CPPDEFINES"]

            original = compiler_defines("#define CFG_TUH_ENABLED 0\n")
            self.assertEqual(original, compiler_defines("#define CFG_TUH_ENABLED 0\n"))
            self.assertNotEqual(original, compiler_defines("#define CFG_TUH_ENABLED 1\n"))

    def test_overlay_keeps_device_sizes_and_removes_other_drivers(self):
        # Model the SDK's permissive defaults; compile the actual overlay.
        enabled = (
            "TUD_ENABLED TUD_MSC TUD_CDC TUH_ENABLED TUSB_RHPORT1_MODE "
            "TUH_HUB TUH_CDC TUH_HID TUH_MSC TUH_MIDI "
            "TUD_HID TUD_MIDI TUD_AUDIO TUD_VIDEO TUD_DFU_RUNTIME TUD_DFU "
            "TUD_VENDOR TUD_NCM TUD_CUSTOM_CLASS"
        ).split()
        with tempfile.TemporaryDirectory() as folder:
            directory = Path(folder)
            (directory / "tusb_config.h").write_text(
                "\n".join(f"#define CFG_{name} 1" for name in enabled)
                + "\n#define CFG_TUD_MSC_BUFSIZE 4096\n#define CFG_TUD_CDC_RX_BUFSIZE 64\n"
            )
            source = '#include "crosspoint_tusb_config.h"\n'
            for name in enabled:
                expected = 1 if name in {"TUD_ENABLED", "TUD_MSC", "TUD_CDC"} else 0
                source += f'#if CFG_{name} != {expected}\n#error unexpected {name}\n#endif\n'
            source += '#if CFG_TUD_MSC_BUFSIZE != 4096 || CFG_TUD_CDC_RX_BUFSIZE != 64\n#error endpoint sizes changed\n#endif\n'
            result = subprocess.run(
                ["cc", "-E", "-x", "c", "-I", str(ROOT / "scripts/tinyusb"), "-I", folder, "-"],
                input=source, capture_output=True, text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_link_guard_accepts_msc_cdc(self):
        checker.check_symbols("4000 T tud_msc_set_sense\n4010 T cdcd_init\n3fc0 b _mscd_itf\n")

    def test_link_guard_rejects_unused_ram(self):
        with self.assertRaisesRegex(ValueError, "_audiod_fct"):
            checker.check_symbols("4000 T tud_msc_set_sense\n4010 T cdcd_init\n3fc0 b _mscd_itf\n3fd0 b _audiod_fct\n")

    def test_link_guard_rejects_missing_mass_storage(self):
        with self.assertRaisesRegex(ValueError, "tud_msc_set_sense"):
            checker.check_symbols("4010 T cdcd_init\n")


if __name__ == "__main__":
    unittest.main()
