"""Exhaustive state-renaming checks; run with python3 -m unittest discover -s test/ot_shaper."""

import hashlib
import importlib.util
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("machines", ROOT / "scripts/gen_ot_shaper_machines.py")
machines = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(machines)

# Pin the emitted transition layout; update deliberately when the grammar changes.
TRANSITION_HASHES = {
    "INDIC": "41ec55c746ae2c68aea3bbceafada40867a8af6c1c448f67848e49607fd27de6",
    "USE": "b052f3139901c42b3058f793d39b7eee6ddc670b74a73fbc14b58d8340790333",
}


def header_array(text, name):
    body = re.search(rf"constexpr uint8_t {name}\[.*?\] = \{{(.*?)\}};", text, re.S).group(1)
    return [int(value) for value in re.findall(r"\d+", body)]


class MachineStateOrderTest(unittest.TestCase):
    def test_generated_scanners(self):
        header = (ROOT / "lib/OtShaper/OtSyllableMachines.h").read_text()
        for name, grammar, shift, extra in (
            ("INDIC", machines.indic_grammar, 13, (11,)),
            ("USE", machines.use_grammar, 23, (1, 18, 22)),
        ):
            with self.subTest(scanner=name):
                patterns, alphabet = grammar()
                alphabet = sorted((set(alphabet) & machines.used_categories(shift)) | set(extra))
                classes, nclasses, original, accept = machines.compile_scanner(patterns, alphabet)
                mapping, table, renamed_accept = machines.order_scanner_states(name, original, accept)
                self.assertEqual(mapping[0], 0)
                self.assertEqual(sorted(mapping), list(range(len(original))))
                self.assertEqual(len(table), len(original))
                self.assertEqual(len(renamed_accept), len(accept))
                self.assertLess(len(table), 255)
                for state, row in enumerate(original):
                    self.assertEqual(len(table[mapping[state]]), nclasses)
                    self.assertEqual(renamed_accept[mapping[state]], accept[state])
                    for cls, destination in enumerate(row):
                        expected = -1 if destination == -1 else mapping[destination]
                        self.assertEqual(table[mapping[state]][cls], expected, (state, cls))
                flat = [255 if value == -1 else value for row in table for value in row]
                self.assertEqual(header_array(header, name + "_TRANSITIONS"), flat)
                self.assertEqual(header_array(header, name + "_ACCEPT"),
                                 [255 if value is None else value for value in renamed_accept])
                self.assertEqual(header_array(header, name + "_CLASSES"),
                                 [classes.get(cat, 0) for cat in range(64)])
                self.assertEqual(hashlib.sha256(bytes(flat)).hexdigest(), TRANSITION_HASHES[name])
                self.assertEqual(machines.order_scanner_states(name, original, accept),
                                 (mapping, table, renamed_accept))


if __name__ == "__main__":
    unittest.main()
