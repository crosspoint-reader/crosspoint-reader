# Pocket Library for CrossPoint
# Copyright (C) 2026 Pocket Library contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Card builder tests against a local stand-in for the Kiwix catalog and mirror.

Needs a built zimindex and openZIM's zim-testing-suite:
  ZIMINDEX=build/zim/zimindex ZIM_TEST_DATA_DIR=.../zim-testing-suite/data \
    python3 -m unittest tools/cardbuilder/test_cardbuilder.py -v
"""

import hashlib
import http.server
import io
import json
import os
import shutil
import sys
import tempfile
import threading
import unittest
from contextlib import redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import cardbuilder  # noqa: E402

DATA = os.environ.get("ZIM_TEST_DATA_DIR", "")
ZIMINDEX = os.environ.get("ZIMINDEX", "")
ZIM_NAME = "wikipedia_en_climate_change_mini_2024-06.zim"


class Mirror(http.server.ThreadingHTTPServer):
    """Serves /catalog/v2/entries and /zim/<file> (+ .sha256), with Range."""

    files: dict          # name -> bytes
    catalog_xml: bytes
    cut_after: int = 0   # first download of a file stops after this many bytes
    cut_done: set
    bad_checksum: bool = False


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        srv: Mirror = self.server
        path = self.path.split("?", 1)[0]
        if path == "/catalog/v2/entries":
            return self._send(200, srv.catalog_xml, "application/atom+xml")
        name = path.rsplit("/", 1)[-1]
        if name.endswith(".sha256"):
            body = srv.files.get(name[:-7])
            if body is None:
                return self._send(404, b"")
            digest = hashlib.sha256(body).hexdigest()
            if srv.bad_checksum:
                digest = "0" * 64
            return self._send(200, f"{digest}  {name[:-7]}\n".encode())
        body = srv.files.get(name)
        if body is None:
            return self._send(404, b"")
        start = 0
        rng = self.headers.get("Range")
        if rng:
            start = int(rng.split("=")[1].split("-")[0])
            if start >= len(body):
                return self._send(416, b"")
        chunk = body[start:]
        cut = srv.cut_after and name not in srv.cut_done
        self.send_response(206 if rng else 200)
        self.send_header("Content-Length", str(len(chunk)))
        self.end_headers()
        if cut:
            srv.cut_done.add(name)
            self.wfile.write(chunk[: srv.cut_after])
            self.wfile.flush()
            self.connection.shutdown(2)  # drop mid-transfer
            return
        self.wfile.write(chunk)

    def _send(self, code, body, ctype="application/octet-stream"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def catalog(base: str, entries: list) -> bytes:
    rows = []
    for name, flavour, fn, date, size in entries:
        rows.append(f"""
  <entry>
    <id>urn:uuid:{hashlib.md5(fn.encode()).hexdigest()}</id>
    <title>{name} title</title>
    <updated>{date}T00:00:00Z</updated>
    <summary>About {name}</summary>
    <language>eng</language>
    <name>{name}</name>
    <flavour>{flavour}</flavour>
    <category>wikipedia</category>
    <author><name>Wikipedia</name></author>
    <dc:issued>{date}T00:00:00Z</dc:issued>
    <link rel="http://opds-spec.org/acquisition/open-access" type="application/x-zim"
          href="{base}/zim/{fn}.meta4" length="{size}" />
  </entry>""")
    return ("""<?xml version="1.0" encoding="UTF-8"?>
<feed xmlns="http://www.w3.org/2005/Atom" xmlns:dc="http://purl.org/dc/terms/"
      xmlns:opds="https://specs.opds.io/opds-1.2">""" + "".join(rows) + "\n</feed>\n").encode()


@unittest.skipUnless(DATA and ZIMINDEX, "set ZIM_TEST_DATA_DIR and ZIMINDEX")
class CardBuilderTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="cardbuilder-"))
        self.staging = self.tmp / "staging"
        self.card = self.tmp / "card"
        self.staging.mkdir()
        self.card.mkdir()
        zim = Path(DATA, "nons", ZIM_NAME).read_bytes()
        small = Path(DATA, "nons", "small.zim").read_bytes()
        self.zim = zim
        self.srv = Mirror(("127.0.0.1", 0), Handler)
        self.srv.cut_done = set()
        base = f"http://127.0.0.1:{self.srv.server_address[1]}"
        self.base = base
        self.srv.files = {"climate_en_all_nopic_2024-06.zim": zim,
                          "climate_en_all_nopic_2023-01.zim": small,
                          "climate_en_all_maxi_2024-09.zim": small,
                          "tiny_en_all_2024-01.zim": small}
        self.srv.catalog_xml = catalog(base, [
            ("climate_en_all", "nopic", "climate_en_all_nopic_2023-01.zim", "2023-01-05", len(small)),
            ("climate_en_all", "nopic", "climate_en_all_nopic_2024-06.zim", "2024-06-10", len(zim)),
            ("climate_en_all", "maxi", "climate_en_all_maxi_2024-09.zim", "2024-09-01", len(small)),
            ("tiny_en_all", "", "tiny_en_all_2024-01.zim", "2024-01-01", len(small)),
        ])
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()
        self.lib = self.tmp / "library.toml"
        self.lib.write_text(f"""
[settings]
staging = "{self.staging}"
card = "{self.card}"

[[collection]]
key = "climate"
name = "climate_en_all"
flavour = ["nopic"]

[[collection]]
key = "tiny"
name = "tiny_en_all"
flavour = [""]

[[collection]]
key = "missing"
name = "nothing_here"
flavour = ["nopic"]
optional = true
""")

    def tearDown(self):
        self.srv.shutdown()
        self.srv.server_close()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_cb(self, *args):
        out = io.StringIO()
        with redirect_stdout(out):
            code = cardbuilder.main([args[0], "--library", str(self.lib), "--catalog",
                                     self.base + "/catalog/v2/entries", "--zimindex", ZIMINDEX,
                                     "--part-size", str(300 * 1024), *args[1:]])
        return code, out.getvalue()

    def test_catalog_picks_newest_edition_of_wanted_flavour(self):
        code, out = self.run_cb("plan")
        self.assertEqual(code, 0, out)
        self.assertIn("climate_en_all_nopic_2024-06.zim", out)
        self.assertNotIn("maxi", out)
        self.assertIn("skipped (optional) nothing_here", out)

    def test_full_build_splits_indexes_and_writes_manifest(self):
        code, out = self.run_cb("all")
        self.assertEqual(code, 0, out)
        folder = self.card / "library" / "climate"
        parts = sorted(p.name for p in folder.glob("*.zim??"))
        self.assertGreater(len(parts), 1, out)
        self.assertEqual(parts[0], "climate_en_all_nopic_2024-06.zimaa")
        joined = b"".join((folder / p).read_bytes() for p in parts)
        self.assertEqual(joined, self.zim)
        self.assertTrue((folder / "climate_en_all_nopic_2024-06.pltitles").exists())
        # Small enough to stay whole.
        self.assertTrue((self.card / "library" / "tiny" / "tiny_en_all_2024-01.zim").exists())

        man = json.loads((self.card / "library" / "manifest.json").read_text())
        self.assertEqual([c["key"] for c in man["collections"]], ["climate", "tiny"])
        c = man["collections"][0]
        self.assertEqual(c["date"], "2024-06-10")
        self.assertEqual(c["zim"]["sha256"], hashlib.sha256(self.zim).hexdigest())
        self.assertEqual(sum(p["size"] for p in c["zim"]["parts"]), len(self.zim))
        self.assertGreater(c["index"]["records"], 1000)
        self.assertEqual(c["zim"]["uuid"], self.zim[8:24].hex())

        # Second run: nothing to download, index or write.
        code, out = self.run_cb("all")
        self.assertEqual(code, 0, out)
        self.assertIn("already downloaded and checked", out)
        self.assertIn("already built", out)
        self.assertIn("0 file(s) to write", out)

    def test_download_resumes_after_a_dropped_connection(self):
        self.srv.cut_after = 100_000
        code, out = self.run_cb("download", "--only", "climate")
        self.assertEqual(code, 0, out)
        self.assertIn("resuming", out)
        self.assertEqual((self.staging / "climate_en_all_nopic_2024-06.zim").read_bytes(), self.zim)

    def test_checksum_mismatch_is_refused(self):
        self.srv.bad_checksum = True
        code, out = self.run_cb("download", "--only", "tiny")
        self.assertEqual(code, 1)
        self.assertIn("does not match its published checksum", out)
        self.assertFalse((self.staging / "tiny_en_all_2024-01.zim").exists())

    def test_dry_run_writes_nothing_and_prune_needs_asking(self):
        self.assertEqual(self.run_cb("download")[0], 0)
        self.assertEqual(self.run_cb("index")[0], 0)
        old = self.card / "library" / "climate" / "climate_en_all_nopic_2023-01.zim"
        old.parent.mkdir(parents=True)
        old.write_bytes(b"old edition")

        code, out = self.run_cb("copy", "--dry-run")
        self.assertEqual(code, 0, out)
        self.assertIn("Dry run", out)
        self.assertEqual([p.name for p in (self.card / "library").rglob("*") if p.is_file()], [old.name])

        code, out = self.run_cb("copy", "--verify")
        self.assertEqual(code, 0, out)
        self.assertTrue(old.exists(), "stale file removed without --prune")
        self.assertIn("--prune", out)

        code, out = self.run_cb("copy", "--prune")
        self.assertEqual(code, 0, out)
        self.assertFalse(old.exists())

    def test_copy_refuses_when_the_card_is_too_small(self):
        self.assertEqual(self.run_cb("download")[0], 0)
        self.assertEqual(self.run_cb("index")[0], 0)
        real = shutil.disk_usage
        cardbuilder.shutil.disk_usage = lambda p: real(p)._replace(free=1000)
        try:
            code, out = self.run_cb("copy")
        finally:
            cardbuilder.shutil.disk_usage = real
        self.assertEqual(code, 1)
        self.assertIn("not enough space", out)
        self.assertFalse((self.card / "library" / "manifest.json").exists())

    def test_copy_skips_what_is_not_downloaded_and_will_not_prune(self):
        self.assertEqual(self.run_cb("download", "--only", "tiny")[0], 0)
        code, out = self.run_cb("index")
        self.assertEqual(code, 0, out)
        self.assertIn("[climate] skipped", out)
        kept = self.card / "library" / "climate" / "climate_en_all_nopic_2024-06.zimaa"
        kept.parent.mkdir(parents=True)
        kept.write_bytes(b"copied earlier")

        code, out = self.run_cb("copy", "--prune")
        self.assertEqual(code, 0, out)
        self.assertIn("Not pruning", out)
        self.assertTrue(kept.exists(), "pruned a collection that was only skipped")
        self.assertTrue((self.card / "library" / "tiny" / "tiny_en_all_2024-01.zim").exists())

    def test_copy_before_download_explains_what_to_do(self):
        code, out = self.run_cb("copy")
        self.assertEqual(code, 1)
        self.assertIn("run download first", out)


class NoServerTest(unittest.TestCase):
    def test_part_names(self):
        self.assertEqual(cardbuilder.part_names("w", 3), ["w.zimaa", "w.zimab", "w.zimac"])
        self.assertEqual(cardbuilder.part_names("w", 28)[26:], ["w.zimba", "w.zimbb"])

    def test_bad_library_files(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d, "l.toml")
            p.write_text("[[collection]]\nkey='a'\nname='x'\n[[collection]]\nkey='a'\nname='y'\n")
            with self.assertRaises(cardbuilder.BuildError):
                cardbuilder.load_library(p)
            p.write_text("[settings]\n")
            with self.assertRaises(cardbuilder.BuildError):
                cardbuilder.load_library(p)

    def test_shipped_library_toml_parses(self):
        _, wants = cardbuilder.load_library(Path(cardbuilder.__file__).with_name("library.toml"))
        self.assertIn("wikipedia", [w.key for w in wants])


if __name__ == "__main__":
    unittest.main()
