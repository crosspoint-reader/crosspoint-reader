#!/usr/bin/env python3
"""Build test_cjk_zh_punctuation.epub: a Simplified Chinese book (dc:language zh-Hans) for the punctuation
widths of the Simplified Chinese table (CjkRulesTest, spec section 5.2).

The sentences are written for this fixture. They put the marks the table adjusts in the places it adjusts them:
runs of marks (。” ！” ？” ）。), a colon before a quotation, the enumeration comma, book title marks, brackets,
the two-em dash, marks at line ends, digits and Latin inside Chinese text, and an ideographic space opening a
paragraph.

Regenerate with:  python test/epubs/make_test_cjk_zh_punctuation.py
then refresh goldens: UPDATE_GOLDENS=1 ctest -R EpubPipeline
"""

import os
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
NAME = "test_cjk_zh_punctuation.epub"
LANG = "zh-Hans"

CSS = """body { margin: 0; padding: 0 }
p { margin: 0; text-indent: 1em }
h1 { margin: 0 0 1em 0; text-indent: 0; font-size: 1.2em }
"""

PARAGRAPHS = [
    "今天早上下了一场小雨，街上的人不多。我带着伞出门，走到车站的时候，雨已经停了。"
    "车站旁边有一家小店，店主正在把桌子搬到门外：“天晴了，今天会有很多客人吧。”",
    "书架上放着三本书：《春天的花园》、《海边的小屋》和《星期六》。我最喜欢第二本——它讲的是"
    "一个渔夫和他的狗。每次读到最后一页，我都会想起小时候住过的那个村子。",
    "他问我：“你明天有空吗？”我说：“有空。”于是我们约好上午十点在图书馆门口见面"
    "（如果下雨，就改在旁边的咖啡店）。",
    "这一段用来测试连续的标点：“好！”“真的吗？”“当然。”——大家都笑了。"
    "（这里的括号后面紧跟着句号。）然后又是一句很长的话，让标点落在行尾。",
    "价格从12元涨到了15元，涨幅是25%。我们用Python写了一个小程序，统计了2020年到2024年的数据，"
    "结果和预想的差不多。",
    "　段首的全角空格保持全宽。这一段的文字也比较长，用来让逗号、顿号和句号出现在行尾，"
    "看看它们在行尾是不是只占半个字的宽度；分号也一样。",
]

CHAPTER = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="{lang}" lang="{lang}">
<head>
  <title>{title}</title>
  <link rel="stylesheet" type="text/css" href="style.css"/>
</head>
<body>
  <h1>{title}</h1>
{body}
</body>
</html>
"""


def main():
    title = "标点测试"
    body = "\n".join(f"  <p>{p}</p>" for p in PARAGRAPHS)
    chapter = CHAPTER.format(lang=LANG, title=title, body=body)
    opf = (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="bookid">\n'
        '  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">\n'
        f"    <dc:title>{title}</dc:title>\n"
        f"    <dc:language>{LANG}</dc:language>\n"
        f'    <dc:identifier id="bookid">{NAME}</dc:identifier>\n'
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
    out = os.path.join(HERE, NAME)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("mimetype", "application/epub+zip", compress_type=zipfile.ZIP_STORED)
        z.writestr("META-INF/container.xml", container)
        z.writestr("OEBPS/content.opf", opf)
        z.writestr("OEBPS/style.css", CSS)
        z.writestr("OEBPS/chapter1.xhtml", chapter)
    print("wrote", out)


if __name__ == "__main__":
    main()
