#!/usr/bin/env python3
"""
Generate a small EPUB with prose that exercises kerning and ligature edge cases.

Kerning pairs targeted:
  AV, AW, AY, AT, AC, AG, AO, AQ, AU
  FA, FO, Fe, Fo, Fr, Fy
  LT, LV, LW, LY
  PA, Pe, Po
  TA, Te, To, Tr, Ty, Tu, Ta, Tw
  VA, Ve, Vo, Vy, Va
  WA, We, Wo, Wa, Wy
  YA, Ya, Ye, Yo, Yu
  Av, Aw, Ay
  ov, oy, ow, ox
  rv, ry, rw
  "r." "r," (right-side space after r)
  f., f,

Ligature sequences targeted:
  fi, fl, ff, ffi, ffl, ft, fb, fh, fj, fk
  st, ct (historical)
  Th  (common Th ligature)

Also includes:
  Quotes around kerning-sensitive letters (e.g. "AWAY", "Typography")
  Numerals with kerning (10, 17, 74, 47)
  Punctuation adjacency (T., V., W., Y.)
"""

import os
import zipfile
import uuid
from datetime import datetime

BOOK_UUID = str(uuid.uuid4())
TITLE = "Kerning &amp; Ligature Edge Cases"
AUTHOR = "Crosspoint Test Fixtures"
DATE = datetime.now().strftime("%Y-%m-%d")

# ── XHTML content pages ──────────────────────────────────────────────

CHAPTER_1 = """\
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="en" lang="en">
<head><title>Chapter 1 – The Typographer's Affliction</title>
<link rel="stylesheet" type="text/css" href="style.css"/></head>
<body>
<h1>Chapter 1<br/>The Typographer&#x2019;s Affliction</h1>

<p>AVERY WATT always wanted to be a typographer. Years of careful study
at Yale had taught him that every typeface holds a secret: the negative
space between letters matters as much as the strokes themselves. &#x201C;AWAY
with sloppy kerning!&#x201D; he would thunder at his apprentices, waving a
proof sheet covered in red annotations.</p>

<p>The office of <i>Watt &amp; Yardley, Fine Typography</i> occupied the top
floor of an old factory on Waverly Avenue. On the frosted glass of the
door, gold leaf spelled WATT &amp; YARDLEY in Caslon capitals. Beneath it,
in smaller letters: <i>Purveyors of Tasteful Composition.</i></p>

<p>Today Avery sat at his desk, frowning at a page of proofs. The client
&#x2014; a wealthy patron named Lydia Thornton-Foxwell &#x2014; had commissioned
a lavish coffee-table volume on the history of calligraphy. It was the
sort of project Avery loved: difficult, fussy, and likely to be
appreciated by fewer than forty people on Earth.</p>

<p>&#x201C;Look at this,&#x201D; he muttered to his assistant, Vera Young. He tapped
the offending line with a pencil. &#x201C;The &#x2018;AW&#x2019; pair in DRAWN is too
loose. And the &#x2018;To&#x2019; in &#x2018;Towards&#x2019; &#x2014; the overhang of the T-crossbar
should tuck over the lowercase o. This is first-rate typeface work; we
can&#x2019;t afford sloppy fit.&#x201D;</p>

<p>Vera adjusted her glasses and peered at the proof. &#x201C;You&#x2019;re right. The
&#x2018;Ty&#x2019; in &#x2018;Typography&#x2019; also looks off. And further down &#x2014; see the
&#x2018;VA&#x2019; in &#x2018;VAULTED&#x2019;? The diagonals aren&#x2019;t meshing at all.&#x201D;</p>

<p>&#x201C;Exactly!&#x201D; Avery slapped the desk. &#x201C;We&#x2019;ll need to revisit every pair:
AV, AW, AT, AY, FA, Fe, LT, LV, LW, LY, PA, TA, Te, To, Tu, Tw, VA,
Ve, Vo, WA, Wa, YA, Ya &#x2014; the whole catalogue. I want this volume to be
flawless.&#x201D;</p>

<p>He leaned back and stared at the ceiling. Forty-seven years of
typesetting had left Avery with impeccable standards and a permanent
squint. He could spot a miskerned &#x2018;AT&#x2019; pair from across the room.
&#x201C;Fetch the reference sheets,&#x201D; he told Vera. &#x201C;And coffee. Strong
coffee.&#x201D;</p>
</body>
</html>
"""

CHAPTER_2 = """\
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="en" lang="en">
<head><title>Chapter 2 – Ligatures in the Afflicted Offices</title>
<link rel="stylesheet" type="text/css" href="style.css"/></head>
<body>
<h1>Chapter 2<br/>Ligatures in the Afflicted Offices</h1>

<p>The first difficulty arose with ligatures. Avery was fiercely attached
to the classic <i>fi</i> and <i>fl</i> ligatures &#x2014; the ones where the
terminal of the f swings gracefully into the dot of the i or the
ascender of the l. Without them, he felt, the page looked ragged and
unfinished.</p>

<p>&#x201C;A fine figure of a man,&#x201D; he read aloud from the proofs, testing the
fi combination. &#x201C;The daffodils in the field were in full flower, their
ruffled petals fluttering in the stiff breeze.&#x201D; He nodded &#x2014; the fi
and fl joins looked clean. But then he frowned. &#x201C;What about the
double-f ligatures? &#x2018;Affixed,&#x2019; &#x2018;baffled,&#x2019; &#x2018;scaffolding,&#x2019;
&#x2018;offload&#x2019; &#x2014; we need the ff, ffi, and ffl forms.&#x201D;</p>

<p>Vera flipped through the character map. &#x201C;The typeface supports ff, fi,
fl, ffi, and ffl. But I&#x2019;m not sure about the rarer ones &#x2014; ft, fb,
fh, fj, fk.&#x201D;</p>

<p>&#x201C;Test them,&#x201D; Avery said. &#x201C;Set a line: <i>The loft&#x2019;s rooftop offered a
deft, soft refuge.</i> That gives us ft. Now try: <i>halfback, offbeat.</i>
That&#x2019;s fb. For fh: <i>The wolfhound sniffed the foxhole.</i> And fj &#x2014;
well, that&#x2019;s mostly in loanwords. <i>Fjord</i> and <i>fjeld</i> are the
usual suspects. Fk is almost nonexistent in English; skip it.&#x201D;</p>

<p>Vera typed dutifully. &#x201C;What about the historical st and ct ligatures?
I know some revival faces include them.&#x201D;</p>

<p>&#x201C;Yes! The &#x2018;st&#x2019; ligature in words like <i>first, strongest, last,
masterful, fastidious</i> &#x2014; it gives the page a lovely archaic flavour.
And &#x2018;ct&#x2019; in <i>strictly, perfectly, tactful, connected, architectural,
instructed.</i> Mrs. Thornton-Foxwell specifically requested them.&#x201D;</p>

<p>He paused, then added: &#x201C;And don&#x2019;t forget the Th ligature. The word
&#x2018;The&#x2019; appears thousands of times in any book. If we can join the T and
the h into a graceful Th, the texture of every page improves. Set
<i>The thrush sat on the thatched roof of the theatre, thinking.</i>
There &#x2014; Th six times in one sentence.&#x201D;</p>
</body>
</html>
"""

CHAPTER_3 = """\
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="en" lang="en">
<head><title>Chapter 3 – The Proof of the Pudding</title>
<link rel="stylesheet" type="text/css" href="style.css"/></head>
<body>
<h1>Chapter 3<br/>The Proof of the Pudding</h1>

<p>Two weeks later, the revised proofs arrived. Avery carried them to the
window and held them up to the light. The paper was a beautiful warm
ivory, the ink a deep, true black.</p>

<p>He began to read, his eye scanning every pair. &#x201C;AWAY TO YESTERDAY&#x201D;
ran the chapter title, in large capitals. The AW was tight, the AY
tucked in, the TO well-fitted, the YE elegantly kerned. He exhaled
slowly.</p>

<p>&#x201C;Page fourteen,&#x201D; he murmured. &#x201C;<i>After years of toil, the faithful
craftsman affixed the final flourish to the magnificent oak
panel.</i>&#x201D; The fi in <i>faithful</i>, the ffi in <i>affixed</i>, the fi in
<i>final</i>, the fl in <i>flourish</i>, the fi in <i>magnificent</i> &#x2014; all were
perfectly joined. The ft in <i>craftsman</i> and <i>after</i> showed a subtle
but satisfying connection.</p>

<p>He turned to page seventeen. The text was denser here, a scholarly
passage on the evolution of letterforms. <i>Effective typographic
practice requires an officer&#x2019;s efficiency and a professor&#x2019;s
perfectionism. Suffice it to say that afflicted typesetters often find
themselves baffled by the sheer profusion of difficulties.</i></p>

<p>Avery counted: the passage contained <i>ff</i> four times, <i>fi</i> six
times, <i>ffl</i> once (in &#x201C;baffled&#x201D; &#x2014; wait, no, that was ff+l+ed), and
<i>ffi</i> twice (in &#x201C;officer&#x2019;s&#x201D; and &#x201C;efficiency&#x201D;). He smiled. The
ligatures were holding up perfectly.</p>

<p>The kerning was impeccable too. In the word &#x201C;ATAVISTIC&#x201D; &#x2014; set as a
pull-quote in small capitals &#x2014; the AT pair was snug, the AV nestled
tightly, and the TI showed just the right clearance. Lower down, a
passage about calligraphers in various countries offered a feast of
tricky pairs:</p>

<blockquote><p><i>Twelve Welsh calligraphers traveled to Avignon, where they
studied Venetian lettering techniques. Years later, they returned to
Pwllheli, Tywyn, and Aberystwyth, bringing with them a wealth of
knowledge about vowel placement, Tuscan ornament, and Lombardic
versals.</i></p></blockquote>

<p>The Tw in <i>Twelve</i>, the We in <i>Welsh</i>, the Av in <i>Avignon</i>, the Ve
in <i>Venetian</i>, the Ye in <i>Years</i>, the Ty in <i>Tywyn</i>, the Tu in
<i>Tuscan</i>, the Lo in <i>Lombardic</i> &#x2014; every pair sat comfortably on the
baseline, with not a hair&#x2019;s breadth of excess space.</p>
</body>
</html>
"""

CHAPTER_4 = """\
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="en" lang="en">
<head><title>Chapter 4 – Punctuation and Numerals</title>
<link rel="stylesheet" type="text/css" href="style.css"/></head>
<body>
<h1>Chapter 4<br/>Punctuation and Numerals</h1>

<p>&#x201C;Now for the tricky part,&#x201D; Avery said, reaching for a loupe. Kerning
around punctuation was notoriously fiddly. A period after a capital V
or W or Y could leave an ugly gap; a comma after an r or an f needed
careful attention.</p>

<p>He set a test passage: <i>Dr. Foxwell arrived at 7:47 a.m. on the 14th
of November. &#x201C;Truly,&#x201D; she declared, &#x201C;your work is perfect.&#x201D; &#x201C;We
try,&#x201D; Avery replied, &#x201C;but perfection is elusive.&#x201D;</i></p>

<p>The r-comma in &#x201C;your,&#x201D; the r-period in &#x201C;Dr.&#x201D; and &#x201C;Mr.&#x201D;, the
f-period in &#x201C;Prof.&#x201D; &#x2014; all needed to be set so that the punctuation
didn&#x2019;t drift too far from the preceding letter. Avery had seen
appalling examples where the period after a V seemed to float in space,
marooned from the word it belonged to.</p>

<p>&#x201C;V. S. Naipaul,&#x201D; he muttered, setting the name in various sizes.
&#x201C;W. B. Yeats. T. S. Eliot. P. G. Wodehouse. F. Scott Fitzgerald.
Y. Mishima.&#x201D; Each initial-period-space sequence was a potential trap.
At display sizes the gaps yawned; at text sizes they could vanish
into a murky blur.</p>

<p>Numerals brought their own challenges. The figures 1, 4, and 7 were
the worst offenders &#x2014; their open shapes created awkward spacing next to
rounder digits. &#x201C;Set these,&#x201D; Avery instructed: <i>10, 17, 47, 74, 114,
747, 1471.</i> Vera typed them in both tabular and proportional figures.
The tabular set looked even but wasteful; the proportional set was
compact but needed kerning between 7 and 4, and between 1 and 7.</p>

<p>&#x201C;And fractions,&#x201D; Avery added. &#x201C;Try &#xBD;, &#xBC;, &#xBE;, and the arbitrary
ones: 3/8, 5/16, 7/32. The virgule kerning against the numerals is
always a headache.&#x201D;</p>

<p>By five o&#x2019;clock they had tested every combination Avery could think
of. The proofs, now bristling with pencil marks and sticky notes, were
ready for the foundry. &#x201C;Tomorrow,&#x201D; Avery said, &#x201C;we tackle the italic
and the bold. And after that &#x2014; the small capitals.&#x201D;</p>

<p>Vera groaned. &#x201C;You&#x2019;re a perfectionist, Avery Watt.&#x201D;</p>

<p>&#x201C;Naturally,&#x201D; he replied. &#x201C;That&#x2019;s what they pay us for.&#x201D;</p>
</body>
</html>
"""

CHAPTER_5 = """\
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="en" lang="en">
<head><title>Chapter 5 – A Glossary of Troublesome Pairs</title>
<link rel="stylesheet" type="text/css" href="style.css"/></head>
<body>
<h1>Chapter 5<br/>A Glossary of Troublesome Pairs</h1>

<p>As a final flourish, Avery drafted an appendix for the volume: a
glossary of every kerning pair and ligature that had given him grief
over forty-seven years. Vera typed it up while Avery dictated.</p>

<h2>Kerning Pairs</h2>

<p><b>AV</b> &#x2014; As in AVID, AVIARY, AVOCADO, TRAVESTY, CAVALIER.<br/>
<b>AW</b> &#x2014; As in AWAY, AWARD, AWNING, DRAWN, BRAWL, SHAWL.<br/>
<b>AY</b> &#x2014; As in AYAH, LAYER, PLAYER, PRAYER, BAYONET.<br/>
<b>AT</b> &#x2014; As in ATLAS, ATTIC, LATERAL, WATER, PLATTER.<br/>
<b>AC</b> &#x2014; As in ACORN, ACCURATE, BACON, PLACATE.<br/>
<b>AG</b> &#x2014; As in AGAIN, AGATE, DRAGON, STAGGER.<br/>
<b>AO</b> &#x2014; As in KAOLIN, PHARAOH, EXTRAORDINARY.<br/>
<b>AQ</b> &#x2014; As in AQUA, AQUIFER, AQUILINE, OPAQUE.<br/>
<b>AU</b> &#x2014; As in AUTHOR, AUTUMN, HAUL, VAULT.<br/>
<b>FA</b> &#x2014; As in FACE, FACTOR, SOFA, AFFAIR.<br/>
<b>FO</b> &#x2014; As in FOLLOW, FORCE, COMFORT, BEFORE.<br/>
<b>Fe</b> &#x2014; As in February, feline, festival.<br/>
<b>Fo</b> &#x2014; As in Forsyth, forever, fortune.<br/>
<b>Fr</b> &#x2014; As in France, fragile, friction.<br/>
<b>Fy</b> &#x2014; As in Fyodor, fytte.<br/>
<b>LT</b> &#x2014; As in ALTITUDE, EXALT, RESULT, VAULT.<br/>
<b>LV</b> &#x2014; As in SILVER, SOLVE, INVOLVE, VALVE.<br/>
<b>LW</b> &#x2014; As in ALWAYS, RAILWAY, HALLWAY.<br/>
<b>LY</b> &#x2014; As in TRULY, ONLY, HOLY, UGLY.<br/>
<b>PA</b> &#x2014; As in PACE, PALACE, COMPANION, SEPARATE.<br/>
<b>TA</b> &#x2014; As in TABLE, TASTE, GUITAR, FATAL.<br/>
<b>Te</b> &#x2014; As in Ten, temple, tender.<br/>
<b>To</b> &#x2014; As in Tomorrow, together, towards.<br/>
<b>Tr</b> &#x2014; As in Travel, trouble, triumph.<br/>
<b>Tu</b> &#x2014; As in Tuesday, tulip, tumble.<br/>
<b>Tw</b> &#x2014; As in Twelve, twenty, twilight.<br/>
<b>Ty</b> &#x2014; As in Tyrant, typical, type.<br/>
<b>VA</b> &#x2014; As in VALUE, VAGUE, CANVAS, OVAL.<br/>
<b>Ve</b> &#x2014; As in Venice, verse, venture.<br/>
<b>Vo</b> &#x2014; As in Voice, volume, voyage.<br/>
<b>Wa</b> &#x2014; As in Water, watch, wander.<br/>
<b>We</b> &#x2014; As in Welcome, weather, welfare.<br/>
<b>Wo</b> &#x2014; As in Wonder, worry, worship.<br/>
<b>Ya</b> &#x2014; As in Yard, yacht, yawn.<br/>
<b>Ye</b> &#x2014; As in Yellow, yesterday, yeoman.<br/>
<b>Yo</b> &#x2014; As in Young, yoke, yoga.<br/>
<b>Yu</b> &#x2014; As in Yukon, Yugoslavia, yule.</p>

<h2>Ligatures</h2>

<p><b>fi</b> &#x2014; fifty, fiction, filter,efinite, affirm, magnify.<br/>
<b>fl</b> &#x2014; flag, flair, flame, floor, influence, reflect.<br/>
<b>ff</b> &#x2014; affair, affect, affirm, afford, buffalo, coffin, daffodil,
differ, effect, effort, offend, offer, office, scaffold, stiff,
suffocate, traffic, waffle.<br/>
<b>ffi</b> &#x2014; affidavit, affiliated, affirmative, baffling (wait &#x2014; that
is ffl!), coefficient, coffin, daffiness, diffident, efficient,
fficacy, muffin, officious, paraffin, sufficient, trafficking.<br/>
<b>ffl</b> &#x2014; affluent, baffled,ffle, offload, piffle, raffle, riffle,
ruffle, scaffold, scuffle, shuffle, sniffle, stiffly, truffle,
waffle.<br/>
<b>ft</b> &#x2014; after, craft, deft, drift, gift, left, loft, raft, shaft,
shift, soft, swift, theft, tuft, waft.<br/>
<b>fb</b> &#x2014; halfback, offbeat, surfboard.<br/>
<b>fh</b> &#x2014; wolfhound, cliffhanger, halfhearted.<br/>
<b>st</b> &#x2014; strong, first, last, must, fast, mist, ghost, roast, trust,
artist, honest, forest, harvest, modest.<br/>
<b>ct</b> &#x2014; act, fact, strict, direct, perfect, connect, collect,
distinct, instruct, architect, effect, exact, expect.<br/>
<b>Th</b> &#x2014; The, This, That, There, Their, They, Than, Though, Through,
Thought, Thousand, Thrive, Throne, Thatch.</p>

<p>&#x201C;There,&#x201D; Avery said, setting down his pencil. &#x201C;If a typesetter can
handle every word in that glossary without a single misfit, miskerned,
or malformed glyph, they deserve their weight in Garamond.&#x201D;</p>
</body>
</html>
"""

COVER_XHTML = """\
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="en" lang="en">
<head><title>Cover</title>
<style>
body { margin: 0; padding: 0; text-align: center; }
img { max-width: 100%; max-height: 100%; }
</style>
</head>
<body>
<img src="cover.jpg" alt="Kerning &amp; Ligature Edge Cases"/>
</body>
</html>
"""

STYLESHEET = """\
body {
  font-family: serif;
  margin: 2em;
  line-height: 1.6;
}
h1 {
  font-size: 1.5em;
  text-align: center;
  margin-bottom: 1.5em;
  line-height: 1.3;
}
h2 {
  font-size: 1.15em;
  margin-top: 1.5em;
  margin-bottom: 0.5em;
}
p {
  text-indent: 1.5em;
  margin: 0.25em 0;
  text-align: justify;
}
blockquote p {
  text-indent: 0;
  margin: 0.5em 1.5em;
  font-style: italic;
}
"""

CONTAINER_XML = """\
<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>
"""

CONTENT_OPF = f"""\
<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="BookId" version="3.0">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="BookId">urn:uuid:{BOOK_UUID}</dc:identifier>
    <dc:title>{TITLE}</dc:title>
    <dc:creator>{AUTHOR}</dc:creator>
    <dc:language>en</dc:language>
    <dc:date>{DATE}</dc:date>
    <meta property="dcterms:modified">{DATE}T00:00:00Z</meta>
    <meta name="cover" content="cover-image"/>
  </metadata>
  <manifest>
    <item id="cover-image" href="cover.jpg" media-type="image/jpeg" properties="cover-image"/>
    <item id="cover" href="cover.xhtml" media-type="application/xhtml+xml"/>
    <item id="style" href="style.css" media-type="text/css"/>
    <item id="ch1" href="chapter1.xhtml" media-type="application/xhtml+xml"/>
    <item id="ch2" href="chapter2.xhtml" media-type="application/xhtml+xml"/>
    <item id="ch3" href="chapter3.xhtml" media-type="application/xhtml+xml"/>
    <item id="ch4" href="chapter4.xhtml" media-type="application/xhtml+xml"/>
    <item id="ch5" href="chapter5.xhtml" media-type="application/xhtml+xml"/>
    <item id="toc" href="toc.xhtml" media-type="application/xhtml+xml" properties="nav"/>
  </manifest>
  <spine>
    <itemref idref="cover"/>
    <itemref idref="toc"/>
    <itemref idref="ch1"/>
    <itemref idref="ch2"/>
    <itemref idref="ch3"/>
    <itemref idref="ch4"/>
    <itemref idref="ch5"/>
  </spine>
</package>
"""

TOC_XHTML = """\
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops"
      xml:lang="en" lang="en">
<head><title>Table of Contents</title>
<link rel="stylesheet" type="text/css" href="style.css"/></head>
<body>
<h1>Kerning &amp; Ligature Edge Cases</h1>
<nav epub:type="toc">
  <ol>
    <li><a href="chapter1.xhtml">Chapter 1 &#x2013; The Typographer&#x2019;s Affliction</a></li>
    <li><a href="chapter2.xhtml">Chapter 2 &#x2013; Ligatures in the Afflicted Offices</a></li>
    <li><a href="chapter3.xhtml">Chapter 3 &#x2013; The Proof of the Pudding</a></li>
    <li><a href="chapter4.xhtml">Chapter 4 &#x2013; Punctuation and Numerals</a></li>
    <li><a href="chapter5.xhtml">Chapter 5 &#x2013; A Glossary of Troublesome Pairs</a></li>
  </ol>
</nav>
</body>
</html>
"""


def build_epub(output_path: str):
    script_dir = os.path.dirname(os.path.abspath(__file__))
    cover_path = os.path.join(script_dir, "cover.jpg")

    with zipfile.ZipFile(output_path, "w", zipfile.ZIP_DEFLATED) as zf:
        # mimetype must be first, stored (not deflated), no extra field
        zf.writestr("mimetype", "application/epub+zip", compress_type=zipfile.ZIP_STORED)
        zf.writestr("META-INF/container.xml", CONTAINER_XML)
        zf.writestr("OEBPS/content.opf", CONTENT_OPF)
        zf.writestr("OEBPS/toc.xhtml", TOC_XHTML)
        zf.writestr("OEBPS/style.css", STYLESHEET)
        zf.write(cover_path, "OEBPS/cover.jpg")
        zf.writestr("OEBPS/cover.xhtml", COVER_XHTML)
        zf.writestr("OEBPS/chapter1.xhtml", CHAPTER_1)
        zf.writestr("OEBPS/chapter2.xhtml", CHAPTER_2)
        zf.writestr("OEBPS/chapter3.xhtml", CHAPTER_3)
        zf.writestr("OEBPS/chapter4.xhtml", CHAPTER_4)
        zf.writestr("OEBPS/chapter5.xhtml", CHAPTER_5)
    print(f"EPUB written to {output_path}")


if __name__ == "__main__":
    script_dir = os.path.dirname(os.path.abspath(__file__))
    out = os.path.join(script_dir, "kerning_ligature_test.epub")
    build_epub(out)
