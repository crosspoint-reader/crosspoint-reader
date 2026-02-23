#!/usr/bin/env python3
"""
Generate a small EPUB with prose that exercises kerning and ligature edge cases.

Kerning pairs targeted (Basic Latin — "western" scope, ASCII):
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

Kerning pairs targeted (Latin-1 Supplement — "western" scope, non-ASCII):
  Tö, Tü, Tä (German: Töchter, Türkei, Tänzer)
  Vö, Vä (German: Vögel, Väter)
  Wü, Wö (German: Würde, Wörter)
  Fü, Fé, Fê (German/French: Für, Février, Fête)
  Äu (German: Äußerst)
  Öf (German: Öffnung — also exercises ff ligature)
  Üb (German: Über)
  Àl, Àp (French: À la, À propos)
  Pè, Pé (French: Père, Pétanque)
  Ré (French: République, Rémy)
  Ño, Ñu (Spanish: niño, Muñoz)
  Eñ (Spanish: España)
  Ça, Çe (French: Ça, Garçon)
  Åk (Scandinavian: Åkesson)
  Ør (Scandinavian: Ørsted)
  Æs, Cæ (Scandinavian/archaic: Cæsar, æsthetic)
  ße, ßb (German: Straße, weißblau)
  «L, «V, r», é» (guillemets: « and »)
  „G, ‚W (German-style low-9 quotation marks)
  …" (horizontal ellipsis adjacent to quotes)

Kerning pairs targeted (Latin Extended-A — "latin" scope additions):
  Tě, Tř (Czech: Těšín, Třebíč)
  Vě (Czech: Věra, věda)
  Př (Czech: Příbram, příroda)
  Wą, Wę (Polish: Wąchock, Węgry)
  Łó, Łu, Ły (Polish: Łódź, Łukasz, łyżka)
  Čá, Če (Czech: Čáslav, České)
  Ří, Řa, Ře (Czech: Říjen, Řád, Řeka)
  Šk, Št (Czech/Slovak: Škoda, Šťastný)
  Ží, Žá (Czech: život, žádný)
  Ať (Czech)
  Tő, Vő (Hungarian: tőke, vőlegény)
  İs (Turkish: İstanbul)
  Ğa, Ğı (Turkish: dağ, Beyoğlu)

Ligature sequences targeted (ASCII):
  fi, fl, ff, ffi, ffl, ft, fb, fh, fj, fk
  st, ct (historical)
  Th  (common Th ligature)

Ligature sequences in Latin-1 Supplement context:
  fi adjacent to accented chars: définition, magnifique, officière
  fl adjacent to accented chars: réflexion, soufflé
  ff adjacent to accented chars: Öffnung, différent, souffrir
  ffi adjacent to accented chars: efficacité, officière
  ffl adjacent to accented chars: soufflé
  Æ/æ (U+00C6/U+00E6): Cæsar, Ærø, mediæval, encyclopædia, æsthetic

Ligature sequences in Latin Extended-A context:
  fi near Extended-A chars: filozofie, firma, finále, fikir
  fl near Extended-A chars: flétnista, flétna, refleks
  ff near Extended-A chars: offikás
  œ (U+0153): cœur, sœur, œuvre, bœuf, manœuvre
  ĳ (U+0133): ĳzer, vrĳ, bĳzonder, ĳverig

Kerning pairs targeted (Latin Extended-B — U+0180–024F):
  Ța, Țe, Țo, Țu (Romanian: T-comma overhang, like T)
  Șa, Șe, Și (Romanian: S-comma descender)
  Tș, Vș (Latin T/V followed by Romanian s-comma)
  Tơ, Vơ, Tư, Vư (Vietnamese: horn diacritics under T/V overhangs)
  Ƒa, Ƒo, Ƒe (African: F-hook pairs)
  DŽ, Dž, LJ, Lj, NJ, Nj (Croatian digraph ligatures)
  Tǎ, Tǒ, Tǔ (Pinyin: caron vowels under T overhang)
  Tǖ, Tǘ, Tǚ, Tǜ (Pinyin: u-diaeresis with tone marks)

Kerning pairs targeted (Greek & Coptic — U+0370–03FF):
  Γα, Γε, Γο, Γυ, Γρ (Γ overhang, like Latin T / Cyrillic Г)
  Τα, Τε, Το, Τυ, Τρ (Τ overhang, identical to Latin T)
  Αυ, Αν, Ατ, Αδ (Α diagonal, like Latin A)
  Υα, Υε, Υο (Υ diagonal, like Latin Y)
  Ρα, Ρε, Ρο (Ρ bowl, like Latin P)
  Φα, Φο, Φυ (Φ wide circular)
  Δα, Δε, Δο (Δ triangular base)
  Λα, Λε, Λο (Λ inverted-V)
  «Γ, «Τ, ε», ο» (guillemets in Greek context)

Kerning pairs targeted (Cyrillic — U+0400–04FF):
  Ге, Го, Гу, Га, Гр (Г has overhanging crossbar like T/F)
  Та, Те, То, Ту, Тр, Ті, Тя (Т = Latin T shape)
  Ра, Ре, Ро, Ру (Р = Latin P shape)
  Ау, Ав, Ат, Ад (А = Latin A shape)
  Ув, Уд, Ук, Ум (У = Latin Y shape — diagonal)
  Фа, Фо, Фу (Ф = wide circular letter)
  Да, Де, До, Ду (Д has descending serifs)
  Ла, Ле, Ло, Лу (Л = inverted V shape)
  Ча, Чо, Чу (Ч has overhanging stroke)
  «Г, «Т, «В, р», е» (guillemets in Cyrillic context)
  Ukrainian: Її, Єв, Ґа
  Bulgarian: Щу, Жа, Юл

Combining marks targeted (U+0300–U+036F — Combining Diacritical Marks):
  U+0300 grave, U+0301 acute, U+0302 circumflex, U+0303 tilde
  U+0304 macron, U+0306 breve, U+0307 dot above, U+0308 diaeresis
  U+030A ring above, U+030B double acute, U+030C caron
  U+0323 dot below (Vietnamese stacking)
  U+0327 cedilla, U+0328 ogonek
  U+031B horn (Vietnamese)

  Decomposed equivalents of precomposed characters (NFD vs NFC):
    o+U+0308 vs ö, e+U+0301 vs é, e+U+0302 vs ê, a+U+0300 vs à, etc.
  Multiple combining marks on one base character:
    e+U+0302+U+0323 (Vietnamese ệ), u+U+031B+U+0301, etc.
  Combining marks adjacent to kerning pairs:
    To+U+0308 (decomposed Tö), Vo+U+0308, Wu+U+0308, etc.
  Combining marks adjacent to ligature sequences:
    de+U+0301+fi (définition), re+U+0301+fl (réflexion), etc.
  Extended Latin-A decomposed compositions:
    e+U+030C (ě), r+U+030C (ř), a+U+0328 (ą), s+U+0327 (ş), D+U+030C (Ď), etc.
  Precomposed vs decomposed side-by-side comparison (Latin-1 and Extended-A)

Also includes:
  Quotes around kerning-sensitive letters (e.g. "AWAY", "Typography")
  Numerals with kerning (10, 17, 74, 47)
  Punctuation adjacency (T., V., W., Y.)
"""

import io
import os
import zipfile
import uuid
from datetime import datetime

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    print("Please install Pillow: pip install Pillow")
    exit(1)


_PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_BOOKERLY_FONT = os.path.join(
    _PROJECT_ROOT, "lib", "EpdFont", "builtinFonts", "source",
    "Bookerly", "Bookerly-Regular.ttf",
)


def _get_font(size=20):
    """Get the Bookerly font at the requested size, with system fallbacks."""
    paths = [_BOOKERLY_FONT]
    for path in paths:
        try:
            return ImageFont.truetype(path, size)
        except (OSError, IOError):
            continue
    return ImageFont.load_default(size)


def _draw_text_centered(draw, y, text, font, fill, width):
    bbox = draw.textbbox((0, 0), text, font=font)
    text_width = bbox[2] - bbox[0]
    x = (width - text_width) // 2
    draw.text((x, y), text, font=font, fill=fill)


def create_cover_image():
    """Generate a cover image matching the original layout and return JPEG bytes."""
    width, height = 536, 800
    bg_color = (30, 42, 58)
    text_color = (225, 220, 205)

    img = Image.new("RGB", (width, height), bg_color)
    draw = ImageDraw.Draw(img)

    font_title = _get_font(72)
    font_subtitle = _get_font(26)
    font_author = _get_font(14)
    font_ornament = _get_font(64)

    title_lines = ["Kerning", "& Ligature", "Edge Cases"]
    title_y = 92
    for line in title_lines:
        _draw_text_centered(draw, title_y, line, font_title, text_color, width)
        title_y += 90

    ornament_y = title_y + 10
    _draw_text_centered(draw, ornament_y, "*", font_ornament, text_color, width)

    subtitle_y = ornament_y + 72
    _draw_text_centered(draw, subtitle_y, "A Typographer\u2019s Compendium",
                        font_subtitle, text_color, width)

    _draw_text_centered(draw, height - 70, "CROSSPOINT TEST FIXTURES",
                        font_author, text_color, width)

    buf = io.BytesIO()
    img.save(buf, "JPEG", quality=90)
    return buf.getvalue()

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

CHAPTER_6 = """\
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="en" lang="en">
<head><title>Chapter 6 &#x2013; Western European Accents</title>
<link rel="stylesheet" type="text/css" href="style.css"/></head>
<body>
<h1>Chapter 6<br/>Western European Accents</h1>

<p>Before the calligraphy volume was even bound, Mrs. Thornton-Foxwell
rang with a revision. Half the captions were in French and German, the
bibliography included Scandinavian and Spanish sources, and the whole
thing needed to work in those languages too. &#x201C;The accented characters,&#x201D;
she said. &#x201C;They must be perfect.&#x201D;</p>

<p>Avery sighed. The Latin-1 Supplement block &#x2014; the accented vowels,
cedillas, tildes, and special letters of Western European typography
&#x2014; would double his kerning workload. Every pair he had already
perfected for plain ASCII now had accented variants.</p>

<h2>German Pairs</h2>

<p>German was the first test. Avery set a paragraph: <i>T&#xF6;chter sa&#xDF;en
&#xFC;ber den B&#xFC;chern. V&#xF6;gel flogen &#xFC;ber die W&#xE4;lder. Die W&#xFC;rde
des Menschen ist unantastbar. T&#xE4;nzer &#xFC;bten in der T&#xFC;rkei.</i>
The T&#xF6; in &#x201C;T&#xF6;chter&#x201D; was telling &#x2014; the umlaut dots on the
&#xF6; sat precisely where the crossbar of the T wanted to extend.
V&#xF6; in &#x201C;V&#xF6;gel&#x201D; had a similar conflict: the V&#x2019;s diagonal
met the &#xF6; at an angle that the umlaut dots complicated. W&#xFC; in
&#x201C;W&#xFC;rde&#x201D; and W&#xF6; in &#x201C;W&#xF6;rter&#x201D; each demanded individual
adjustment. T&#xFC; in &#x201C;T&#xFC;rkei&#x201D; and T&#xE4; in &#x201C;T&#xE4;nzer&#x201D;
added two more accented vowels to the T&#x2019;s already long list of
right-side partners.</p>

<p>&#x201C;And don&#x2019;t forget &#xD6;ffnung,&#x201D; Avery said. &#x201C;The &#xD6;f pair is
tricky enough, but &#x2018;&#xD6;ffnung&#x2019; also contains an ff ligature right
after the umlaut. A double test.&#x201D; He set more examples: <i>&#xC4;u&#xDF;erst
sorgf&#xE4;ltig pr&#xFC;fte er die Gr&#xF6;&#xDF;e der Stra&#xDF;e. F&#xFC;r die
Gr&#xFC;&#xDF;e seiner F&#xFC;&#xDF;e brauchte er Ma&#xDF;band.</i> The &#xC4;u
in &#x201C;&#xC4;u&#xDF;erst,&#x201D; the F&#xFC; in &#x201C;F&#xFC;r,&#x201D; the Gr&#xFC; in
&#x201C;Gr&#xFC;&#xDF;e&#x201D; &#x2014; every pairing of accented vowels against
consonants needed attention. The &#xDF; (eszett) in &#x201C;Stra&#xDF;e,&#x201D;
&#x201C;Gr&#xFC;&#xDF;e,&#x201D; and &#x201C;F&#xFC;&#xDF;e&#x201D; had its own right-side bearing
issues: &#xDF;e and &#xDF;b in &#x201C;wei&#xDF;blau&#x201D; required careful attention,
as the eszett&#x2019;s unusual tail affected spacing against the
following letter. &#xDC;b in &#x201C;&#xDC;ber&#x201D; and &#x201C;&#xDC;bung&#x201D; placed
an umlaut directly over the narrow U, which could collide with
ascenders in the line above.</p>

<p>German punctuation style added another layer of complexity.
&#x201E;Guten Tag,&#x201C; sagte er. &#x201A;Warum nicht?&#x2018; The low opening
quotes &#x2014; &#x201E; (U+201E) and &#x201A; (U+201A) &#x2014; sat on the baseline
rather than hanging near the cap height, changing the spacing dynamics
against the following capital letter. The &#x201E;G pair, the
&#x201A;W pair &#x2014; these were entirely different animals from their
English-style &#x201C;G and &#x2018;W counterparts.</p>

<h2>French Pairs</h2>

<p>French was rich in accented characters. <i>F&#xEA;te de la R&#xE9;publique.
P&#xE8;re No&#xEB;l arriva en F&#xE9;vrier. &#xC0; la recherche du
caf&#xE9; id&#xE9;al. &#xC0; propos de rien.</i> The F&#xEA; in
&#x201C;F&#xEA;te,&#x201D; the P&#xE8; in &#x201C;P&#xE8;re,&#x201D; the F&#xE9; in
&#x201C;F&#xE9;vrier,&#x201D; the &#xC0;l in &#x201C;&#xC0; la,&#x201D; the &#xC0;p in
&#x201C;&#xC0; propos&#x201D; &#x2014; each involved a diacritical mark that could
interfere with kerning. The R&#xE9; in &#x201C;R&#xE9;publique&#x201D; needed the
accent on the &#xC9; to clear the shoulder of the R.</p>

<p>French also offered excellent ligature-with-accent test cases:
<i>La d&#xE9;finition de l&#x2019;efficacit&#xE9; r&#xE9;side dans la
r&#xE9;flexion. L&#x2019;offici&#xE8;re v&#xE9;rifia les diff&#xE9;rentes
souffl&#xE9;s. Il souffrit magnifiquement.</i> The fi in
&#x201C;d&#xE9;finition&#x201D; and &#x201C;magnifiquement,&#x201D; the ffi in
&#x201C;efficacit&#xE9;&#x201D; and &#x201C;offici&#xE8;re,&#x201D; the fl in
&#x201C;r&#xE9;flexion,&#x201D; the ff in &#x201C;diff&#xE9;rentes&#x201D; and
&#x201C;souffrir,&#x201D; the ffl in &#x201C;souffl&#xE9;s&#x201D; &#x2014; all occurred in
words where accented characters sat adjacent to the ligature sequence.
This was precisely the sort of combination that exposed rendering
bugs.</p>

<p>Then there was &#xC7;a. &#x201C;The cedilla on the &#xC7;,&#x201D; Avery explained,
&#x201C;descends below the baseline just like a comma. &#xC7;a and &#xC7;e are
pairs we must not ignore.&#x201D; He added: <i>&#xC7;a va? Gar&#xE7;on, un
caf&#xE9; cr&#xE8;me, s&#x2019;il vous pla&#xEE;t.</i></p>

<p>French typography also used guillemets instead of quotation marks.
&#xAB;&#x202F;Venez ici,&#x202F;&#xBB; dit-elle. &#xAB;&#x202F;Regardez la
beaut&#xE9; de ces lettres.&#x202F;&#xBB; The kerning between &#xAB; and the
following letter (&#xAB;V, &#xAB;R, &#xAB;L), and between the preceding
letter and &#xBB; (r&#xBB;, &#xE9;&#xBB;, s&#xBB;), required their own
adjustments &#x2014; the angular shapes of the guillemets created different
spacing needs from curly quotation marks.</p>

<h2>Spanish and Portuguese</h2>

<p>Spanish contributed the tilde-N. <i>El ni&#xF1;o so&#xF1;&#xF3; con el
a&#xF1;o nuevo en Espa&#xF1;a. Se&#xF1;or Mu&#xF1;oz ense&#xF1;aba con
cari&#xF1;o.</i> The &#xD1;o in &#x201C;ni&#xF1;o&#x201D; and &#x201C;a&#xF1;o,&#x201D; the
&#xD1;u in &#x201C;Mu&#xF1;oz,&#x201D; the E&#xF1; in &#x201C;Espa&#xF1;a&#x201D; &#x2014; the
tilde sat high, potentially colliding with ascenders in the line above
and altering the perceived spacing of the pair. ESPA&#xD1;A and A&#xD1;O
in capitals were particularly demanding: the &#xD1;&#x2019;s tilde could
feel disconnected from the diagonal strokes of a flanking A.</p>

<p>Portuguese added its own accents: <i>A tradi&#xE7;&#xE3;o da na&#xE7;&#xE3;o
&#xE9; a educa&#xE7;&#xE3;o. Tr&#xEA;s irm&#xE3;os viviam em S&#xE3;o Paulo.</i>
The &#xE3;o sequence in &#x201C;tradi&#xE7;&#xE3;o&#x201D; and &#x201C;na&#xE7;&#xE3;o,&#x201D;
the &#xE3;os in &#x201C;irm&#xE3;os,&#x201D; the &#xEA;s in &#x201C;Tr&#xEA;s&#x201D; &#x2014; all
involved characters with tildes or circumflexes that changed vertical
clearance.</p>

<h2>Scandinavian and the &#xC6; Ligature</h2>

<p>The Scandinavian languages brought &#xC5;, &#xD8;, and the &#xC6; ligature
into play. <i>&#xC5;kesson reste till &#xD8;rsted via &#xC6;r&#xF8;.
Medi&#xE6;val &#xE6;sthetics influenced Encyclop&#xE6;dia entries about
C&#xE6;sar.</i></p>

<p>The &#xC5;k in &#x201C;&#xC5;kesson&#x201D; placed a ring-above diacritical directly
over the A&#x2019;s apex &#x2014; a collision risk with the line above. &#xD8;r in
&#x201C;&#xD8;rsted&#x201D; combined the O-stroke with a tight r pairing. And
&#xC6; (U+00C6) was itself a ligature glyph: the visual fusion of A and E
into a single character. Kerning &#xC6; against its neighbors &#x2014;
&#xC6;r, &#xC6;s, C&#xE6;, medi&#xE6; &#x2014; required treating it as a wide glyph
with unique sidebearings.</p>

<h2>Typographic Punctuation</h2>

<p>Vera looked up from her notes. &#x201C;Should I add the en dash and ellipsis
tests? We&#x2019;ve been using em dashes everywhere, but en dashes kern
differently.&#x201D;</p>

<p>&#x201C;Yes,&#x201D; Avery said. &#x201C;Set: <i>pages 47&#x2013;74, the years
1910&#x2013;1947.</i> The en dash sits higher than a hyphen and is narrower
than an em dash, so it creates different spacing against the flanking
digits.&#x201D;</p>

<p>&#x201C;And for the ellipsis: <i>The answer was&#x2026; not what he expected.
&#x2018;Well&#x2026;&#x2019; she trailed off. &#x201C;Vraiment&#x2026;&#x201D;
murmured the Frenchman.</i> The horizontal ellipsis &#x2014; a single glyph
at U+2026, not three periods &#x2014; needs its own kerning against adjacent
quotation marks, letters, and spaces. The pair &#x2026;&#x201D; and
&#x2026;&#x2019; are especially important: the ellipsis must not crash
into the closing quote.&#x201D;</p>
</body>
</html>
"""

CHAPTER_7 = """\
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="en" lang="en">
<head><title>Chapter 7 &#x2013; Beyond the Western Alphabet</title>
<link rel="stylesheet" type="text/css" href="style.css"/></head>
<body>
<h1>Chapter 7<br/>Beyond the Western Alphabet</h1>

<p>Just when Avery thought the project was finished, Lydia Thornton-Foxwell
rang with a new request. She wanted a companion volume &#x2014; a survey of
calligraphic traditions across Central and Eastern Europe, with chapters
on Polish, Czech, Hungarian, and Turkish lettering. &#x201C;The same standard
of kerning,&#x201D; she insisted. &#x201C;Every pair, every ligature.&#x201D;</p>

<p>Avery groaned. The Latin Extended characters &#x2014; the haceks, ogoneks,
acutes, and cedillas of Slavic and Turkic alphabets &#x2014; would multiply
his kerning tables enormously. But he was a professional. He reached
for his reference books and began.</p>

<h2>Czech Pairs</h2>

<p>The Czech language was a minefield of diacritics. Avery set a test
paragraph: <i>T&#x11B;&#x161;&#xED;n le&#x17E;&#xED; nedaleko T&#x159;eb&#xED;&#x10D;e. P&#x159;&#xED;bram a P&#x159;erov
jsou m&#x11B;sta, kde se V&#x11B;ra u&#x10D;ila v&#x11B;d&#x11B;. &#x10C;&#xE1;slav le&#x17E;&#xED;
na jih od &#x10C;esk&#xE9;ho Brodu.</i> He examined the
T&#x11B; pair in &#x201C;T&#x11B;&#x161;&#xED;n&#x201D; &#x2014; the crossbar of the T needed to tuck
over the &#x11B; just as it would over a plain e. The T&#x159; in
&#x201C;T&#x159;eb&#xED;&#x10D;e&#x201D; was trickier; the caron on the &#x159; changed its
vertical profile.</p>

<p>&#x201C;And look at these,&#x201D; he said to Vera. &#x201C;P&#x159; in &#x2018;P&#x159;&#xED;bram&#x2019;
and &#x2018;P&#x159;erov&#x2019; &#x2014; the overhang of the P&#x2019;s bowl over the &#x159;
is critical. V&#x11B; in &#x2018;V&#x11B;ra&#x2019; and &#x2018;v&#x11B;d&#x11B;&#x2019; &#x2014; the
diagonal of the V must relate correctly to the caron.&#x201D;</p>

<p>He continued with more Czech pairs: <i>&#x158;&#xED;jen je kr&#xE1;sn&#xFD; m&#x11B;s&#xED;c.
&#x158;eka te&#x10D;e p&#x159;es &#x158;ad obchodn&#xED;ch dom&#x16F;. &#x160;koda vyr&#xE1;b&#xED;
automobily. &#x160;&#x165;astn&#xFD; den! &#x17D;ivot nen&#xED; &#x17E;&#xE1;dn&#xE1; procházka.</i>
The &#x158;&#xED; in &#x201C;&#x158;&#xED;jen,&#x201D; the &#x158;e in &#x201C;&#x158;eka,&#x201D; the &#x160;k in
&#x201C;&#x160;koda,&#x201D; the &#x160;&#x165; in &#x201C;&#x160;&#x165;astn&#xFD;,&#x201D; the &#x17D;i in
&#x201C;&#x17D;ivot,&#x201D; the &#x17E;&#xE1; in &#x201C;&#x17E;&#xE1;dn&#xE1;&#x201D; &#x2014; each demanded
individual attention. A&#x165; he added to the list: the Czech word
&#x201C;a&#x165;&#x201D; was tiny but the kerning between A and &#x165; mattered in
display settings.</p>

<h2>Polish Pairs</h2>

<p>Polish was equally demanding. <i>W&#x105;chock to ma&#x142;e miasteczko.
W&#x119;gry s&#x105;siaduj&#x105; z Polsk&#x105;. &#x141;&#xF3;d&#x17A; jest trzecim co do
wielko&#x15B;ci miastem. &#x141;ukasz mieszka w &#x141;ucku. &#x141;y&#x17C;ka
le&#x17C;y na stole.</i></p>

<p>The W&#x105; in &#x201C;W&#x105;chock&#x201D; was crucial &#x2014; the ogonek on the
&#x105; dangled below the baseline, and the W&#x2019;s diagonal had to
account for it. Similarly, W&#x119; in &#x201C;W&#x119;gry&#x201D; needed the same
care. The &#x141; with its stroke was a special case: &#x141;&#xF3; in
&#x201C;&#x141;&#xF3;d&#x17A;,&#x201D; &#x141;u in &#x201C;&#x141;ukasz&#x201D; and &#x201C;&#x141;uck,&#x201D; &#x141;y in
&#x201C;&#x141;y&#x17C;ka&#x201D; &#x2014; the horizontal bar through the L altered every
right-side pairing.</p>

<h2>Hungarian and Turkish Pairs</h2>

<p>Hungarian brought the double-acute characters. <i>A t&#x151;ke
n&#xF6;vekedett. A v&#x151;leg&#xE9;ny meg&#xE9;rkezett. F&#x171;z&#x151;
k&#xE9;sz&#xED;tette az &#xE9;telt.</i> The T&#x151; in &#x201C;t&#x151;ke&#x201D;
and V&#x151; in &#x201C;v&#x151;leg&#xE9;ny&#x201D; were new territory &#x2014; the double
acute over the &#x151; added height that could collide with ascenders
in the line above.</p>

<p>Turkish was another story entirely. <i>&#x130;stanbul&#x2019;da ya&#x15F;&#x131;yoruz.
Beyo&#x11F;lu g&#xFC;zel bir semt. Da&#x11F;dan inen yol
&#x15E;i&#x15F;li&#x2019;ye ula&#x15F;&#x131;r.</i> The &#x130;s in &#x201C;&#x130;stanbul&#x201D;
was distinctive &#x2014; the dotted capital I (&#x130;) sat differently from a
standard I. &#x11E;a and &#x11E;&#x131; pairs appeared in words like
&#x201C;da&#x11F;&#x201D; (mountain), where the breve on the &#x11E; changed the
letter&#x2019;s visual weight. The &#x15E;i in &#x201C;&#x15E;i&#x15F;li&#x201D;
required the cedilla of the &#x15E; to clear the descending stroke
gracefully.</p>

<h2>Ligatures Across Extended Latin</h2>

<p>Ligature handling grew more complex with extended characters. Avery
tested sequences where fi and fl appeared near or adjacent to
diacritical marks: <i>Filozofie vy&#x17E;aduje p&#x159;esn&#xE9;
my&#x161;len&#xED;. Firma z T&#x159;eb&#xED;&#x10D;e exportuje fin&#xE1;le
do cel&#xE9;ho sv&#x11B;ta. Fl&#xE9;tnista hr&#xE1;l na
fl&#xE9;tnu.</i></p>

<p>The fi in &#x201C;Filozofie,&#x201D; &#x201C;Firma,&#x201D; and &#x201C;fin&#xE1;le&#x201D;
all needed proper ligature joining even when surrounded by Extended-A
characters. The fl in &#x201C;Fl&#xE9;tnista&#x201D; and &#x201C;fl&#xE9;tnu&#x201D;
similarly demanded clean joins. Polish offered its own test cases:
<i>Refleks jest szybki. Oficjalny dokument le&#x17C;y na biurku.
Afirmacja jest wa&#x17C;na w filozofii.</i> The fl in
&#x201C;Refleks,&#x201D; the fi in &#x201C;Oficjalny&#x201D; and &#x201C;filozofii,&#x201D;
the ffi in &#x201C;Afirmacja&#x201D; &#x2014; all exercised the ligature engine in
a Latin Extended-A context.</p>

<p>Turkish added another dimension: <i>Fikir &#xF6;zg&#xFC;rl&#xFC;&#x11F;&#xFC;n
temelidir. Fi&#x15F;ek havaya f&#x131;rlat&#x131;ld&#x131;.</i> The fi in
&#x201C;Fikir&#x201D; and &#x201C;Fi&#x15F;ek&#x201D; tested whether the ligature engine
correctly handled the Turkish dotless-&#x131; (&#x131;) and
dotted-&#x130; (&#x130;) distinction.</p>

<h2>French &#x152; and Dutch ĳ</h2>

<p>Two Latin Extended-A characters were themselves ligatures by heritage.
The French &#x153; (o-e ligature) appeared in: <i>Le c&#x153;ur de l&#x2019;&#x153;uvre
bat au rythme des s&#x153;urs. Le b&#x153;uf traverse la man&#x153;uvre
avec aplomb.</i> Though modern French treats &#x153; as a single
letter rather than a typographic ligature, its glyph still required
careful kerning against adjacent characters &#x2014; the &#x153;u in
&#x201C;c&#x153;ur,&#x201D; the &#x153;v in &#x201C;&#x153;uvre,&#x201D; the b&#x153; in
&#x201C;b&#x153;uf.&#x201D;</p>

<p>Dutch provided the ĳ digraph. <i>Het ĳzer is sterk. Zĳ is ĳverig en
bĳzonder vrĳ in haar oordeel.</i> The ĳ glyph, occupying a single
codepoint (U+0133), needed its own kerning entries &#x2014; particularly
the pairs Hĳ, Zĳ, bĳ, and vrĳ, where the preceding letter&#x2019;s
right-side bearing abutted the unusual shape of the ĳ.</p>

<h2>Extended-A Kerning Glossary</h2>

<p>Avery appended a supplementary glossary to his earlier catalogue:</p>

<p><b>T&#x11B;</b> &#x2014; As in T&#x11B;&#x161;&#xED;n, t&#x11B;&#x17E;k&#xFD;, t&#x11B;lo.<br/>
<b>T&#x159;</b> &#x2014; As in T&#x159;eb&#xED;&#x10D;, t&#x159;&#xED;da, t&#x159;i.<br/>
<b>V&#x11B;</b> &#x2014; As in V&#x11B;ra, v&#x11B;da, v&#x11B;&#x17E;.<br/>
<b>P&#x159;</b> &#x2014; As in P&#x159;&#xED;bram, p&#x159;&#xED;roda, p&#x159;&#xED;tel.<br/>
<b>W&#x105;</b> &#x2014; As in W&#x105;chock, w&#x105;ski, w&#x105;w&#xF3;z.<br/>
<b>W&#x119;</b> &#x2014; As in W&#x119;gry, w&#x119;ze&#x142;, W&#x119;gierska.<br/>
<b>&#x141;&#xF3;</b> &#x2014; As in &#x141;&#xF3;d&#x17A;, &#x142;&#xF3;d&#x17A;, &#x142;&#xF3;&#x17C;ko.<br/>
<b>&#x141;u</b> &#x2014; As in &#x141;ukasz, &#x141;uck, &#x142;uk.<br/>
<b>&#x141;y</b> &#x2014; As in &#x141;y&#x17C;ka, &#x142;ydka, &#x142;ysy.<br/>
<b>&#x10C;&#xE1;</b> &#x2014; As in &#x10C;&#xE1;slav, &#x10D;&#xE1;st, &#x10D;&#xE1;p.<br/>
<b>&#x10C;e</b> &#x2014; As in &#x10C;esk&#xE9;, &#x10D;esk&#xFD;, &#x10D;elo.<br/>
<b>&#x158;&#xED;</b> &#x2014; As in &#x158;&#xED;jen, &#x159;&#xED;&#x10D;n&#xED;, &#x159;&#xED;zen&#xED;.<br/>
<b>&#x158;e</b> &#x2014; As in &#x158;eka, &#x159;e&#x10D;, &#x159;emeslo.<br/>
<b>&#x160;k</b> &#x2014; As in &#x160;koda, &#x161;k&#xE1;la, &#x161;kol&#xE1;k.<br/>
<b>&#x160;&#x165;</b> &#x2014; As in &#x160;&#x165;astn&#xFD;.<br/>
<b>&#x17D;i</b> &#x2014; As in &#x17D;ivot, &#x17E;iv&#xFD;, &#x17E;ivnost.<br/>
<b>&#x17D;&#xE1;</b> &#x2014; As in &#x17D;&#xE1;dn&#xFD;, &#x17E;&#xE1;k, &#x17E;&#xE1;r.<br/>
<b>A&#x165;</b> &#x2014; As in a&#x165; (Czech: &#x201C;let&#x201D; / &#x201C;whether&#x201D;).<br/>
<b>T&#x151;</b> &#x2014; As in t&#x151;ke, t&#x151;r, t&#x151;leg&#xE9;ny.<br/>
<b>V&#x151;</b> &#x2014; As in v&#x151;leg&#xE9;ny, v&#x151;f&#xE9;l.<br/>
<b>&#x130;s</b> &#x2014; As in &#x130;stanbul, &#x130;stiklal, &#x130;slam.<br/>
<b>&#x11E;a</b> &#x2014; As in da&#x11F;, ya&#x11F;mur, &#x11F;araj.<br/>
<b>&#x15E;i</b> &#x2014; As in &#x15E;i&#x15F;li, &#x15F;ifa, &#x15F;irin.</p>

<p>&#x201C;If we can kern all of these correctly,&#x201D; Avery declared,
&#x201C;we&#x2019;ll have covered every major Latin-script language in
Europe and beyond. Not just the Western set &#x2014; the full Latin
range.&#x201D;</p>

<p>Vera looked at the list and sighed. &#x201C;I&#x2019;ll put the kettle on.
This is going to be a long night.&#x201D;</p>
</body>
</html>
"""

CHAPTER_8 = """\
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="en" lang="en">
<head><title>Chapter 8 &#x2013; The Cyrillic Challenge</title>
<link rel="stylesheet" type="text/css" href="style.css"/></head>
<body>
<h1>Chapter 8<br/>The Cyrillic Challenge</h1>

<p>The companion volume was barely off the press when Mrs. Thornton-Foxwell
telephoned again. &#x201C;Avery, darling, I&#x2019;ve been in contact with
a collector in Saint Petersburg. He wants the calligraphy survey
extended to cover Cyrillic traditions &#x2014; Russian, Ukrainian, Bulgarian.
The same standard.&#x201D;</p>

<p>Avery set down his coffee. Cyrillic was an entirely new script, with its
own letterforms and its own kerning nightmares. Several Cyrillic letters
shared shapes with their Latin counterparts &#x2014; &#x410; resembled A,
&#x420; resembled P, &#x422; resembled T &#x2014; but many others were
unique. He would need to kern every pair from scratch.</p>

<h2>The Overhanging Letters</h2>

<p>The most troublesome Cyrillic letter was &#x413; (Ge). Its shape &#x2014;
a horizontal crossbar extending rightward from a vertical stem, like a
reversed L &#x2014; created an overhang that demanded tight kerning against
every following letter. Avery set his first test: <i>&#x413;&#x435;&#x43D;&#x435;&#x440;&#x430;&#x43B;
&#x413;&#x43E;&#x433;&#x43E;&#x43B;&#x44C; &#x433;&#x43E;&#x432;&#x43E;&#x440;&#x438;&#x43B; &#x43E; &#x413;&#x443;&#x441;&#x430;&#x440;&#x430;&#x445;.
&#x413;&#x440;&#x430;&#x43C;&#x43E;&#x442;&#x430; &#x413;&#x430;&#x43B;&#x438;&#x43B;&#x435;&#x44F;
&#x43F;&#x43E;&#x440;&#x430;&#x437;&#x438;&#x43B;&#x430; &#x413;&#x435;&#x440;&#x43C;&#x430;&#x43D;&#x438;&#x44E;.</i></p>

<p>The &#x413;&#x435; in &#x201C;&#x413;&#x435;&#x43D;&#x435;&#x440;&#x430;&#x43B;&#x201D; was
critical &#x2014; the crossbar of &#x413; needed to tuck over the
lowercase &#x435; without crushing it. &#x413;&#x43E; in
&#x201C;&#x413;&#x43E;&#x433;&#x43E;&#x43B;&#x44C;&#x201D; demanded similar attention, as did
&#x413;&#x443; in &#x201C;&#x413;&#x443;&#x441;&#x430;&#x440;&#x430;&#x445;&#x201D; and &#x413;&#x440; in
&#x201C;&#x413;&#x440;&#x430;&#x43C;&#x43E;&#x442;&#x430;.&#x201D; &#x413;&#x430; in
&#x201C;&#x413;&#x430;&#x43B;&#x438;&#x43B;&#x435;&#x44F;&#x201D; rounded out the set.</p>

<p>&#x422; (Te) presented the same challenge as its Latin twin T. <i>&#x422;&#x430;&#x43C;
&#x422;&#x430;&#x442;&#x44C;&#x44F;&#x43D;&#x430; &#x442;&#x438;&#x445;&#x43E; &#x442;&#x43A;&#x430;&#x43B;&#x430;
&#x442;&#x43A;&#x430;&#x43D;&#x44C;. &#x422;&#x435;&#x43F;&#x43B;&#x43E; &#x442;&#x435;&#x43A;&#x43B;&#x43E; &#x438;&#x437;
&#x422;&#x443;&#x43B;&#x44C;&#x441;&#x43A;&#x43E;&#x433;&#x43E; &#x43A;&#x430;&#x43C;&#x438;&#x43D;&#x430;.
&#x422;&#x440;&#x438; &#x442;&#x44B;&#x441;&#x44F;&#x447;&#x438; &#x442;&#x440;&#x443;&#x431;
&#x43F;&#x435;&#x43B;&#x438; &#x432; &#x422;&#x44F;&#x43D;&#x44C;&#x446;&#x437;&#x438;&#x43D;&#x44C;.</i>
Every pair &#x2014; &#x422;&#x430;, &#x422;&#x435;, &#x422;&#x443;, &#x422;&#x440;,
&#x422;&#x438;, &#x422;&#x44F; &#x2014; required the T-crossbar to reach over the
following lowercase letter.</p>

<p>&#x427; (Che) had a subtler overhang. <i>&#x427;&#x430;&#x441;&#x44B;
&#x43F;&#x440;&#x43E;&#x431;&#x438;&#x43B;&#x438; &#x447;&#x435;&#x442;&#x432;&#x435;&#x440;&#x442;&#x44C;
&#x447;&#x435;&#x442;&#x432;&#x435;&#x440;&#x433;&#x430;. &#x427;&#x443;&#x434;&#x43E;!
&#x427;&#x43E;&#x440;&#x43D;&#x43E;&#x435; &#x43C;&#x43E;&#x440;&#x435;.</i> The
&#x427;&#x430;, &#x427;&#x443;, &#x427;&#x43E; pairs each had different spacing needs
depending on the round or straight shape of the following vowel.</p>

<h2>The Diagonal Letters</h2>

<p>&#x423; (U) was the Cyrillic counterpart of the Latin Y &#x2014; a letter whose
diagonals created open space against adjacent characters. <i>&#x423;&#x432;&#x435;&#x440;&#x435;&#x43D;&#x43D;&#x43E;&#x441;&#x442;&#x44C;
&#x423;&#x434;&#x430;&#x43B;&#x43E;&#x441;&#x44C; &#x443;&#x43A;&#x440;&#x435;&#x43F;&#x438;&#x442;&#x44C;.
&#x423;&#x43C; &#x443;&#x441;&#x442;&#x440;&#x435;&#x43C;&#x438;&#x43B;&#x441;&#x44F; &#x432;&#x43F;&#x435;&#x440;&#x451;&#x434;.</i>
The &#x423;&#x432;, &#x423;&#x434;, &#x423;&#x43A;, &#x423;&#x43C; pairs all
needed tighter kerning than the default sidebearings provided.</p>

<p>&#x410; (A) and &#x41B; (El) were equally demanding. <i>&#x410;&#x443;&#x434;&#x438;&#x442;&#x43E;&#x440;&#x438;&#x44F;
&#x410;&#x432;&#x438;&#x430;&#x442;&#x43E;&#x440; &#x410;&#x442;&#x43B;&#x430;&#x441;
&#x410;&#x434;&#x440;&#x435;&#x441;&#x430;&#x442;. &#x41B;&#x430;&#x43C;&#x43F;&#x430;
&#x41B;&#x435;&#x43D;&#x438;&#x43D;&#x433;&#x440;&#x430;&#x434; &#x41B;&#x43E;&#x43D;&#x434;&#x43E;&#x43D;
&#x41B;&#x443;&#x43D;&#x430;.</i> The &#x410;&#x443;, &#x410;&#x432;, &#x410;&#x442;,
&#x410;&#x434; pairs mirrored the Latin AV/AW/AT family. The &#x41B; (El),
with its inverted-V left stroke, created unique spacing against
&#x430;, &#x435;, &#x43E;, &#x443;.</p>

<h2>Round and Complex Letters</h2>

<p>&#x420; (Er) was the Cyrillic P &#x2014; a letter with a bowl that overhung
the following character. <i>&#x420;&#x430;&#x431;&#x43E;&#x442;&#x430;
&#x420;&#x435;&#x447;&#x438; &#x420;&#x43E;&#x441;&#x441;&#x438;&#x438;
&#x420;&#x443;&#x441;&#x441;&#x43A;&#x438;&#x439;.</i> The &#x420;&#x430;,
&#x420;&#x435;, &#x420;&#x43E;, &#x420;&#x443; pairs echoed the Latin Pa, Pe, Po
challenge.</p>

<p>&#x424; (Ef) was the widest Cyrillic letter &#x2014; a circle bisected by a
vertical stem. <i>&#x424;&#x430;&#x43A;&#x443;&#x43B;&#x44C;&#x442;&#x435;&#x442;
&#x424;&#x43E;&#x43D;&#x442;&#x430;&#x43D;&#x43A;&#x430; &#x424;&#x443;&#x440;&#x430;.</i>
The &#x424;&#x430;, &#x424;&#x43E;, &#x424;&#x443; pairs needed generous clearance
on both sides of the circle.</p>

<p>&#x414; (De) had descending serifs that complicated baseline kerning.
<i>&#x414;&#x430;&#x43B;&#x44C;&#x43D;&#x438;&#x439; &#x414;&#x435;&#x43D;&#x44C;
&#x414;&#x43E;&#x43C;&#x430; &#x414;&#x443;&#x43C;&#x430;&#x442;&#x44C;.</i> The
&#x414;&#x430;, &#x414;&#x435;, &#x414;&#x43E;, &#x414;&#x443; pairs were unique to
Cyrillic &#x2014; no Latin letter had quite the same descending structure.</p>

<h2>Ukrainian and Bulgarian</h2>

<p>Ukrainian added its own characters. <i>&#x407;&#x457; &#x43C;&#x430;&#x442;&#x438;
&#x43D;&#x435;&#x43C;&#x430;&#x454; &#x440;&#x456;&#x432;&#x43D;&#x438;&#x445;.
&#x404;&#x432;&#x440;&#x43E;&#x43F;&#x430; &#x447;&#x435;&#x43A;&#x430;&#x454;.
&#x490;&#x430;&#x43D;&#x43E;&#x43A; &#x432;&#x438;&#x440;&#x456;&#x441;
&#x43D;&#x430; &#x490;&#x440;&#x443;&#x43D;&#x442;&#x456;.</i>
The &#x407;&#x457; pair (Yi + yi) tested the double-dotted characters
unique to Ukrainian. &#x404;&#x432; in &#x201C;&#x404;&#x432;&#x440;&#x43E;&#x43F;&#x430;&#x201D;
tested the Ukrainian Ye against a following consonant. &#x490;&#x430; and
&#x490;&#x440; in &#x201C;&#x490;&#x430;&#x43D;&#x43E;&#x43A;&#x201D; and
&#x201C;&#x490;&#x440;&#x443;&#x43D;&#x442;&#x456;&#x201D; tested the upturn-Ge
(&#x490;), a letter unique to Ukrainian.</p>

<p>Bulgarian Cyrillic had its own typographic traditions. <i>&#x429;&#x443;&#x43A;&#x430;
&#x449;&#x430;&#x441;&#x442;&#x43B;&#x438;&#x432;&#x430; &#x436;&#x435;&#x43D;&#x430;
&#x436;&#x438;&#x432;&#x435;&#x435;&#x448;&#x435; &#x432; &#x416;&#x435;&#x43B;&#x435;&#x437;&#x43D;&#x438;&#x43A;.
&#x42E;&#x43B;&#x438;&#x44F; &#x44E;&#x442;&#x438;&#x43B;&#x430;&#x441;&#x44C;.</i>
The &#x429;&#x443; pair tested the complex Shcha with its descender
against a round vowel. &#x416;&#x430; and &#x416;&#x435; tested the wide Zhe.
&#x42E;&#x43B; in &#x201C;&#x42E;&#x43B;&#x438;&#x44F;&#x201D; placed the round Yu
against the narrow El.</p>

<h2>Cyrillic with Guillemets</h2>

<p>Russian typography uses guillemets as quotation marks, just like French.
&#xAB;&#x413;&#x43E;&#x432;&#x43E;&#x440;&#x438;&#x442;&#x435; &#x442;&#x438;&#x448;&#x435;,&#xBB;
&#x2014; &#x441;&#x43A;&#x430;&#x437;&#x430;&#x43B;&#x430; &#x43E;&#x43D;&#x430;.
&#xAB;&#x422;&#x438;&#x445;&#x43E;!&#xBB;
&#xAB;&#x412;&#x441;&#x451; &#x431;&#x443;&#x434;&#x435;&#x442;
&#x445;&#x43E;&#x440;&#x43E;&#x448;&#x43E;,&#xBB; &#x2014;
&#x43E;&#x442;&#x432;&#x435;&#x442;&#x438;&#x43B; &#x43E;&#x43D;.
The &#xAB;&#x413;, &#xAB;&#x422;, &#xAB;&#x412; pairs &#x2014; guillemet
against the overhanging Ge, the crossbarred Te, and the round Ve &#x2014;
each needed individual spacing. On the closing side, &#x440;&#xBB; and
&#x435;&#xBB; presented the same challenges as their Latin counterparts.</p>

<h2>Cyrillic Kerning Glossary</h2>

<p>Avery appended the Cyrillic pairs to his growing catalogue:</p>

<p><b>&#x413;&#x430;</b> &#x2014; &#x413;&#x430;&#x43B;&#x438;&#x43B;&#x435;&#x44F;, &#x433;&#x430;&#x437;&#x435;&#x442;&#x430;.<br/>
<b>&#x413;&#x435;</b> &#x2014; &#x413;&#x435;&#x43D;&#x435;&#x440;&#x430;&#x43B;, &#x433;&#x435;&#x440;&#x43E;&#x439;.<br/>
<b>&#x413;&#x43E;</b> &#x2014; &#x413;&#x43E;&#x433;&#x43E;&#x43B;&#x44C;, &#x433;&#x43E;&#x440;&#x43E;&#x434;.<br/>
<b>&#x413;&#x443;</b> &#x2014; &#x413;&#x443;&#x441;&#x430;&#x440;&#x44B;, &#x433;&#x443;&#x431;&#x435;&#x440;&#x43D;&#x438;&#x44F;.<br/>
<b>&#x413;&#x440;</b> &#x2014; &#x413;&#x440;&#x430;&#x43C;&#x43E;&#x442;&#x430;, &#x433;&#x440;&#x430;&#x43D;&#x438;&#x446;&#x430;.<br/>
<b>&#x422;&#x430;</b> &#x2014; &#x422;&#x430;&#x43C;, &#x442;&#x430;&#x43A;&#x436;&#x435;, &#x442;&#x430;&#x43D;&#x435;&#x446;.<br/>
<b>&#x422;&#x435;</b> &#x2014; &#x422;&#x435;&#x43F;&#x43B;&#x43E;, &#x442;&#x435;&#x43A;&#x441;&#x442;, &#x442;&#x435;&#x43B;&#x43E;.<br/>
<b>&#x422;&#x43E;</b> &#x2014; &#x422;&#x43E;&#x43B;&#x44C;&#x43A;&#x43E;, &#x442;&#x43E;&#x432;&#x430;&#x440;.<br/>
<b>&#x422;&#x443;</b> &#x2014; &#x422;&#x443;&#x43B;&#x44C;&#x441;&#x43A;&#x438;&#x439;, &#x442;&#x443;&#x447;&#x430;.<br/>
<b>&#x422;&#x440;</b> &#x2014; &#x422;&#x440;&#x438;, &#x442;&#x440;&#x443;&#x431;&#x430;.<br/>
<b>&#x422;&#x44F;</b> &#x2014; &#x422;&#x44F;&#x43D;&#x44C;&#x446;&#x437;&#x438;&#x43D;&#x44C;.<br/>
<b>&#x420;&#x430;</b> &#x2014; &#x420;&#x430;&#x431;&#x43E;&#x442;&#x430;, &#x440;&#x430;&#x437;&#x443;&#x43C;.<br/>
<b>&#x420;&#x435;</b> &#x2014; &#x420;&#x435;&#x447;&#x438;, &#x440;&#x435;&#x43A;&#x430;.<br/>
<b>&#x420;&#x43E;</b> &#x2014; &#x420;&#x43E;&#x441;&#x441;&#x438;&#x44F;, &#x440;&#x43E;&#x434;.<br/>
<b>&#x420;&#x443;</b> &#x2014; &#x420;&#x443;&#x441;&#x441;&#x43A;&#x438;&#x439;, &#x440;&#x443;&#x43A;&#x430;.<br/>
<b>&#x410;&#x443;</b> &#x2014; &#x410;&#x443;&#x434;&#x438;&#x442;&#x43E;&#x440;&#x438;&#x44F;.<br/>
<b>&#x410;&#x432;</b> &#x2014; &#x410;&#x432;&#x438;&#x430;&#x442;&#x43E;&#x440;.<br/>
<b>&#x410;&#x442;</b> &#x2014; &#x410;&#x442;&#x43B;&#x430;&#x441;, &#x430;&#x442;&#x43E;&#x43C;.<br/>
<b>&#x410;&#x434;</b> &#x2014; &#x410;&#x434;&#x440;&#x435;&#x441;&#x430;&#x442;.<br/>
<b>&#x423;&#x432;</b> &#x2014; &#x423;&#x432;&#x435;&#x440;&#x435;&#x43D;&#x43D;&#x43E;&#x441;&#x442;&#x44C;.<br/>
<b>&#x423;&#x434;</b> &#x2014; &#x423;&#x434;&#x430;&#x43B;&#x43E;&#x441;&#x44C;.<br/>
<b>&#x423;&#x43A;</b> &#x2014; &#x423;&#x43A;&#x440;&#x435;&#x43F;&#x438;&#x442;&#x44C;.<br/>
<b>&#x423;&#x43C;</b> &#x2014; &#x423;&#x43C;, &#x443;&#x43C;&#x43D;&#x438;&#x43A;.<br/>
<b>&#x414;&#x430;</b> &#x2014; &#x414;&#x430;&#x43B;&#x44C;&#x43D;&#x438;&#x439;, &#x434;&#x430;&#x442;&#x430;.<br/>
<b>&#x414;&#x435;</b> &#x2014; &#x414;&#x435;&#x43D;&#x44C;, &#x434;&#x435;&#x43B;&#x43E;.<br/>
<b>&#x414;&#x43E;</b> &#x2014; &#x414;&#x43E;&#x43C;&#x430;, &#x434;&#x43E;&#x440;&#x43E;&#x433;&#x430;.<br/>
<b>&#x41B;&#x430;</b> &#x2014; &#x41B;&#x430;&#x43C;&#x43F;&#x430;, &#x43B;&#x430;&#x432;&#x43A;&#x430;.<br/>
<b>&#x41B;&#x435;</b> &#x2014; &#x41B;&#x435;&#x43D;&#x438;&#x43D;&#x433;&#x440;&#x430;&#x434;, &#x43B;&#x435;&#x441;.<br/>
<b>&#x41B;&#x43E;</b> &#x2014; &#x41B;&#x43E;&#x43D;&#x434;&#x43E;&#x43D;, &#x43B;&#x43E;&#x434;&#x43A;&#x430;.<br/>
<b>&#x427;&#x430;</b> &#x2014; &#x427;&#x430;&#x441;&#x44B;, &#x447;&#x430;&#x439;.<br/>
<b>&#x427;&#x43E;</b> &#x2014; &#x427;&#x43E;&#x440;&#x43D;&#x43E;&#x435;, &#x447;&#x43E;&#x440;&#x442;.<br/>
<b>&#x427;&#x443;</b> &#x2014; &#x427;&#x443;&#x434;&#x43E;, &#x447;&#x443;&#x432;&#x441;&#x442;&#x432;&#x43E;.<br/>
<b>&#x424;&#x430;</b> &#x2014; &#x424;&#x430;&#x43A;&#x443;&#x43B;&#x44C;&#x442;&#x435;&#x442;.<br/>
<b>&#x424;&#x43E;</b> &#x2014; &#x424;&#x43E;&#x43D;&#x442;&#x430;&#x43D;&#x43A;&#x430;.</p>

<p>&#x201C;Cyrillic has fewer kerning traps than Latin,&#x201D; Avery reflected,
&#x201C;but the ones it has are severe. &#x413; and &#x422; dominate every
page of Russian text, and if they&#x2019;re not kerned properly, the whole
paragraph looks like it&#x2019;s falling apart.&#x201D;</p>

<p>Vera glanced at the stack of proofs &#x2014; now three volumes deep &#x2014;
and smiled wearily. &#x201C;At least there are no Cyrillic ligatures.&#x201D;</p>

<p>&#x201C;Yet,&#x201D; said Avery.</p>
</body>
</html>
"""

CHAPTER_9 = """\
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="en" lang="en">
<head><title>Chapter 9 &#x2013; Latin Extended-B</title>
<link rel="stylesheet" type="text/css" href="style.css"/></head>
<body>
<h1>Chapter 9<br/>Latin Extended-B</h1>

<p>Months passed. Avery had just begun to relax when the telephone rang
again. This time it was not Mrs. Thornton-Foxwell but her publisher,
a harried man named Grigor, who explained that the calligraphy survey
had attracted interest from scholars in Bucharest, Hanoi, and Lagos.
&#x201C;We need Romanian, Vietnamese, and several West African
languages,&#x201D; he said. &#x201C;Plus Croatian digraphs and Pinyin
romanization. Latin Extended-B, the whole block.&#x201D;</p>

<p>Avery looked at the Unicode chart for U+0180&#x2013;U+024F and sighed.
It was a miscellany: characters from a dozen unrelated traditions,
each with its own typographic demands.</p>

<h2>Romanian</h2>

<p>Romanian was the most urgent addition. The language required two
characters that looked deceptively like their Latin-1 cousins but
were typographically distinct: &#x218; (S with comma below, U+0218)
and &#x21A; (T with comma below, U+021A). Avery set a test
paragraph: <i>&#x21A;ara noastr&#x103; este frumoas&#x103;.
&#x218;ase sute de &#x219;colari au venit la &#x21A;&#x103;r&#x103;ncu&#x21B;a.
&#x21A;es&#x103;tura &#x21B;inutului este unic&#x103;.
&#x218;eful sta&#x21B;iei &#x219;tia totul.</i></p>

<p>The &#x21A;&#x103; pair in &#x201C;&#x21A;ara&#x201D; and
&#x201C;&#x21A;&#x103;r&#x103;ncu&#x21B;a&#x201D; was the key test &#x2014;
the comma descender on &#x21A; distinguished it from the cedilla-T
(&#xDE;) found in Turkish, but the crossbar overhang was identical.
&#x21A;e in &#x201C;&#x21A;es&#x103;tura&#x201D; and &#x21A;o demanded the same
T-crossbar tucking as their ASCII equivalents. &#x218;a and &#x218;e in
&#x201C;&#x218;ase&#x201D; and &#x201C;&#x218;eful&#x201D; needed the comma below to
clear the baseline without colliding with descenders in the line
below.</p>

<p>&#x201C;The tricky part,&#x201D; Avery told Vera, &#x201C;is that Romanian also uses
&#x103; (a-breve) and &#xEE; (i-circumflex) from Latin-1, and
&#x21B; (t-comma) interacts with both. The pair &#x21B;&#x103; in
&#x2018;&#x21A;&#x103;r&#x103;ncu&#x21B;a&#x2019; tests the comma-below
against the breve-above &#x2014; a vertical sandwich of diacritics.&#x201D;</p>

<h2>Vietnamese</h2>

<p>Vietnamese typography brought the horn diacritic into play. The
characters &#x1A0; (O with horn, U+01A0) and &#x1AF;
(U with horn, U+01AF) appeared constantly in Vietnamese text. Avery
set: <i>&#x1A0;i &#x21; Ng&#x1B0;&#x1A1;i Vi&#x1EC7;t Nam
y&#xEA;u th&#x1B0;&#x1A1;ng &#x111;&#x1EA5;t n&#x1B0;&#x1EDB;c.
V&#x1EEB;a &#x111;&#x1EB9;p v&#x1EEB;a hay. T&#x1B0;&#x1A1;i s&#xE1;ng
r&#x1EE1;i.</i></p>

<p>The horn on &#x1A0; and &#x1AF; extended to the upper right of the
letter, creating potential collisions with the following character.
T&#x1B0; and V&#x1B0; were particularly demanding: the T-crossbar or
V-diagonal needed to accommodate the horn&#x2019;s extra width. Similarly,
T&#x1A1; placed the T&#x2019;s crossbar over a horned lowercase o &#x2014; the
horn could crash into the crossbar at small sizes.</p>

<h2>Croatian Digraphs</h2>

<p>Croatian contributed its titular digraph ligatures. The Unicode
block included precomposed forms: D&#x17D; (U+01C4), D&#x17E; (U+01C5),
d&#x17E; (U+01C6), LJ (U+01C7), Lj (U+01C8), lj (U+01C9),
NJ (U+01CA), Nj (U+01CB), nj (U+01CC). These were single codepoints
representing two-letter combinations, each with unique glyph widths.
<i>D&#x17E;ep je velik. Ljeto je toplo. Njiva je zelena.
D&#x17E;amija stoji na brdu.</i></p>

<p>&#x201C;These digraphs are wider than normal letters,&#x201D; Avery observed.
&#x201C;Kerning D&#x17E; against a following lowercase vowel is unlike
kerning D or &#x17D; individually &#x2014; the combined glyph has its own
sidebearings. Same for Lj and Nj.&#x201D;</p>

<h2>Pinyin Tone Marks</h2>

<p>Mandarin Chinese romanization &#x2014; Pinyin &#x2014; used Latin letters
with caron and diaeresis-plus-tone combinations that fell squarely
in Extended-B. <i>N&#x1D0; h&#x1CE;o! W&#x1D2; shi
Zh&#x14D;nggu&#xF3; r&#xE9;n. L&#x1DA;shi zh&#x1D4;yi
T&#x1D0;men de f&#x101;y&#x12B;n.</i></p>

<p>The &#x1CE; (a with caron) under a T-crossbar in &#x201C;T&#x1CE;men&#x201D;
presented the same challenge as Czech T&#x11B; &#x2014; but the Pinyin
context meant it appeared in entirely different words. The diaeresis-
plus-tone characters were uniquely demanding: &#x1D6; (u-diaeresis-
macron), &#x1D8; (u-diaeresis-acute), &#x1DA; (u-diaeresis-caron),
&#x1DC; (u-diaeresis-grave) each stacked two diacritical marks above
the u, creating height that could collide with the preceding
T-crossbar. <i>L&#x1DC;shi T&#x1D6; V&#x1D8;</i> &#x2014; Avery set each
combination and winced at the vertical crowding.</p>

<h2>African Languages</h2>

<p>West African languages used hooked and barred variants of familiar
Latin letters. <i>&#x181;ala &#x253;e &#x18A;ala &#x257;e.
&#x191;arin kowa ya san. &#x190;di&#x272; &#x254;k&#x254; n&#x254;.</i>
The &#x181; (B-hook) and &#x18A; (D-hook) had descending hooks that
affected baseline spacing. &#x191; (F-hook) shared the overhang
issues of a standard F but with an added complication: the hook at the
bottom altered the letter&#x2019;s center of gravity. &#x191;a,
&#x191;o, &#x191;e all needed individual attention &#x2014; the hook
pulled the eye downward while the crossbar demanded tuck-over
kerning above.</p>

<p>&#x201C;And the open vowels,&#x201D; Avery added. &#x201C;&#x190;
(open E, U+0190) and &#x254; (open O, U+0254) have wider apertures
than their standard counterparts. Every consonant-to-open-vowel pair
needs rechecking.&#x201D;</p>

<h2>Extended-B Kerning Glossary</h2>

<p>Avery appended to his catalogue:</p>

<p><b>&#x21A;a</b> &#x2014; As in &#x21A;ara, &#x21B;ar&#x103;.<br/>
<b>&#x21A;e</b> &#x2014; As in &#x21A;es&#x103;tura, &#x21B;esut.<br/>
<b>&#x21A;o</b> &#x2014; As in &#x21A;oca, &#x21B;ocul.<br/>
<b>&#x218;a</b> &#x2014; As in &#x218;ase, &#x219;arpe.<br/>
<b>&#x218;e</b> &#x2014; As in &#x218;eful, &#x219;ed.<br/>
<b>T&#x1A1;</b> &#x2014; As in T&#x1A1;i, t&#x1A1;i s&#xE1;ng.<br/>
<b>T&#x1B0;</b> &#x2014; As in T&#x1B0;&#x1A1;i, t&#x1B0;&#x1A1;ng lai.<br/>
<b>V&#x1A1;</b> &#x2014; As in V&#x1A1;i, v&#x1A1;.<br/>
<b>V&#x1B0;</b> &#x2014; As in V&#x1EEB;a, v&#x1B0;&#x1A1;n.<br/>
<b>T&#x1CE;</b> &#x2014; As in T&#x1CE;men (Pinyin).<br/>
<b>T&#x1D6;</b> &#x2014; As in n&#x1D6; (Pinyin: female).<br/>
<b>&#x191;a</b> &#x2014; As in &#x191;arin (Hausa).<br/>
<b>&#x191;o</b> &#x2014; As in &#x191;oto (Hausa).</p>
</body>
</html>
"""

CHAPTER_10 = """\
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="en" lang="en">
<head><title>Chapter 10 &#x2013; Greek &amp; Coptic</title>
<link rel="stylesheet" type="text/css" href="style.css"/></head>
<body>
<h1>Chapter 10<br/>Greek &amp; Coptic</h1>

<p>The final challenge arrived not by telephone but by post: a handwritten
letter from a professor of classics at the University of Athens,
requesting that the calligraphy survey include a chapter on the Greek
alphabet. &#x201C;The birthplace of Western lettering,&#x201D; the professor
wrote, &#x201C;deserves proper typographic treatment.&#x201D;</p>

<p>Avery could hardly disagree. Greek was where it all began &#x2014; the
ancestor of Latin, Cyrillic, and Coptic. And the Greek alphabet had
its own kerning nightmares, many of them eerily familiar.</p>

<h2>The Overhanging Letters</h2>

<p>&#x393; (Gamma, U+0393) was the Greek counterpart of the Cyrillic
&#x413; and a close relative of the Latin T. Its horizontal stroke
extended rightward, creating the same tuck-over demands. Avery set:
<i>&#x393;&#x3B1;&#x3BB;&#x3AE;&#x3BD;&#x3B7; &#x3B3;&#x3B1;&#x3BB;&#x3AC;&#x3B6;&#x3B9;&#x3B1;
&#x393;&#x3B5;&#x3C9;&#x3C1;&#x3B3;&#x3AF;&#x3B1; &#x393;&#x3BF;&#x3C1;&#x3B3;&#x3CC;&#x3BD;&#x3B1;
&#x393;&#x3C1;&#x3B1;&#x3BC;&#x3BC;&#x3B1;&#x3C4;&#x3B9;&#x3BA;&#x3AE;
&#x393;&#x3C5;&#x3BC;&#x3BD;&#x3AC;&#x3C3;&#x3B9;&#x3BF;.</i></p>

<p>The &#x393;&#x3B1; in &#x201C;&#x393;&#x3B1;&#x3BB;&#x3AE;&#x3BD;&#x3B7;&#x201D; and
&#x201C;&#x3B3;&#x3B1;&#x3BB;&#x3AC;&#x3B6;&#x3B9;&#x3B1;&#x201D; needed tight
kerning &#x2014; the horizontal bar of &#x393; had to reach over the
&#x3B1; without crushing it. &#x393;&#x3B5; in
&#x201C;&#x393;&#x3B5;&#x3C9;&#x3C1;&#x3B3;&#x3AF;&#x3B1;&#x201D; was equally
sensitive. &#x393;&#x3BF; in &#x201C;&#x393;&#x3BF;&#x3C1;&#x3B3;&#x3CC;&#x3BD;&#x3B1;&#x201D;,
&#x393;&#x3C1; in &#x201C;&#x393;&#x3C1;&#x3B1;&#x3BC;&#x3BC;&#x3B1;&#x3C4;&#x3B9;&#x3BA;&#x3AE;&#x201D;,
and &#x393;&#x3C5; in &#x201C;&#x393;&#x3C5;&#x3BC;&#x3BD;&#x3AC;&#x3C3;&#x3B9;&#x3BF;&#x201D;
completed the set of Gamma&#x2019;s right-side partners.</p>

<p>&#x3A4; (Tau, U+03A4) was structurally identical to the Latin T.
<i>&#x3A4;&#x3B1;&#x3BE;&#x3AF;&#x3B4;&#x3B9; &#x3C4;&#x3B1;&#x3C7;&#x3CD;
&#x3A4;&#x3B5;&#x3C7;&#x3BD;&#x3BF;&#x3BB;&#x3BF;&#x3B3;&#x3AF;&#x3B1;
&#x3A4;&#x3BF;&#x3C0;&#x3BF;&#x3B8;&#x3B5;&#x3C3;&#x3AF;&#x3B1;
&#x3A4;&#x3C1;&#x3AF;&#x3B3;&#x3C9;&#x3BD;&#x3BF;
&#x3A4;&#x3C5;&#x3C1;&#x3AF;.</i>
Every pair &#x2014; &#x3A4;&#x3B1;, &#x3A4;&#x3B5;, &#x3A4;&#x3BF;,
&#x3A4;&#x3C1;, &#x3A4;&#x3C5; &#x2014; demanded the crossbar to tuck over
the following lowercase letter, just as in Latin.</p>

<h2>The Diagonal Letters</h2>

<p>&#x3A5; (Upsilon, U+03A5) mirrored the Latin Y&#x2019;s diagonal
challenges. <i>&#x3A5;&#x3B3;&#x3B5;&#x3AF;&#x3B1;
&#x3C5;&#x3C0;&#x3AC;&#x3C1;&#x3C7;&#x3B5;&#x3B9;
&#x3A5;&#x3C0;&#x3BF;&#x3C5;&#x3C1;&#x3B3;&#x3CC;&#x3C2;
&#x3A5;&#x3C0;&#x3BF;&#x3B8;&#x3AE;&#x3BA;&#x3B7;.</i>
The &#x3A5;&#x3B3;, &#x3A5;&#x3C0; pairs showed the same open spacing
that the Latin Y created against following lowercase letters.</p>

<p>&#x391; (Alpha, U+0391) and &#x39B; (Lambda, U+039B) were the
Greek equivalents of A and an inverted V. <i>&#x391;&#x3C5;&#x3C4;&#x3CC;&#x3C2;
&#x391;&#x3BD;&#x3B1;&#x3C4;&#x3BF;&#x3BB;&#x3AE; &#x391;&#x3C4;&#x3BB;&#x3B1;&#x3BD;&#x3C4;&#x3B9;&#x3BA;&#x3CC;&#x3C2;
&#x391;&#x3B4;&#x3B5;&#x3BB;&#x3C6;&#x3CC;&#x3C2;. &#x39B;&#x3B1;&#x3BC;&#x3C0;&#x3C1;&#x3AE;
&#x39B;&#x3B5;&#x3C5;&#x3BA;&#x3AC;&#x3B4;&#x3B1; &#x39B;&#x3BF;&#x3BD;&#x3B4;&#x3AF;&#x3BD;&#x3BF;.</i>
The &#x391;&#x3C5;, &#x391;&#x3BD;, &#x391;&#x3C4;, &#x391;&#x3B4; pairs
followed the same diagonal-against-vertical pattern as Latin AV, AW,
AT. &#x39B;&#x3B1;, &#x39B;&#x3B5;, &#x39B;&#x3BF; needed the inverted-V&#x2019;s
right stroke to relate cleanly to the following round or vertical
letter.</p>

<h2>Round and Complex Letters</h2>

<p>&#x3A1; (Rho, U+03A1) was the Greek P &#x2014; bowl overhanging the
following character. <i>&#x3A1;&#x3B1;&#x3B4;&#x3B9;&#x3CC;&#x3C6;&#x3C9;&#x3BD;&#x3BF;
&#x3A1;&#x3B5;&#x3CD;&#x3BC;&#x3B1; &#x3A1;&#x3BF;&#x3B4;&#x3CC;&#x3C2;
&#x3C1;&#x3CC;&#x3B4;&#x3B1;.</i> The &#x3A1;&#x3B1;,
&#x3A1;&#x3B5;, &#x3A1;&#x3BF; pairs echoed the Latin Pa, Pe, Po and
Cyrillic &#x420;&#x430;, &#x420;&#x435;, &#x420;&#x43E; challenges.</p>

<p>&#x3A6; (Phi, U+03A6) was one of the widest Greek letters &#x2014; a circle
bisected by a vertical stem, like the Cyrillic &#x424;.
<i>&#x3A6;&#x3B1;&#x3BD;&#x3C4;&#x3B1;&#x3C3;&#x3AF;&#x3B1;
&#x3A6;&#x3BF;&#x3AF;&#x3BD;&#x3B9;&#x3BA;&#x3B1;&#x3C2;
&#x3C6;&#x3C5;&#x3C3;&#x3B9;&#x3BA;&#x3AE;.</i> The &#x3A6;&#x3B1;,
&#x3A6;&#x3BF;, &#x3A6;&#x3C5; pairs needed generous clearance for the
circle&#x2019;s width.</p>

<p>&#x394; (Delta, U+0394) had a triangular shape with a wide base,
unlike anything in Latin. <i>&#x394;&#x3B1;&#x3C3;&#x3BA;&#x3AC;&#x3BB;&#x3B1;
&#x394;&#x3B5;&#x3BB;&#x3C6;&#x3BF;&#x3AF; &#x394;&#x3BF;&#x3CD;&#x3BD;&#x3B1;&#x3B2;&#x3B7;&#x3C2;
&#x3B4;&#x3B9;&#x3AC;&#x3B2;&#x3B1;&#x3C3;&#x3B7;.</i> The &#x394;&#x3B1;,
&#x394;&#x3B5;, &#x394;&#x3BF; pairs needed the wide base to relate
to the following letter without excessive gaps.</p>

<h2>Greek with Guillemets and Polytonic</h2>

<p>Modern Greek typography, like French and Russian, uses guillemets.
&#xAB;&#x393;&#x3B5;&#x3B9;&#x3B1; &#x3C3;&#x3B1;&#x3C2;,&#xBB;
&#x3B5;&#x3AF;&#x3C0;&#x3B5;. &#xAB;&#x3A4;&#x3B9;
&#x3BA;&#x3AC;&#x3BD;&#x3B5;&#x3C4;&#x3B5;;&#xBB;
&#xAB;&#x39A;&#x3B1;&#x3BB;&#x3AC;,&#xBB; &#x3B1;&#x3C0;&#x3AC;&#x3BD;&#x3C4;&#x3B7;&#x3C3;&#x3B5;.
The &#xAB;&#x393; and &#xAB;&#x3A4; pairs tested the guillemet
against overhanging capitals, while &#x3B5;&#xBB; and &#x3BF;&#xBB;
tested closing spacing.</p>

<p>Greek also carried a rich tradition of polytonic accents &#x2014; the
acute (&#x384;), grave, circumflex, rough breathing, and smooth
breathing marks that adorned classical and katharevousa texts.
<i>&#x1F08;&#x3B8;&#x3AE;&#x3BD;&#x3B1; &#x1F10;&#x3C3;&#x3C4;&#x3AF;
&#x3BC;&#x3B5;&#x3B3;&#x3AC;&#x3BB;&#x3B7; &#x3C0;&#x3CC;&#x3BB;&#x3B7;.
&#x1F48; &#x3BA;&#x3CC;&#x3C3;&#x3BC;&#x3BF;&#x3C2;
&#x3B5;&#x1F36;&#x3BD;&#x3B1;&#x3B9; &#x3C9;&#x3C1;&#x3B1;&#x1FD6;&#x3BF;&#x3C2;.</i>
Though polytonic marks are handled by combining characters (from the
U+0300 block), their visual interaction with kerning pairs
remained &#x2014; a breathing mark over an Alpha could encroach on the
preceding or following letter&#x2019;s space.</p>

<h2>Greek in Scientific Text</h2>

<p>Beyond natural language, Greek letters appeared constantly in
scientific and mathematical prose. <i>The wavelength &#x3BB; is
inversely proportional to frequency &#x3BD;. The ratio &#x3C0;/&#x3C6;
appears in the golden angle. Angle &#x3B8; subtends arc &#x3B1;&#x3B2;,
while &#x3A3; denotes summation and &#x394; denotes change.</i></p>

<p>&#x201C;When Greek letters appear inline with Latin text,&#x201D; Avery
explained, &#x201C;the kerning engine must handle cross-script pairs:
Latin-T followed by Greek-&#x3B1;, or Greek-&#x3C3; followed by
a Latin comma. These hybrid pairs are rare but they matter in
any book that discusses physics, mathematics, or engineering.&#x201D;</p>

<h2>Greek Kerning Glossary</h2>

<p>Avery added the final appendix to his growing catalogue:</p>

<p><b>&#x393;&#x3B1;</b> &#x2014; &#x393;&#x3B1;&#x3BB;&#x3AE;&#x3BD;&#x3B7;, &#x3B3;&#x3B1;&#x3BB;&#x3B1;&#x3BE;&#x3AF;&#x3B1;&#x3C2;.<br/>
<b>&#x393;&#x3B5;</b> &#x2014; &#x393;&#x3B5;&#x3C9;&#x3C1;&#x3B3;&#x3AF;&#x3B1;, &#x3B3;&#x3B5;&#x3C1;&#x3CC;&#x3C2;.<br/>
<b>&#x393;&#x3BF;</b> &#x2014; &#x393;&#x3BF;&#x3C1;&#x3B3;&#x3CC;&#x3BD;&#x3B1;, &#x3B3;&#x3BF;&#x3BD;&#x3B9;&#x3CC;&#x3C2;.<br/>
<b>&#x393;&#x3C5;</b> &#x2014; &#x393;&#x3C5;&#x3BC;&#x3BD;&#x3AC;&#x3C3;&#x3B9;&#x3BF;, &#x3B3;&#x3CD;&#x3C1;&#x3BF;&#x3C2;.<br/>
<b>&#x393;&#x3C1;</b> &#x2014; &#x393;&#x3C1;&#x3B1;&#x3BC;&#x3BC;&#x3B1;&#x3C4;&#x3B9;&#x3BA;&#x3AE;.<br/>
<b>&#x3A4;&#x3B1;</b> &#x2014; &#x3A4;&#x3B1;&#x3BE;&#x3AF;&#x3B4;&#x3B9;, &#x3C4;&#x3B1;&#x3C7;&#x3CD;.<br/>
<b>&#x3A4;&#x3B5;</b> &#x2014; &#x3A4;&#x3B5;&#x3C7;&#x3BD;&#x3BF;&#x3BB;&#x3BF;&#x3B3;&#x3AF;&#x3B1;.<br/>
<b>&#x3A4;&#x3BF;</b> &#x2014; &#x3A4;&#x3BF;&#x3C0;&#x3BF;&#x3B8;&#x3B5;&#x3C3;&#x3AF;&#x3B1;.<br/>
<b>&#x3A4;&#x3C5;</b> &#x2014; &#x3A4;&#x3C5;&#x3C1;&#x3AF;, &#x3C4;&#x3C5;&#x3C7;&#x3B5;&#x3C1;&#x3CC;&#x3C2;.<br/>
<b>&#x391;&#x3C5;</b> &#x2014; &#x391;&#x3C5;&#x3C4;&#x3CC;&#x3C2;, &#x3B1;&#x3C5;&#x3BB;&#x3AE;.<br/>
<b>&#x391;&#x3BD;</b> &#x2014; &#x391;&#x3BD;&#x3B1;&#x3C4;&#x3BF;&#x3BB;&#x3AE;.<br/>
<b>&#x391;&#x3C4;</b> &#x2014; &#x391;&#x3C4;&#x3BB;&#x3B1;&#x3BD;&#x3C4;&#x3B9;&#x3BA;&#x3CC;&#x3C2;.<br/>
<b>&#x391;&#x3B4;</b> &#x2014; &#x391;&#x3B4;&#x3B5;&#x3BB;&#x3C6;&#x3CC;&#x3C2;.<br/>
<b>&#x3A5;&#x3B3;</b> &#x2014; &#x3A5;&#x3B3;&#x3B5;&#x3AF;&#x3B1;.<br/>
<b>&#x3A5;&#x3C0;</b> &#x2014; &#x3A5;&#x3C0;&#x3BF;&#x3C5;&#x3C1;&#x3B3;&#x3CC;&#x3C2;.<br/>
<b>&#x3A1;&#x3B1;</b> &#x2014; &#x3A1;&#x3B1;&#x3B4;&#x3B9;&#x3CC;&#x3C6;&#x3C9;&#x3BD;&#x3BF;.<br/>
<b>&#x3A1;&#x3B5;</b> &#x2014; &#x3A1;&#x3B5;&#x3CD;&#x3BC;&#x3B1;.<br/>
<b>&#x3A1;&#x3BF;</b> &#x2014; &#x3A1;&#x3BF;&#x3B4;&#x3CC;&#x3C2;.<br/>
<b>&#x3A6;&#x3B1;</b> &#x2014; &#x3A6;&#x3B1;&#x3BD;&#x3C4;&#x3B1;&#x3C3;&#x3AF;&#x3B1;.<br/>
<b>&#x3A6;&#x3BF;</b> &#x2014; &#x3A6;&#x3BF;&#x3AF;&#x3BD;&#x3B9;&#x3BA;&#x3B1;&#x3C2;.<br/>
<b>&#x394;&#x3B1;</b> &#x2014; &#x394;&#x3B1;&#x3C3;&#x3BA;&#x3AC;&#x3BB;&#x3B1;.<br/>
<b>&#x394;&#x3B5;</b> &#x2014; &#x394;&#x3B5;&#x3BB;&#x3C6;&#x3BF;&#x3AF;.<br/>
<b>&#x39B;&#x3B1;</b> &#x2014; &#x39B;&#x3B1;&#x3BC;&#x3C0;&#x3C1;&#x3AE;.<br/>
<b>&#x39B;&#x3B5;</b> &#x2014; &#x39B;&#x3B5;&#x3C5;&#x3BA;&#x3AC;&#x3B4;&#x3B1;.<br/>
<b>&#x39B;&#x3BF;</b> &#x2014; &#x39B;&#x3BF;&#x3BD;&#x3B4;&#x3AF;&#x3BD;&#x3BF;.</p>

<p>&#x201C;And with that,&#x201D; Avery said, setting down his pencil for the last
time, &#x201C;we have covered every script from the Acropolis to the
Urals, from the Rhine to the Mekong. If a typesetter can render
every word in these chapters without a single miskerned pair,
they have earned my respect.&#x201D;</p>

<p>Vera closed her notebook and smiled. &#x201C;Shall I put the kettle on
one last time?&#x201D;</p>

<p>&#x201C;Please,&#x201D; said Avery. &#x201C;And make it strong.&#x201D;</p>
</body>
</html>
"""

CHAPTER_11 = """\
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="en" lang="en">
<head><title>Chapter 11 &#x2013; Combining Marks</title>
<link rel="stylesheet" type="text/css" href="style.css"/></head>
<body>
<h1>Chapter 11<br/>Combining Marks</h1>

<p>Avery had thought the project was finally complete when Vera placed
a new stack of proofs on his desk. &#x201C;These came from a different
typesetter,&#x201D; she explained. &#x201C;Their system outputs decomposed
Unicode &#x2014; every accented letter is split into a base character
followed by one or more combining diacritical marks.&#x201D;</p>

<p>Avery stared. &#x201C;You mean instead of &#xF6; as a single glyph, they
send o&#x0308;? And instead of &#xE9;, they send e&#x0301;?&#x201D;</p>

<p>&#x201C;Exactly. The renderer has to overlay the combining mark onto
the preceding base character &#x2014; centred horizontally, with proper
vertical clearance, and without advancing the cursor. If it gets any
of that wrong, the diacritics float off into space or crash into
neighbouring letters.&#x201D;</p>

<h2>Single Combining Marks</h2>

<p>Avery began with the most common combining diacritical marks from the
U+0300 block. He set each one after a simple base character to verify
placement:</p>

<p><i>a&#x0300; (a + grave), e&#x0301; (e + acute), i&#x0302; (i + circumflex),
o&#x0303; (o + tilde), u&#x0308; (u + diaeresis), a&#x030A; (a + ring above),
c&#x0327; (c + cedilla), e&#x0328; (e + ogonek), z&#x030C; (z + caron),
o&#x030B; (o + double acute), a&#x0304; (a + macron),
e&#x0306; (e + breve), z&#x0307; (z + dot above).</i></p>

<p>&#x201C;Each mark must sit centred over its base character,&#x201D; Avery
said, &#x201C;with at least a pixel of clearance between the top of the
base glyph and the bottom of the combining mark. If the mark drifts
left or right, the reader sees a broken letter.&#x201D;</p>

<h2>Decomposed German</h2>

<p>He turned to German text rendered entirely in decomposed form.
Every umlaut and eszett combination that had worked perfectly in
Chapter 6 now needed to survive the decomposition:</p>

<p><i>To&#x0308;chter sa&#xDF;en u&#x0308;ber den Bu&#x0308;chern.
Vo&#x0308;gel flogen u&#x0308;ber die Wa&#x0308;lder. Die Wu&#x0308;rde
des Menschen ist unantastbar. Ta&#x0308;nzer u&#x0308;bten in der
Tu&#x0308;rkei. O&#x0308;ffnung der A&#x0308;mter war um zehn Uhr.
A&#x0308;u&#xDF;erst sorgfa&#x0308;ltig pru&#x0308;fte er die
Gro&#x0308;&#xDF;e der Stra&#xDF;e.</i></p>

<p>The To&#x0308; in &#x201C;To&#x0308;chter&#x201D; was the critical
test &#x2014; the T-crossbar had to kern correctly against the
base o, while the combining diaeresis (U+0308) sat above
without shifting the cursor. Vo&#x0308; in &#x201C;Vo&#x0308;gel,&#x201D;
Wu&#x0308; in &#x201C;Wu&#x0308;rde,&#x201D; and Ta&#x0308; in
&#x201C;Ta&#x0308;nzer&#x201D; each exercised a different kerning pair
with a decomposed umlaut. The O&#x0308;f in &#x201C;O&#x0308;ffnung&#x201D;
tested a combining mark immediately before a double-f ligature.</p>

<h2>Decomposed French</h2>

<p>French offered its own decomposition challenges. Avery set the same
passage from Chapter 6, but with every accent decomposed:</p>

<p><i>Fe&#x0302;te de la Re&#x0301;publique. Pe&#x0300;re Noe&#x0308;l
arriva en Fe&#x0301;vrier. A&#x0300; la recherche du cafe&#x0301;
ide&#x0301;al. C&#x0327;a va? Garc&#x0327;on, un cafe&#x0301;
cre&#x0300;me, s&#x2019;il vous plai&#x0302;t.</i></p>

<p>The Fe&#x0302; in &#x201C;Fe&#x0302;te&#x201D; placed a combining
circumflex over the e after the F &#x2014; both the F-overhang kerning
and the mark placement had to work simultaneously.
Re&#x0301; in &#x201C;Re&#x0301;publique&#x201D; tested acute placement
after an R. The A&#x0300; in &#x201C;A&#x0300; la&#x201D; placed a
combining grave accent on a capital A, which had to clear the apex
of the letterform.</p>

<h2>Combining Marks and Ligatures</h2>

<p>The most demanding test combined decomposed diacritics with ligature
sequences. In precomposed text, the ligature engine only saw
single-codepoint accented letters. With decomposition, a combining
mark could sit between a base character and the start of a ligature,
or immediately after one:</p>

<p><i>La de&#x0301;finition de l&#x2019;efficacite&#x0301; re&#x0301;side
dans la re&#x0301;flexion. L&#x2019;officie&#x0300;re
ve&#x0301;rifia les diffe&#x0301;rentes souffle&#x0301;s. Il souffrit
magnifiquement. De&#x0301;fiant toute difficult&#xE9;, le
greffier affirma l&#x2019;efficience du syste&#x0300;me.</i></p>

<p>The fi ligature in &#x201C;de&#x0301;finition&#x201D; came right after
a combining acute on the e. The ffi in
&#x201C;efficacite&#x0301;&#x201D; was followed by a combining acute.
The fl in &#x201C;re&#x0301;flexion&#x201D; came after a combining
acute. The ff in &#x201C;diffe&#x0301;rentes&#x201D; contained a combining
mark between the ligature and the following vowel. Each of these
sequences tested whether the combining mark handler and the ligature
engine interacted correctly.</p>

<h2>Multiple Combining Marks</h2>

<p>Some writing systems required two or even three combining marks on
a single base character. Vietnamese was the classic example, where
a vowel could carry both a diacritical mark (circumflex, horn, or
breve) and a tone mark (acute, grave, hook above, tilde, or dot
below):</p>

<p><i>Vie&#x0302;&#x0323;t Nam ye&#x0302;u thu&#x031B;&#x0301;o&#x031B;ng
&#x0111;a&#x0302;&#x0301;t nu&#x031B;&#x0301;o&#x031B;&#x0301;c.
To&#x031B;&#x0300;i sa&#x0301;ng ro&#x0300;&#x0300;i.</i></p>

<p>The e&#x0302;&#x0323; in &#x201C;Vie&#x0302;&#x0323;t&#x201D; stacked
a combining circumflex (U+0302) and a combining dot below (U+0323) on
a single base e. Both marks had to be positioned correctly relative to
the base glyph and to each other &#x2014; the circumflex above and the
dot below the baseline. The u&#x031B;&#x0301; sequences placed a
combining horn (U+031B) and a combining acute (U+0301) on the same
base u, testing whether the second mark used the base character&#x2019;s
metrics rather than the first combining mark&#x2019;s.</p>

<h2>Combining Marks in Extended Latin</h2>

<p>The Czech and Polish texts from Chapter 7 could also appear in
decomposed form. Avery set a test paragraph:</p>

<p><i>Te&#x030C;s&#x030C;i&#x0301;n lez&#x030C;i&#x0301; nedaleko
Tr&#x030C;ebi&#x0301;c&#x030C;e. Pr&#x030C;i&#x0301;bram a
Pr&#x030C;erov jsou me&#x030C;sta. Ve&#x030C;ra se uc&#x030C;ila
ve&#x030C;de&#x030C;. C&#x030C;a&#x0301;slav lez&#x030C;i&#x0301;
na jih od C&#x030C;eske&#x0301;ho Brodu. Wa&#x0328;chock to
ma&#x0142;e miasteczko. We&#x0328;gry sa&#x0328;siaduja&#x0328; z
Polska&#x0328;.</i></p>

<p>The Te&#x030C; in &#x201C;Te&#x030C;s&#x030C;i&#x0301;n&#x201D; placed
a combining caron over e after the T-crossbar &#x2014; the same visual
result as the precomposed &#x11B;, but assembled from parts. Each
subsequent caron and acute in the sentence tested a different
base-plus-mark combination. The Polish ogonek (U+0328) in
&#x201C;Wa&#x0328;chock&#x201D; and &#x201C;We&#x0328;gry&#x201D; tested
a below-baseline combining mark, which had to clear descenders in the
line below without disrupting the W kerning.</p>

<h2>Combining Marks with Capitals</h2>

<p>Capital letters presented additional challenges because their greater
height left less room for marks above. Avery tested each common
combining mark on capitals:</p>

<p><i>A&#x0300; propos. A&#x0301;gnes. A&#x0302;me. A&#x0303;o.
A&#x0308;rger. A&#x030A;kesson. A&#x030C;lef. E&#x0300;ve.
E&#x0301;mile. E&#x0302;tre. I&#x0300;talo. I&#x0301;ngrid.
I&#x0302;le. O&#x0300;slo. O&#x0301;scar. O&#x0302;ter.
O&#x0303;telo. O&#x0308;ffnung. U&#x0300;bald. U&#x0301;ltimo.
U&#x0302;nion. U&#x0308;bung. N&#x0303;oqu&#xED;.</i></p>

<p>The combining marks on capitals sat higher than on lowercase letters,
and each mark needed to clear the top of the letterform. In particular,
A&#x0308; (A + combining diaeresis) and O&#x0308; (O + combining
diaeresis) had to match their precomposed equivalents &#xC4; and
&#xD6; visually &#x2014; any discrepancy would be immediately obvious
to the reader.</p>

<h2>Precomposed vs. Decomposed Comparison</h2>

<p>As a final verification, Avery set the same sentence in both forms,
one after the other, so the typesetter could compare them directly:</p>

<p><b>Precomposed:</b> <i>T&#xF6;chter &#xFC;bten in der T&#xFC;rkei.
V&#xF6;gel flogen &#xFC;ber die W&#xE4;lder. F&#xEA;te de la
R&#xE9;publique. &#xC0; la recherche du caf&#xE9;.
&#xC7;a va?</i></p>

<p><b>Decomposed:</b> <i>To&#x0308;chter u&#x0308;bten in der
Tu&#x0308;rkei. Vo&#x0308;gel flogen u&#x0308;ber die
Wa&#x0308;lder. Fe&#x0302;te de la Re&#x0301;publique.
A&#x0300; la recherche du cafe&#x0301;. C&#x0327;a va?</i></p>

<p>&#x201C;If those two lines are indistinguishable on screen,&#x201D; Avery
said, &#x201C;the combining mark renderer is working correctly. Any
difference in spacing, vertical position, or glyph alignment means
something is wrong.&#x201D;</p>

<p>Vera studied both lines through the loupe. &#x201C;They look identical
to me.&#x201D;</p>

<h2>Extended Latin Composition</h2>

<p>&#x201C;But what about the Latin Extended-A characters?&#x201D; Vera
asked. &#x201C;The old composition table only covered grave, acute,
circumflex, tilde, diaeresis, and cedilla. Characters like
&#x11B; (e-caron), &#x159; (r-caron), &#x105; (a-ogonek),
&#x142; (l-stroke), and &#x171; (u-double-acute)
were never composed from decomposed input.&#x201D;</p>

<p><b>Precomposed:</b> <i>T&#x11B;&#x161;&#xED;n le&#x17E;&#xED;
nedaleko T&#x159;eb&#xED;&#x10D;e. P&#x159;&#xED;bram a P&#x159;erov
jsou m&#x11B;sta. V&#x11B;ra se u&#x10D;ila v&#x11B;d&#x11B;.
&#x10C;&#xE1;slav le&#x17E;&#xED; na jih od &#x10C;esk&#xE9;ho
Brodu.</i></p>

<p><b>Decomposed:</b> <i>Te&#x030C;s&#x030C;i&#x0301;n
le&#x030C;z&#x030C;i&#x0301; nedaleko Tr&#x030C;ebi&#x0301;c&#x030C;e.
Pr&#x030C;i&#x0301;bram a Pr&#x030C;erov jsou me&#x030C;sta.
Ve&#x030C;ra se uc&#x030C;ila ve&#x030C;de&#x030C;.
C&#x030C;a&#x0301;slav le&#x030C;z&#x030C;i&#x0301; na jih od
C&#x030C;eske&#x0301;ho Brodu.</i></p>

<p><b>Precomposed:</b> <i>W&#x105;chock to ma&#x142;e miasteczko.
W&#x119;gry s&#x105;siaduj&#x105; z Polsk&#x105;. G&#x15F;r&#xFC;n
&#xDC;ber &#x15E;en &#x10E;&#xE1;le.</i></p>

<p><b>Decomposed:</b> <i>Wa&#x0328;chock to ma&#x142;e miasteczko.
We&#x0328;gry sa&#x0328;siaduja&#x0328; z Polska&#x0328;.
Gs&#x0327;ru&#x0308;n U&#x0308;ber S&#x0327;en
D&#x030C;a&#x0301;le.</i></p>

<p>&#x201C;With the new composition table these should be
indistinguishable,&#x201D; Avery said. &#x201C;Carons, ogoneks,
cedillas, double acutes &#x2014; all composed from their parts into the
same precomposed codepoints the font expects.&#x201D;</p>

<p>&#x201C;Then we&#x2019;re done,&#x201D; Avery said. &#x201C;Eleven chapters,
four scripts, three hundred kerning pairs, two dozen ligature sequences,
and now combining marks. If the renderer survives all of that, it can
handle anything a publisher throws at it.&#x201D;</p>

<p>He set down his pencil and reached for his coffee. It was cold.</p>
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
    <item id="ch6" href="chapter6.xhtml" media-type="application/xhtml+xml"/>
    <item id="ch7" href="chapter7.xhtml" media-type="application/xhtml+xml"/>
    <item id="ch8" href="chapter8.xhtml" media-type="application/xhtml+xml"/>
    <item id="ch9" href="chapter9.xhtml" media-type="application/xhtml+xml"/>
    <item id="ch10" href="chapter10.xhtml" media-type="application/xhtml+xml"/>
    <item id="ch11" href="chapter11.xhtml" media-type="application/xhtml+xml"/>
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
    <itemref idref="ch6"/>
    <itemref idref="ch7"/>
    <itemref idref="ch8"/>
    <itemref idref="ch9"/>
    <itemref idref="ch10"/>
    <itemref idref="ch11"/>
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
    <li><a href="chapter6.xhtml">Chapter 6 &#x2013; Western European Accents</a></li>
    <li><a href="chapter7.xhtml">Chapter 7 &#x2013; Beyond the Western Alphabet</a></li>
    <li><a href="chapter8.xhtml">Chapter 8 &#x2013; The Cyrillic Challenge</a></li>
    <li><a href="chapter9.xhtml">Chapter 9 &#x2013; Latin Extended-B</a></li>
    <li><a href="chapter10.xhtml">Chapter 10 &#x2013; Greek &amp; Coptic</a></li>
    <li><a href="chapter11.xhtml">Chapter 11 &#x2013; Combining Marks</a></li>
  </ol>
</nav>
</body>
</html>
"""


def build_epub(output_path: str):
    cover_data = create_cover_image()

    with zipfile.ZipFile(output_path, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr("mimetype", "application/epub+zip", compress_type=zipfile.ZIP_STORED)
        zf.writestr("META-INF/container.xml", CONTAINER_XML)
        zf.writestr("OEBPS/content.opf", CONTENT_OPF)
        zf.writestr("OEBPS/toc.xhtml", TOC_XHTML)
        zf.writestr("OEBPS/style.css", STYLESHEET)
        zf.writestr("OEBPS/cover.jpg", cover_data)
        zf.writestr("OEBPS/cover.xhtml", COVER_XHTML)
        zf.writestr("OEBPS/chapter1.xhtml", CHAPTER_1)
        zf.writestr("OEBPS/chapter2.xhtml", CHAPTER_2)
        zf.writestr("OEBPS/chapter3.xhtml", CHAPTER_3)
        zf.writestr("OEBPS/chapter4.xhtml", CHAPTER_4)
        zf.writestr("OEBPS/chapter5.xhtml", CHAPTER_5)
        zf.writestr("OEBPS/chapter6.xhtml", CHAPTER_6)
        zf.writestr("OEBPS/chapter7.xhtml", CHAPTER_7)
        zf.writestr("OEBPS/chapter8.xhtml", CHAPTER_8)
        zf.writestr("OEBPS/chapter9.xhtml", CHAPTER_9)
        zf.writestr("OEBPS/chapter10.xhtml", CHAPTER_10)
        zf.writestr("OEBPS/chapter11.xhtml", CHAPTER_11)
    print(f"EPUB written to {output_path}")


if __name__ == "__main__":
    project_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = os.path.join(project_root, "test", "epubs", "test_kerning_ligature.epub")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    build_epub(out)
