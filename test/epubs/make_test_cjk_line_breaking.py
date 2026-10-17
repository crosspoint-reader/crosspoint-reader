#!/usr/bin/env python3
"""Build test_cjk_line_breaking.epub: one paragraph per CJK line-breaking case, for the rule checker
(test/epub_pipeline/CjkRulesTest.cpp) and the goldens.

  1. kinsoku       punctuation that must not start a line (。、」！？…… 々 ・), brackets that must not end
                   one (「『（), and small kana and ー, which may start one
  2. newlines      hard-wrapped source: a line break between two CJK characters vanishes, one between
                   CJK and Latin stays a space
  3. long run      a CJK run longer than 200 bytes and 96 tokens, with no punctuation to break it
  4. inline        CJK split across <b>, <i> and <span>: the seams are breaks without spaces
  5. mixed         Latin, digits, fullwidth and halfwidth forms next to CJK
  6. korean        Hangul words stay whole and keep their spaces
  7. table         CJK text in a table cell
  8. table budget  a cell of 385 CJK characters, one token each: charged per token, the row passes the
                   12 KB row budget and flows as text (CjkTable.ALongCjkCellIsChargedPerToken)

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

CASES = [
    ("1. kinsoku", f"<p>{KINSOKU}</p>\n<p>{KINSOKU}</p>"),
    (
        "2. newlines",
        "<p>天下大勢，分久必合，\n合久必分。周末七國分爭，\n並入於秦。及秦滅之後，楚、漢分爭，\n"
        "又並入於漢。漢朝自高祖斬白蛇而起義，\n一統天下。</p>\n"
        "<p>漢字の文章の途中で\n改行があっても、\n空白にはならない。しかし漢字\nLatin の前では空白になる。\n"
        "「引用」の後も\n「次の引用」の前も同じ。</p>",
    ),
    ("3. long run", f"<p>{LONG_RUN}</p>"),
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
