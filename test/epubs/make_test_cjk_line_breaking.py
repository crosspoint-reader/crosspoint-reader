#!/usr/bin/env python3
"""Build test_cjk_line_breaking.epub: one paragraph per CJK line-breaking case, for the rule checker
(test/epub_pipeline/CjkRulesTest.cpp) and the goldens.

  1. kinsoku       punctuation that must not start a line (。、」！？…… 々 ・), brackets that must not end
                   one (「『（), and small kana and ー, which may start one
  2. newlines      hard-wrapped source: a line break between two CJK characters vanishes, one between
                   CJK and Latin stays a space
  3. long run      a CJK run longer than 200 bytes and than the 96 tokens of a mid-block layout, with no punctuation to break it
  4. inline        CJK split across <b>, <i> and <span>: the seams are breaks without spaces
  5. mixed         Latin, digits, fullwidth and halfwidth forms next to CJK
  6. korean        Hangul words stay whole and keep their spaces
  7. table         CJK text in a table cell
  8. table budget  a cell of 289 CJK characters, one token each: charged per token (about 14 KB), the
                   row passes the 12 KB row budget and flows as text
                   (CjkTable.ALongCjkCellIsChargedPerToken). The row degrades before the run that
                   would cross the budget is added, so the cell's word lists never double to 512
                   entries (the heap baseline guards it)
  9. chunk seams   the SAX parser hands text over in 256-byte pieces that may cut a character in two:
                   a line break between CJK characters still vanishes when the character after it
                   (or the one before it) straddles a piece boundary
 10. quote seams   a line break between unspaced CJK and a quotation mark (“ ” ‘ ’ " ' ＂ ＇) on
                   either side vanishes too: in CJK text the quotes are CJK punctuation. Quotes on
                   both sides of the line break keep the space
                   (CjkQuoteSeams.ALineBreakNextToAQuoteInCjkTextVanishes)
 11. dense digits  600 codepoints of Han characters alternating with digits: every digit is a token
                   of its own, and the parser's 32-token run flush counts it, so the paragraph's
                   word lists keep their 128-entry steady state (the heap baseline guards it)

Each case paragraph opens with an indented first line (CSS text-indent), so the rule checker tells a
paragraph's first line from a line the layout broke.

Regenerate with:  python test/epubs/make_test_cjk_line_breaking.py
then refresh goldens: UPDATE_GOLDENS=1 ctest -R EpubPipeline
"""

import os
import zipfile

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "test_cjk_line_breaking.epub")

CSS = """body { margin: 0; padding: 0 }
p { margin: 0; text-indent: 1em }
h2 { margin: 1em 0 0 0; text-indent: 0; font-size: 1em }
td { padding: 0 }
"""

KINSOKU = (
    "「こんにちは。」と彼は言った。『本当に？』――私は答えた。（もちろん、そうです！）……そして、静かになった。"
    "人々は時々笑い、ラーメンを食べ、きゃっきゃっと騒いだ。ああ……！　彼女は「ええ、そうね」と言い、"
    "窓の外を見た。雨は止まない。「どうしよう？」――誰も答えない。"
)

# The Thousand Character Classic: a long run of Han characters with no punctuation at all.
LONG_RUN = (
    "天地玄黃宇宙洪荒日月盈昃辰宿列張寒來暑往秋收冬藏閏餘成歲律呂調陽雲騰致雨露結為霜金生麗水玉出崑岡"
    "劍號巨闕珠稱夜光果珍李柰菜重芥薑海鹹河淡鱗潛羽翔龍師火帝鳥官人皇始制文字乃服衣裳推位讓國有虞陶唐"
)

# Case 3 passes the 96-token mid-block layout by 12 characters.
LONG_RUN_PLUS = LONG_RUN + "舟車馬牛羊犬豕鹿鳥魚貝石"

# Case 9: each paragraph's text starts right after its <p>, so the 256-byte piece boundary falls
# where noted. 84 Han characters are 252 bytes; the first character after the line break then starts
# at byte 253 + spaces, and is cut after 1 or 2 of its 3 bytes.
SEAM_AFTER_1 = "漢" * 84 + "\n 字字字"
SEAM_AFTER_2 = "漢" * 84 + "\n  字字字"
# 25 words of 10 bytes and one of 4 make 254 bytes; the 字 that follows is cut after 2 bytes and is
# the last character before the line break.
SEAM_BEFORE = "abcdefghi " * 25 + "abc " + "字\n字字"

# Case 10, after Hong lou meng's hard-wrapped source: CJK then a closing quote, CJK then an ASCII
# quote, a closing quote then CJK, and CJK then an opening quote. The last paragraph is the control,
# with quotes on both sides of the line break.
QUOTE_SEAMS = (
    "<p>到那富貴鄉去安身樂業。\n”石頭听了，喜不能禁。</p>\n"
    '<p>那僧笑說：\n"通靈寶玉"之說也。</p>\n'
    "<p>富貴場中安居樂業。”\n兄听了。</p>\n"
    "<p>途中の漢字\n“引用”の文字。</p>\n"
    "<p>引用の“前半”\n“後半”も同じ。</p>"
)

# Case 11: 300 Han characters, each followed by a digit, in one paragraph of 600 codepoints.
DENSE_DIGITS = "".join(f"{c}{i % 10}" for i, c in enumerate((LONG_RUN * 4)[:300]))

CASES = [
    ("1. kinsoku", f"<p>{KINSOKU}</p>\n<p>{KINSOKU}</p>"),
    (
        "2. newlines",
        "<p>天下大勢，分久必合，\n合久必分。周末七國分爭，\n並入於秦。及秦滅之後，楚、漢分爭，\n"
        "又並入於漢。漢朝自高祖斬白蛇而起義，\n一統天下。</p>\n"
        "<p>漢字の文章の途中で\n改行があっても、\n空白にはならない。しかし漢字\nLatin の前では空白になる。\n"
        "「引用」の後も\n「次の引用」の前も同じ。</p>",
    ),
    ("3. long run", f"<p>{LONG_RUN_PLUS}</p>"),
    (
        "4. inline",
        "<p>漢字の<b>強調された文字</b>と<i>斜体の文字</i>、<span>そして</span>普通の文字が"
        "<b>一つの段落</b>の中で<i>混ざって</i>いる。</p>",
    ),
    (
        "5. mixed",
        "<p>漢字 iPhone 漢字と漢字iPhone漢字。2026年10月17日に、ＡＢＣ全角と半角のｱｲｳｴｵ｡を書いた。"
        "価格は1,234円（税込）です。</p>",
    ),
    (
        "6. korean",
        "<p>가나다 라마 소신(ab)이 12월부터 한국어<b>문장</b>입니다. 새침하게 흐린 품이 눈이 올 듯하더니 "
        "눈은 아니 오고 얼다가 만 비가 추적추적 내리었다.</p>",
    ),
    (
        "7. table",
        "<table><tr><td><p>表のセルの中にある長い文章も、同じ規則で改行される。句読点は行頭に来ない。</p></td>"
        "<td><p>短い</p></td></tr></table>",
    ),
    ("8. table budget", f"<table><tr><td><p>鑑{LONG_RUN * 3}</p></td></tr></table>"),
    ("9. chunk seams", f"<p>{SEAM_AFTER_1}</p>\n<p>{SEAM_AFTER_2}</p>\n<p>{SEAM_BEFORE}</p>"),
    ("10. quote seams", QUOTE_SEAMS),
    ("11. dense digits", f"<p>{DENSE_DIGITS}</p>"),
]

CHAPTER = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="ja" lang="ja">
<head>
  <title>CJK line breaking</title>
  <link rel="stylesheet" type="text/css" href="style.css"/>
</head>
<body>
{body}
</body>
</html>
"""


def main():
    body = "\n".join(f"<h2>{label}</h2>\n{content}" for label, content in CASES)
    opf = (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="bookid">\n'
        '  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">\n'
        "    <dc:title>CJK Line Breaking</dc:title>\n"
        "    <dc:creator>Test Corpus</dc:creator>\n"
        "    <dc:language>ja</dc:language>\n"
        '    <dc:identifier id="bookid">cjk-line-breaking</dc:identifier>\n'
        "  </metadata>\n"
        "  <manifest>\n"
        '    <item id="css" href="style.css" media-type="text/css"/>\n'
        '    <item id="chapter1" href="chapter1.xhtml" media-type="application/xhtml+xml"/>\n'
        "  </manifest>\n"
        '  <spine>\n    <itemref idref="chapter1"/>\n  </spine>\n'
        "</package>\n"
    )
    container = (
        '<?xml version="1.0"?>\n'
        '<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">\n'
        '  <rootfiles><rootfile full-path="OEBPS/content.opf" '
        'media-type="application/oebps-package+xml"/></rootfiles>\n'
        "</container>\n"
    )
    with zipfile.ZipFile(OUT, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("mimetype", "application/epub+zip", compress_type=zipfile.ZIP_STORED)
        z.writestr("META-INF/container.xml", container)
        z.writestr("OEBPS/content.opf", opf)
        z.writestr("OEBPS/style.css", CSS)
        z.writestr("OEBPS/chapter1.xhtml", CHAPTER.format(body=body))
    print("wrote", OUT)


if __name__ == "__main__":
    main()
