"""Check upstream SD font bundles without network access."""

import hashlib
import importlib.util
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile


SCRIPTS = Path(__file__).resolve().parents[1] / "lib/EpdFont/scripts"
sys.path.insert(0, str(SCRIPTS))
spec = importlib.util.spec_from_file_location("build_sd_fonts", SCRIPTS / "build-sd-fonts.py")
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)


class PrebuiltSdFontsTest(unittest.TestCase):
    def test_bundle_validation_and_copy(self):
        header = struct.pack("<8sHHB19x", b"CPFONT\0\0", builder.CPFONT_VERSION, 1, 4)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive = root / "bundle.zip"
            cases = (
                ("valid", header, False, False, True),
                ("wrong checksum", header, True, False, False),
                ("missing size", header, False, True, False),
                ("truncated header", header[:10], False, False, False),
                ("wrong version", struct.pack("<8sHHB19x", b"CPFONT\0\0", 0, 1, 4), False, False, False),
                ("wrong styles", struct.pack("<8sHHB19x", b"CPFONT\0\0", builder.CPFONT_VERSION, 1, 1), False, False, False),
            )
            for label, data, wrong_hash, missing_size, succeeds in cases:
                with self.subTest(label=label):
                    with zipfile.ZipFile(archive, "w") as bundle:
                        bundle.writestr("Libron/Libron_12.cpfont", data)
                        if not missing_size:
                            bundle.writestr("Libron/Libron_14.cpfont", data)
                        bundle.writestr("../escape.cpfont", data)
                    family = {
                        "name": "Libron",
                        "sizes": [12, 14],
                        "prebuilt": {
                            "url": "https://example.com/bundle.zip",
                            "sha256": "0" * 64 if wrong_hash else hashlib.sha256(archive.read_bytes()).hexdigest(),
                        },
                    }
                    output = root / label
                    with patch.object(builder, "download_font", return_value=archive):
                        name, success, message = builder.build_family(family, output)
                    self.assertEqual(name, "Libron")
                    self.assertEqual(success, succeeds, message)
                    files = list((output / "Libron").iterdir())
                    if succeeds:
                        self.assertEqual({f.name for f in files}, {"Libron_12.cpfont", "Libron_14.cpfont"})
                        self.assertTrue(all(f.read_bytes() == data for f in files))
                    else:
                        self.assertTrue(message)
                        self.assertEqual(files, [])
                    self.assertFalse((root / "escape.cpfont").exists())


if __name__ == "__main__":
    unittest.main()
