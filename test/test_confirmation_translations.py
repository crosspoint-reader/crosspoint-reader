"""Confirmation headings are translated questions, not concatenated button labels."""
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from gen_i18n import parse_yaml_file


class ConfirmationTranslationsTest(unittest.TestCase):
    def test_spanish_questions_include_both_marks(self):
        values = parse_yaml_file(str(ROOT / "lib/I18n/translations/spanish.yaml"))
        for key in ("STR_DELETE_ITEM_CONFIRM", "STR_SET_SLEEP_COVER_CONFIRM"):
            with self.subTest(key=key):
                self.assertTrue(values[key].startswith("¿"))
                self.assertTrue(values[key].endswith("?"))

    def test_all_locales_have_explicit_confirmation_headings(self):
        for path in (ROOT / "lib/I18n/translations").glob("*.yaml"):
            values = parse_yaml_file(str(path))
            with self.subTest(language=path.stem):
                for key in ("STR_DELETE_ITEM_CONFIRM", "STR_SET_SLEEP_COVER_CONFIRM"):
                    self.assertTrue(values[key])
                    self.assertEqual(values[key], values[key].strip())

    def test_callers_use_complete_question_keys(self):
        for filename, key in (
            ("src/activities/home/FileBrowserActivity.cpp", "STR_DELETE_ITEM_CONFIRM"),
            ("src/activities/library/LibraryListActivity.cpp", "STR_DELETE_ITEM_CONFIRM"),
            ("src/activities/util/BmpViewerActivity.cpp", "STR_SET_SLEEP_COVER_CONFIRM"),
        ):
            with self.subTest(filename=filename):
                text = (ROOT / filename).read_text()
                self.assertIn(f"tr({key})", text)
                self.assertNotIn('tr(STR_DELETE) + std::string("? ")', text)
                self.assertNotIn('tr(STR_SET_SLEEP_COVER) + std::string("?")', text)


if __name__ == "__main__":
    unittest.main()
