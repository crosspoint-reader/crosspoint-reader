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
        zf.writestr("OEBPS/chapter6.xhtml", CHAPTER_6)
        zf.writestr("OEBPS/chapter7.xhtml", CHAPTER_7)
    print(f"EPUB written to {output_path}")


if __name__ == "__main__":
    script_dir = os.path.dirname(os.path.abspath(__file__))
    out = os.path.join(script_dir, "kerning_ligature_test.epub")
    build_epub(out)
