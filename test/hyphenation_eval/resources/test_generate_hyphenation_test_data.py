import tempfile
import unittest
import zipfile
from pathlib import Path

from generate_hyphenation_test_data import EpubTextExtractor, extract_text_from_epub


class EpubTextExtractorTest(unittest.TestCase):
    def extract(self, markup):
        parser = EpubTextExtractor()
        parser.feed(markup)
        parser.close()
        return parser.text()

    def test_outer_close_recovers_from_unclosed_inner_tag(self):
        self.assertEqual(self.extract("<nav><div>hidden</nav><p>visible</p>"), "visible")

    def test_nested_same_name_remains_skipped_until_outer_close(self):
        markup = '<div class="pg-boilerplate"><div>hidden</div>also hidden</div>visible'
        self.assertEqual(self.extract(markup), "visible")

    def test_unmatched_close_does_not_end_exclusion(self):
        self.assertEqual(self.extract("<nav></aside>hidden</nav>visible"), "visible")

    def test_void_and_self_closing_tags_do_not_extend_exclusion(self):
        self.assertEqual(self.extract("<nav><br><span/>hidden</nav>visible"), "visible")

    def test_epub_flushes_buffered_final_text(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "fixture.epub"
            with zipfile.ZipFile(path, "w") as archive:
                archive.writestr("chapter.xhtml", "visible&unfinished")
            self.assertIn("unfinished", extract_text_from_epub(path))


if __name__ == "__main__":
    unittest.main()
