# Pocket Library for CrossPoint
# Copyright (C) 2026 Pocket Library contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This program is free software: you can redistribute it and/or modify it under
# the terms of the GNU General Public License as published by the Free Software
# Foundation, either version 3 of the License, or (at your option) any later
# version. See LICENSE-GPL-3.0 at the repository root.
"""python3 -m unittest test_booklist (from tools/books)."""

import csv
import os
import tempfile
import unittest
from unittest import mock

import booklist


class TitleCaseTest(unittest.TestCase):
    def check(self, before, after):
        self.assertEqual(booklist.tidy_title(before)[0], after)

    def test_lower_case_titles_are_capitalised(self):
        self.check("The demolished man", "The Demolished Man")
        self.check("A farewell to arms", "A Farewell to Arms")
        self.check("Tender is the night", "Tender Is the Night")
        self.check("Heart-shaped box", "Heart-Shaped Box")

    def test_small_words_go_lower_inside_a_title(self):
        self.check("The Letter Of Marque", "The Letter of Marque")
        self.check("On The Road", "On the Road")
        self.check("How To Win Friends and Influence People", "How to Win Friends and Influence People")

    def test_deliberate_capitals_are_kept(self):
        for title in ("UR", "H.M.S. Surprise", "V.", "Catch-22", "L.A. Confidential", "Less Than Zero",
                      "Born Standing Up: A Comic's Life", "Guards! Guards!"):
            self.check(title, title)

    def test_after_a_colon_or_an_alternative_title(self):
        self.check("2001: a space odyssey", "2001: A Space Odyssey")
        self.check("The origin of species, or, The preservation", "The Origin of Species, or, The Preservation")

    def test_a_novel_subtitles_go(self):
        self.check("Choke: a novel", "Choke")
        self.check("Bridget Jones's Diary : A Novel", "Bridget Jones's Diary")
        self.check("One Shot: A Reacher Novel", "One Shot")
        self.check("Haunted: a novel of stories", "Haunted: A Novel of Stories")
        self.check("Matterhorn: A Novel of the Vietnam War", "Matterhorn: A Novel of the Vietnam War")

    def test_series_prefix_is_split_off(self):
        self.assertEqual(booklist.tidy_title("Joe Pitt 1 - Already Dead"), ("Already Dead", "Joe Pitt", "1"))
        self.assertEqual(booklist.tidy_title("A Series of Unfortunate Events, The Bad Beginning"),
                         ("The Bad Beginning", "A Series of Unfortunate Events", ""))

    def test_authors(self):
        self.assertEqual(booklist.tidy_author("Cormac Mccarthy"), "Cormac McCarthy")
        self.assertEqual(booklist.tidy_author("Douglas Preston;Lincoln Child"), "Douglas Preston & Lincoln Child")
        self.assertEqual(booklist.tidy_author("j.d. salinger"), "J.D. Salinger")


class PruneTest(unittest.TestCase):
    def test_dry_run_moves_nothing_and_apply_moves_unkept_books(self):
        with tempfile.TemporaryDirectory() as tmp:
            keep = os.path.join(tmp, "keep.epub")
            drop = os.path.join(tmp, "drop.epub")
            for p in (keep, drop):
                with open(p, "w") as f:
                    f.write("x")
            sheet = os.path.join(tmp, "books.csv")
            with open(sheet, "w", newline="", encoding="utf-8-sig") as f:
                w = csv.DictWriter(f, fieldnames=["keep", "author", "title", "file"])
                w.writeheader()
                w.writerow({"keep": "x", "author": "A", "title": "Kept", "file": keep})
                w.writerow({"keep": "", "author": "B", "title": "Dropped", "file": drop})
            trash = os.path.join(tmp, "Trash")
            with mock.patch("os.path.expanduser", side_effect=lambda p: p.replace("~/.Trash", trash)), \
                    mock.patch("builtins.print"):
                booklist.cmd_prune(sheet, apply=False)
                self.assertTrue(os.path.exists(drop))
                booklist.cmd_prune(sheet, apply=True)
            self.assertTrue(os.path.exists(keep))
            self.assertFalse(os.path.exists(drop))
            self.assertTrue(os.path.exists(os.path.join(trash, "drop.epub")))


if __name__ == "__main__":
    unittest.main()
