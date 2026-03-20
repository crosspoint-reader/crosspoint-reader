#!/usr/bin/env python3
"""
Generate a test EPUB that exercises edge cases in spine/TOC relationships.

Edge cases covered:

  1. Multiple TOC entries → single spine item (via fragment anchors)
     - frontmatter.xhtml: Dedication (#dedication), Epigraph (#epigraph),
       Foreword (#foreword) — three TOC entries, one spine item
     - chapter3.xhtml: Chapter 3 heading + three sub-sections
       (#the-reef, #the-current, #the-depths) — four TOC entries, one spine item
     - chapter5.xhtml: Chapter 5 heading + three sub-sections
       (#isle-of-echoes, #compass-rose-atoll, #whirlpool-narrows) — four TOC entries
     - appendix.xhtml: Appendix A (#appendix-a), B (#appendix-b),
       C (#appendix-c), D (#appendix-d), E (#appendix-e)
       — five TOC entries, one spine item (D and E are deliberately tiny)

  2. Single TOC entry → multiple spine items (chapter spans files)
     - Chapter 2: chapter2_part1.xhtml + chapter2_part2.xhtml — one TOC entry,
       two spine items
     - Chapter 4: chapter4_part1.xhtml + chapter4_part2.xhtml +
       chapter4_part3.xhtml — one TOC entry, three spine items

  3. Spine item with no TOC entry
     - interlude.xhtml: present in spine order, absent from TOC nav

  4. TOC entry pointing to mid-file anchor (not file start)
     - backmatter.xhtml#colophon: the file starts with an Author's Note,
       but only the mid-file Colophon anchor appears in the TOC

  5. Nested TOC hierarchy
     - Chapter 3 and Chapter 5 use nested <ol> sub-entries in the nav

  6. Normal 1:1 spine-to-TOC mapping (baseline)
     - chapter1.xhtml: one spine item, one TOC entry
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
    for path in [_BOOKERLY_FONT]:
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
    """Generate a cover image and return JPEG bytes."""
    width, height = 536, 800
    bg_color = (15, 55, 65)
    text_color = (225, 220, 205)

    img = Image.new("RGB", (width, height), bg_color)
    draw = ImageDraw.Draw(img)

    font_title = _get_font(72)
    font_subtitle = _get_font(26)
    font_author = _get_font(14)
    font_ornament = _get_font(64)

    title_lines = ["Spine", "& Anchor"]
    title_y = 140
    for line in title_lines:
        _draw_text_centered(draw, title_y, line, font_title, text_color, width)
        title_y += 90

    ornament_y = title_y + 10
    _draw_text_centered(draw, ornament_y, "*", font_ornament, text_color, width)

    subtitle_y = ornament_y + 72
    _draw_text_centered(draw, subtitle_y, "A Nautical Misadventure",
                        font_subtitle, text_color, width)

    _draw_text_centered(draw, height - 70, "CROSSPOINT TEST FIXTURES",
                        font_author, text_color, width)

    buf = io.BytesIO()
    img.save(buf, "JPEG", quality=90)
    return buf.getvalue()


# ---------------------------------------------------------------------------
#  Book metadata
# ---------------------------------------------------------------------------

BOOK_UUID = str(uuid.uuid5(uuid.NAMESPACE_URL, "crosspoint:test:spine-anchor"))
TITLE = "Spine &amp; Anchor: A Nautical Misadventure"
AUTHOR = "Crosspoint Test Fixtures"
DATE = datetime.now().strftime("%Y-%m-%d")

# ---------------------------------------------------------------------------
#  FRONTMATTER — three TOC anchors in one spine item
# ---------------------------------------------------------------------------

FRONTMATTER = """\
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="en" lang="en">
<head><title>Front Matter</title>
<link rel="stylesheet" type="text/css" href="style.css"/></head>
<body>

<h2 id="dedication">Dedication</h2>

<p>For every reader whose bookmark landed on the wrong page, and every
navigator whose chart placed the lighthouse on the wrong shore.</p>

<h2 id="epigraph">Epigraph</h2>

<blockquote>
<p>&#x201C;A captain who trusts a single table of contents has never
sailed past the edge of a spine.&#x201D;</p>
<p>&#x2014; Admiral Fragmenta, <i>On the Perils of Pagination</i></p>
</blockquote>

<h2 id="foreword">Foreword</h2>

<p>The document you hold in your hands &#x2014; or rather, the document
your e-reader is attempting to reassemble from its constituent
parts &#x2014; is deliberately, almost aggressively, tangled.</p>

<p>Some chapters sprawl across multiple files. Others cram several
table-of-contents entries into a single page. At least one section
appears in the reading order yet refuses to show up in the table of
contents at all. And if you look carefully, you will find a table of
contents entry that points not to the beginning of its file, but to a
spot squarely in the middle.</p>

<p>This is all by design. If your reader survives, it can survive
anything.</p>

</body>
</html>
"""

# ---------------------------------------------------------------------------
#  CHAPTER 1 — normal 1:1 spine-to-TOC (baseline)
# ---------------------------------------------------------------------------

CHAPTER_1 = """\
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="en" lang="en">
<head><title>Chapter 1</title>
<link rel="stylesheet" type="text/css" href="style.css"/></head>
<body>

<h1>Chapter 1<br/>Setting Sail</h1>

<p>Anchora had packed three things for the voyage: a sextant with a
cracked lens, a notebook whose pages were already curling from salt air,
and an unshakeable conviction that every archipelago deserved a proper
atlas.</p>

<p>The sextant had belonged to her grandmother, a woman who had charted
the entire Leeward Passage in an open dinghy using nothing but dead
reckoning and a vocabulary of profanity that could strip barnacles at
forty paces. The crack in the lens was the result of an encounter with
a boom during an unexpected jibe &#x2014; not Anchora&#x2019;s
grandmother&#x2019;s boom, but rather the boom of a racing yacht that
had cut across her bow in the harbor approaches at a speed the
grandmother had described, with characteristic understatement, as
&#x201C;imprudent.&#x201D;</p>

<p>The notebook was new, purchased from a stationer in the port town
who specialized in paper for maritime use. It was bound in oilskin and
stitched with waxed thread, and the pages were a heavy cream stock that
the stationer claimed would resist salt spray, coffee spills, and the
tears of frustrated navigators in roughly equal measure. Anchora had
tested the first two claims. She hoped not to test the third.</p>

<p>&#x201C;The trouble with charts,&#x201D; she told the harbour-master
as he untied her bowline, &#x201C;is that they assume the world sits
still long enough to be drawn.&#x201D;</p>

<p>The harbour-master, whose name was Gideon and whose experience of
the sea was limited to watching it from the end of the municipal pier,
nodded politely and threw her the rope. He had heard variations of this
sentiment from every cartographer, surveyor, and marine geologist who
had passed through his harbour in thirty-one years of service, and he
had learned that the most efficient response was agreement.</p>

<p>&#x201C;Tides shift, sandbars migrate, and new rocks appear where
no rocks have any business being,&#x201D; Anchora continued, warming to
her theme. She was coiling the bowline with the automatic precision of
someone who has coiled ten thousand ropes. &#x201C;By the time you
publish an atlas, half the coastline has wandered off. You might as well
try to draw a portrait of a cloud.&#x201D;</p>

<p>&#x201C;And yet you keep drawing them,&#x201D; Gideon observed.</p>

<p>&#x201C;And yet I keep drawing them,&#x201D; Anchora agreed. She
stowed the coiled rope in the lazarette and checked the halyards. The
main was neatly furled on the boom; the jib was hanked on and ready.
Everything was in order. Everything was always in order aboard the
<i>Pagination</i>. Anchora tolerated chaos in the natural world because
she had no choice, but she would not tolerate it aboard her own boat.</p>

<p>The <i>Pagination</i> was a twenty-eight-foot sloop of a design that
had been fashionable forty years ago and was now merely practical, which
suited Anchora considerably better. She had a long waterline, a shallow
keel for poking into the kind of anchorages that deeper boats could only
look at wistfully, and a rig that one person could handle in all but
the most theatrical weather. Her hull was white, her boot-stripe was
navy, and her name was painted across the transom in gold leaf that was
beginning, after twelve seasons, to develop the kind of distinguished
patina that a less generous observer might have called peeling.</p>

<p>Anchora had bought her for a sum that the previous owner had
described as &#x201C;a bargain&#x201D; and that Anchora&#x2019;s bank
manager had described as &#x201C;concerning.&#x201D; That had been eight
years ago. In the intervening time, the <i>Pagination</i> had carried
Anchora to thirty-seven islands, through four named storms, across two
time zones, and into one regrettable encounter with a shipping container
that had fallen off a freighter in heavy weather and was drifting,
unmarked, at exactly the height of a sloop&#x2019;s waterline. The
encounter had cost Anchora a starboard stanchion and three weeks in a
boatyard. The container, so far as she knew, was still drifting.</p>

<p>She cast off the stern line and let the <i>Pagination</i> drift clear
of the dock. The wind was from the northwest at eight knots, which was
enough to move but not enough to excite. She unfurled the jib, sheeted
it in, and felt the boat come alive beneath her feet &#x2014; that small,
particular pleasure of a hull finding its groove in the water.</p>

<p>The harbour fell away behind her: the stone breakwater, the red and
green channel markers, the row of chandleries and fish restaurants that
constituted the town&#x2019;s entire economy. Gideon was still standing
on the dock, growing smaller. He raised a hand. Anchora raised hers in
return.</p>

<p>Beyond the breakwater, the sea opened up. It was mid-morning and the
light was the flat, even grey of an overcast day &#x2014; not dramatic,
not beautiful, but excellent for navigation. Dramatic light made for
good paintings but unreliable bearings. Anchora preferred her horizons
unambiguous.</p>

<p>She unrolled the chart she had prepared the night before and pinned
it to the table in the companionway. It was mostly blank. A few
coastlines sketched from Admiralty data. A scattering of depth soundings
copied from a survey that was either thirty or sixty years old,
depending on which edition you believed. A compass rose in the corner,
drawn with the care of someone who understood that a compass rose is
both a tool and a promise.</p>

<p>The rest was empty space. White paper waiting for ink. Terra
incognita, or rather aqua incognita, which sounded less impressive in
Latin but was considerably more relevant to someone in a boat.</p>

<p>She spread a fresh sheet of vellum on the chart table, uncapped her
pen, and wrote at the top in careful block letters: <b>ATLAS OF THE
UNCHARTED REACH</b>.</p>

<p>Below this she added, in smaller letters: <i>Compiled from original
observations by A. Vellum, Master Cartographer, aboard the sloop
Pagination.</i> This was perhaps optimistic &#x2014; the atlas currently
contained no observations at all &#x2014; but Anchora believed in
stating one&#x2019;s intentions clearly. A blank atlas was not an
admission of ignorance; it was a declaration of ambition.</p>

<p>She set the pen down, took a bearing on the harbour entrance &#x2014;
now two miles astern and shrinking &#x2014; and plotted her first
position fix on the chart. A small cross, precisely drawn, with the time
noted beside it: <i>09:17, departure.</i></p>

<p>The first mark on a new chart. It never got old.</p>

<p>She adjusted course to the southeast, trimmed the jib, and settled
into the cockpit with her notebook on her knee. The <i>Pagination</i>
heeled gently to port and began to make way. The wake streamed out
behind in a narrow V, the only mark she would leave on the water. By
evening it would be gone. By tomorrow the sea would have no memory of
her passage at all.</p>

<p>That was the fundamental asymmetry of cartography. The cartographer
remembers the sea; the sea does not remember the cartographer. You
could spend a lifetime recording every depth, every current, every
contour of a coastline, and the ocean would regard you with precisely
the same indifference it had shown the first person who ever put to
sea on a log.</p>

<p>Anchora found this oddly comforting. It meant the work was never
finished. It meant there was always another chart to draw.</p>

<p>She uncapped her pen again and began to sketch the headland that was
passing to starboard: a blunt promontory of dark rock, topped with
scrub grass and a navigation light that blinked every four seconds. She
noted the light&#x2019;s characteristics in the margin &#x2014;
<i>Fl(1) 4s, 12m, 8M</i> &#x2014; and added a small annotation about
the rocks that extended from the headland&#x2019;s base in a series of
jagged shelves. These rocks did not appear on the Admiralty chart. They
were, she suspected, a relatively recent arrival, deposited by a
landslip that no one had bothered to report to the hydrographic
office.</p>

<p>This was exactly the sort of thing that justified the voyage. The
official charts were not wrong, exactly. They were merely incomplete.
And in navigation, incomplete and wrong amounted to the same thing
when your keel was the instrument of discovery.</p>

<p>By noon, the headland was well astern and the coast had receded to
a thin grey line on the port beam. Anchora made a sandwich from the
provisions she had stowed in the ice-box &#x2014; hard cheese, cured
ham, and bread that would be stale by tomorrow &#x2014; and ate it in
the cockpit with one hand on the tiller. The wind was backing toward
the west and strengthening. She would need to reef before evening.</p>

<p>She washed the sandwich down with coffee from the thermos, plotted
another position fix, and wrote in her notebook: <i>Day 1, 12:30.
Twenty miles offshore. Wind NW 12 kn, backing. Seas moderate. One
headland charted, with previously unreported rocks. Atlas begun.</i></p>

<p>She had no idea how uncharted things were about to become.</p>

</body>
</html>
"""

# ---------------------------------------------------------------------------
#  CHAPTER 2 — one TOC entry, TWO spine items
# ---------------------------------------------------------------------------

CHAPTER_2_PART1 = """\
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="en" lang="en">
<head><title>Chapter 2 (Part 1)</title>
<link rel="stylesheet" type="text/css" href="style.css"/></head>
<body>

<h1>Chapter 2<br/>The Twin Harbors</h1>

<p>The first landmark on Anchora&#x2019;s route was a pair of harbors so
close together that early cartographers had drawn them as one. Later
cartographers, attempting to correct the error, had split the entry in
two &#x2014; but neglected to update the name on the second sheet. The
result was a pair of harbors that existed on some charts as a single
entity and on others as two separate places with the same name,
separated by a narrow cut of water that was either an inlet, a channel,
or &#x201C;the bit in the middle,&#x201D; depending on whom you
asked.</p>

<p>Anchora first spotted the harbors from three miles out. They
presented themselves as a low, dark line of wharves and warehouses,
punctuated by the vertical strokes of mast-heads and the occasional
plume of woodsmoke from a chandler&#x2019;s stove. Behind the
waterfront, the town rose in terraces: whitewashed houses with terracotta
roofs, a church tower, and what appeared to be either a lighthouse or a
very ambitious chimney.</p>

<p>She consulted her chart. According to the Admiralty edition, this was
Port Gemini &#x2014; a single harbor with a single entrance. According
to the pilot book, it was East Harbor and West Harbor, two separate
harbors sharing a common breakwater. According to the local fishing
cooperative&#x2019;s newsletter, which Anchora had found pinned to a
notice board during her last provisioning stop, it was &#x201C;the
Twins,&#x201D; and had been called that for longer than anyone could
remember, which is to say at least forty years.</p>

<p>A pilot boat emerged from behind the breakwater as Anchora rounded
the outer buoy. It was a stubby craft, painted orange, with a cabin
that looked like it had been designed by someone who believed windows
were a sign of weakness. A man stood on the foredeck, holding a coiled
line and wearing an expression of professional neutrality.</p>

<p>&#x201C;So which is East Harbor and which is West?&#x201D; Anchora
called across the narrowing gap.</p>

<p>&#x201C;Depends which chart you&#x2019;re reading,&#x201D; the pilot
replied cheerfully. &#x201C;On mine, you&#x2019;re entering East. On
the harbourmaster&#x2019;s, this is still West. We gave up arguing about
it years ago.&#x201D;</p>

<p>&#x201C;And the Admiralty?&#x201D;</p>

<p>&#x201C;The Admiralty says there&#x2019;s only one harbor, which will
come as a surprise to the three hundred boats moored in the other one.
We sent them a letter about it. Twice. They sent back a form asking us
to confirm the coordinates of the harbor we were claiming didn&#x2019;t
exist, which rather missed the point.&#x201D;</p>

<p>The pilot threw his line to Anchora, who caught it, made it fast to
the midship cleat, and allowed herself to be towed through the entrance
at a stately two knots. The breakwater slid past: massive blocks of
granite, green with weed below the tide line, topped with a rusting
iron railing and a row of bollards that had seen better centuries.</p>

<p>Inside the breakwater, the harbor opened up. It was larger than
Anchora had expected &#x2014; a broad basin ringed with pontoons,
moorings, and the kind of stone quay walls that speak of an era when
civic infrastructure was built to impress rather than merely to
function. Fishing boats occupied most of the moorings: trawlers with
rust-streaked hulls, lobster boats bristling with pots, and the
occasional sleek yacht that looked profoundly uncomfortable in such
working company.</p>

<p>&#x201C;This is East Harbor, then?&#x201D; Anchora asked as the
pilot pointed her toward an empty berth on the visitors&#x2019;
pontoon.</p>

<p>&#x201C;If you like,&#x201D; the pilot said agreeably. &#x201C;The
post office calls it East. The tax office calls it West. The pub calls
it &#x2018;here.&#x2019; I find that last one the most accurate.&#x201D;
</p>

<p>Anchora secured the <i>Pagination</i> to the pontoon with a bowline
fore and aft, adjusted the fenders, and stepped ashore. The pontoon
swayed under her feet in a way that she found pleasantly familiar. Solid
ground, by contrast, always felt slightly unreliable.</p>

<p>She spent the morning exploring the first harbor on foot, making
notes and sketches. The quay wall was three hundred and twelve paces
long &#x2014; she measured it, because that was the kind of person she
was. The depth alongside ranged from two to four metres, depending on
the state of the tide, which was semi-diurnal with a range of about two
metres. There were fourteen pontoons, each capable of berthing eight to
ten boats. The harbor entrance was fifty metres wide, oriented to the
southwest, and partially sheltered by the breakwater from the prevailing
swell.</p>

<p>All of this she recorded in her notebook, then transferred to her
chart in the careful, precise hand that was her professional signature.
She drew the quay walls in solid black lines, the pontoons in dashed
lines, the depth contours in fine blue, and added a small compass rose
in the corner of the inset. It was satisfying work. By lunchtime she
had a complete survey of what she was, for the sake of her sanity,
calling East Harbor.</p>

<p>In the afternoon she walked through the narrow cut that connected the
two basins. It was barely thirty metres wide &#x2014; a slot in the
rock through which the tide funneled with surprising force. The walls
of the cut were sheer, rising six or seven metres above the waterline,
and covered with the layered evidence of centuries of marine growth:
barnacles, mussels, a fur of green algae, and the occasional optimistic
sea anemone.</p>

<p>Anchora recorded the discrepancy in her notebook &#x2014; one harbor
or two? &#x2014; and sailed the <i>Pagination</i> through the narrow
cut under engine. The water changed from grey-green to a deep cobalt as
the bottom dropped away. The echo of the engine bounced off the cut
walls and returned with a slight delay, as though the passage were
thinking about what it had heard before deciding to repeat it.</p>

</body>
</html>
"""

CHAPTER_2_PART2 = """\
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="en" lang="en">
<head><title>Chapter 2 (Part 2)</title>
<link rel="stylesheet" type="text/css" href="style.css"/></head>
<body>

<p>On the far side of the cut, the second harbor opened like a cupped
hand. Fishing boats bobbed in neat rows, their hulls painted in fading
primaries: cadmium red, cerulean blue, chrome yellow. This harbor was
slightly smaller than the first but considerably busier: a fish market
was in full cry along the north quay, and the air smelled of brine,
diesel, and the particular sharp tang of fresh-caught mackerel.</p>

<p>Anchora nosed the <i>Pagination</i> into a gap between two lobster
boats whose skippers were engaged in a conversation conducted entirely
in hand gestures and appeared to concern the ownership of a crate of
bait. She made fast, fore and aft, and went ashore with her notebook.</p>

<p>The second harbor was, if anything, more confusing than the first.
It had its own harbourmaster &#x2014; a different one from the East
Harbor harbourmaster, which suggested a degree of administrative
independence that was hard to reconcile with the Admiralty&#x2019;s
insistence that this was all one place. It also had its own set of
charts, posted on a notice board outside the harbourmaster&#x2019;s
office, and these charts were not merely different from the Admiralty
charts; they were different from the charts posted in the East
Harbor office.</p>

<p>&#x201C;We draw our own,&#x201D; the West Harbor harbourmaster
explained when Anchora asked about this. Her name was Constance and she
had the weathered complexion and steady gaze of someone who had spent
forty years watching boats do foolish things. &#x201C;The Admiralty
charts are&#x2026; aspirational. We prefer something grounded in what
we can actually see.&#x201D;</p>

<p>&#x201C;And East Harbor?&#x201D;</p>

<p>&#x201C;They draw their own as well. Different from ours, naturally.
We disagree about the depth in the cut &#x2014; they say three metres at
low water, we say two and a half. We also disagree about the location
of the inner reef &#x2014; they put it forty metres south of where we
put it. We have been disagreeing about this for, I believe, about
seventy years.&#x201D;</p>

<p>&#x201C;Has anyone thought of measuring?&#x201D; Anchora
ventured.</p>

<p>Constance gave her a look that suggested this question had been asked
before and that the answer was both yes and complicated.</p>

<p>&#x201C;Three times,&#x201D; Constance said. &#x201C;The first
survey found three metres. The second found two and a half. The third
found two point seven but was conducted by the East Harbor
harbourmaster&#x2019;s nephew and is therefore inadmissible.&#x201D;</p>

<p>Anchora spent two days surveying the second harbor with the same
methodical care she had applied to the first. She measured the quay
walls, sounded the depths, timed the tides, and charted the moorings.
She also, because she was thorough, measured the cut from both ends and
found it to be two point six metres at low water springs, which
satisfied nobody but at least had the virtue of being her own
measurement.</p>

<p>On the second evening, as the sun dropped below the breakwater and
turned the water in the harbor to molten copper, the pilot boat came
alongside the <i>Pagination</i>. The pilot &#x2014; whose name, Anchora
had learned, was Rufus &#x2014; was sitting on the gunwale eating an
apple.</p>

<p>&#x201C;You&#x2019;ll want to note,&#x201D; he called across,
&#x201C;that neither harbor appears on the Admiralty chart. Officially,
this is all open water.&#x201D;</p>

<p>&#x201C;I had noticed,&#x201D; Anchora said.</p>

<p>&#x201C;The chart shows a straight coastline where we&#x2019;re
sitting. No indentation, no breakwater, no harbors. According to the
Royal Hydrographic Office, we are currently bobbing about in the open
sea. I sometimes think about this when I&#x2019;m tying up to the
pontoon.&#x201D;</p>

<p>Anchora drew both harbors on a single sheet &#x2014; one chart, two
basins, no ambiguity &#x2014; and labeled it with coordinates she
trusted rather than names she did not. She drew the cut at its correct
width, marked the depth as 2.6 m LAT, and added the inner reef in the
position she had determined by triangulation from three fixed points on
shore. She used blue for the water, black for the structures, and a
particularly assertive shade of red for the reef, because reefs that
are subject to a seventy-year argument deserve to be drawn in a colour
that commands attention.</p>

<p>She pinned the completed chart to the wall above her berth, where
the varnished wood held brass tacks with the grip of long practice.
The chart looked good. Clean lines, clear labels, unambiguous depth
figures. For a moment &#x2014; a brief, perfect moment &#x2014; the
world felt like a place that could be made orderly.</p>

<p>She would not feel that way again for some time.</p>

</body>
</html>
"""

# ---------------------------------------------------------------------------
#  CHAPTER 3 — one spine item, FOUR TOC entries (chapter + 3 sub-sections)
# ---------------------------------------------------------------------------

CHAPTER_3 = """\
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="en" lang="en">
<head><title>Chapter 3</title>
<link rel="stylesheet" type="text/css" href="style.css"/></head>
<body>

<h1>Chapter 3<br/>Charting the Depths</h1>

<p>Beyond the Twin Harbors, the continental shelf dropped away in three
distinct terraces. Anchora&#x2019;s fathom-line, weighted with lead and
tallow, told the story in numbers: ten, forty, two hundred. Each number
represented a different world &#x2014; a different temperature, a
different colour, a different set of hazards &#x2014; and she intended
to chart them all.</p>

<p>She had sailed out of the second harbor at dawn, motoring through
the cut in the grey half-light while the fishing boats were still
sorting their nets. Constance had waved from the harbourmaster&#x2019;s
office window. Rufus had been asleep on his pilot boat and had not
waved at all.</p>

<p>Each terrace had its own character, its own light, its own hazards.
She gave each a name and &#x2014; because she was, above all, a
cartographer &#x2014; a sub-heading.</p>

<h2 id="the-reef">The Reef</h2>

<p>The first terrace was a broad shelf of coral, alive with colour and
peril in roughly equal measure. It extended for perhaps two miles from
the outer edge of the harbor approaches, a gradually shoaling platform
of limestone and living coral that the chart described as &#x201C;foul
ground&#x201D; and the local fishermen described with a word that
Anchora did not record in her notebook but committed to memory for
future use.</p>

<p>Staghorn formations reached toward the hull like bony fingers. Fan
corals swayed in the current with the slow grace of a metronome. Brain
corals squatted on the seabed like boulders, their surfaces etched with
grooves that looked, from above, like the contour lines on a
particularly complicated chart. Anchora appreciated the irony. Even
the ocean floor was trying to be a map.</p>

<p>The water over the reef was extraordinarily clear. She could see the
bottom in seven metres of water as though it were behind glass: every
coral head, every sandy patch, every dark crevice where a moray eel
might be contemplating its options. Fish moved through this landscape in
schools and squadrons, their colours too vivid for the grey day
&#x2014; electric blue, acid yellow, a hot pink that no chart
convention had a symbol for.</p>

<p>Anchora worked her way across the reef under reduced sail, taking
soundings every hundred metres. The depth varied unpredictably: three
metres here, seven metres there, and in one alarming spot, barely one
and a half metres over a coral head that lurked just below the surface
like a geological ambush. She marked each sounding on her chart with
the compulsive precision of someone who knows that a single missed
hazard can ruin a hull and a reputation in the same instant.</p>

<p>&#x201C;Beautiful,&#x201D; she murmured, leaning over the rail to
watch a school of parrotfish graze on the coral below. The fish were
vivid turquoise, the size of dinner plates, and they ate the coral with
an audible crunching sound that was both fascinating and faintly
disturbing. Then her keel scraped limestone &#x2014; a sound like a
giant clearing its throat &#x2014; and she revised her opinion to
&#x201C;beautiful but inconvenient.&#x201D;</p>

<p>She started the engine and motored off the coral head, checking the
bilge for leaks (none, thank goodness; the <i>Pagination</i>&#x2019;s
hull was tougher than it looked) and the keel for damage (a scrape,
nothing more, but a scrape that would need antifouling paint before
the season was out). The parrotfish watched her departure with the
sublime indifference of creatures that do not have keels.</p>

<p>By late morning she had completed her survey of the reef. It covered
an area of roughly four square miles and contained, by her count,
twenty-three significant coral heads within two metres of the surface
at low water. She inked the reef in hatched lines on her chart, shading
the shallowest areas in a warning blue, and adding a marginal note:
<i>Draft exceeding 1.5 m: proceed with caution and strong
language.</i></p>

<p>She added, after a moment&#x2019;s thought: <i>Draft exceeding 2.0 m:
do not proceed at all. Go around.</i></p>

<p>The reef chart was one of the most detailed pieces of work she had
produced. She allowed herself a moment of satisfaction, then turned her
attention to the second terrace.</p>

<h2 id="the-current">The Current</h2>

<p>Below the reef shelf, the seabed fell away sharply &#x2014; not
gradually, not gently, but with the decisiveness of a cliff edge. One
moment the depth sounder was reading eight metres; the next it was
reading forty, and still dropping. The colour of the water changed too,
from the pale turquoise of the reef to a deep, serious blue that
suggested the ocean was no longer in a playful mood.</p>

<p>And the water began to move. Not the slow, companionable drift of
tidal flow, not the rhythmic surge of swell, but a purposeful lateral
current that pushed the <i>Pagination</i> sideways at a rate Anchora
found personally offensive. She had been sailing a straight course
toward a landmark on the far shore; within ten minutes, the landmark
had migrated from dead ahead to thirty degrees off the starboard bow,
and the <i>Pagination</i> was crabbing sideways like a startled dog on
a polished floor.</p>

<p>Anchora dropped the jib, started the engine, and pointed the bow
directly into the current. The <i>Pagination</i> held position, but
only just. The engine was working at three-quarters throttle to make
zero progress over the ground, which was the nautical equivalent of
running on a treadmill.</p>

<p>She set a kedge anchor &#x2014; a small, light anchor attached to a
long line that she rowed out in the dinghy and dropped on the seabed
upstream of the current. With the boat held in place by the anchor, she
could take bearings without the distraction of being pushed steadily
toward a different postcode.</p>

<p>The boat swung in lazy arcs at the end of her anchor line, like a
weathervane that could not quite decide where the wind was coming from.
Anchora took bearings on three fixed points on shore every fifteen
minutes, plotted them on her chart, and calculated the current&#x2019;s
speed and direction from the resulting drift vectors.</p>

<p>The current, she calculated, ran at two and a quarter knots on the
ebb tide, swinging to three and a half on the flood. It flowed roughly
parallel to the coast, with a slight onshore component during the flood
that would push an unwary vessel toward the reef. At springs &#x2014;
when the tidal range was at its greatest &#x2014; she estimated the
current would exceed four knots, which was faster than many boats could
motor against and would certainly be faster than any sailboat could
point into.</p>

<p>It was the kind of data that looked innocent in a table and lethal on
a lee shore. Anchora had a healthy respect for currents. They were the
ocean&#x2019;s way of reminding you that it was bigger than you and had
somewhere to be.</p>

<p>She spent the rest of the day anchored in the current zone, taking
readings at half-hour intervals. By evening she had enough data to draw
a current chart: a pattern of arrows showing direction and speed at
various states of the tide, annotated with times referenced to high
water at the Twin Harbors. She added a caution box in the margin:
<i>Spring rates may exceed 4 kn. Passage against the flood inadvisable
for vessels under power of 15 hp. Passage under sail alone is a matter
for the individual conscience.</i></p>

<p>She noted it all down, adding small arrows to indicate direction and
strength. The chart was filling up nicely. She had coral, she had
current, and she had the beginnings of a bathymetric profile that would
make a hydrographer weep with something that might have been either
admiration or envy.</p>

<h2 id="the-depths">The Depths</h2>

<p>Past the current, the water turned from blue to black. It was a
gradual transition &#x2014; not a line, not a boundary, but a slow
deepening of colour that spoke of depths where sunlight was a rumour
and pressure was a fact of life. The <i>Pagination</i> sailed into
this darker water on a light breeze, and Anchora felt the temperature
drop a degree as the deep water exhaled its chill.</p>

<p>Anchora&#x2019;s lead-line ran out at two hundred fathoms and found
nothing. She tied on an extension &#x2014; fifty fathoms of spare line
she kept coiled in the lazarette for exactly this purpose. At three
hundred fathoms: nothing. The line hung straight down into the black
water, trembling slightly with the current, offering no information
except that the bottom was not yet here.</p>

<p>She tied on a second extension. At four hundred fathoms &#x2014;
nearly half a mile of line, its weight now enough to make her arms ache
from the hauling &#x2014; she felt a distant, uncertain bump. The lead
had found something. Whether it was bedrock, sediment, a drowned
mountain, or the roof of something she preferred not to think about,
the line could not tell her. It only knew that it had stopped going
down.</p>

<p>&#x201C;Well,&#x201D; she said to the empty cockpit, &#x201C;there
is apparently a bottom.&#x201D;</p>

<p>She hauled the line in, arm over arm, for what felt like an hour but
was probably fifteen minutes. The lead came up coated in a fine grey
clay that told her the bottom was soft sediment &#x2014; the
accumulated drift of millennia, particles of sand and silt and the
ground-down remains of creatures that had lived and died in the water
column above and settled, with infinite slowness, to the floor.</p>

<p>She examined the clay sample with the professional interest of
someone who understands that even mud has a story to tell. It was
fine-grained, slightly sticky, with no visible shell fragments or
organic material. Deep-water sediment. The kind of bottom that offers
poor holding for an anchor but excellent preservation for anything that
sinks to it.</p>

<p>She took three more soundings over the course of the afternoon,
sailing a line perpendicular to the coast. The depths were remarkably
consistent: three hundred and eighty fathoms, four hundred and ten,
three hundred and ninety-five. The seabed here was a flat plain,
featureless and unchanging, stretching into the darkness in every
direction.</p>

<p>She drew a single contour line on her chart &#x2014; the four-hundred
fathom line, traced with a steady hand despite the gentle rolling of
the boat &#x2014; and wrote <i>400+ fm</i> beside it. Below the line
she left blank space: the cartographer&#x2019;s admission that some
things are simply too deep to record, too far from light and air and
the concerns of surface-dwellers to justify the ink.</p>

<p>But she kept the clay sample, sealed in a small glass jar and
labeled with the date, position, and depth. A cartographer records
what can be recorded. The blank space on the chart was not ignorance.
It was honesty.</p>

</body>
</html>
"""

# ---------------------------------------------------------------------------
#  INTERLUDE — in spine, NOT in TOC
# ---------------------------------------------------------------------------

INTERLUDE = """\
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="en" lang="en">
<head><title>Interlude</title>
<link rel="stylesheet" type="text/css" href="style.css"/></head>
<body>

<h1>The Phantom Island</h1>

<p>Between Chapter Three and Chapter Four, Anchora sailed past an island
that did not exist.</p>

<p>She spotted it on the morning of her third day out from the Twin
Harbors, in a stretch of sea where her charts showed nothing but open
water and a scattering of depth soundings that ranged from
&#x201C;deep&#x201D; to &#x201C;very deep.&#x201D; It appeared first
as a dark smudge on the horizon, which she initially took for a low
cloud or a particularly ambitious wave. But clouds do not stay in one
place, and waves do not have palm trees, and this thing had both.</p>

<p>She altered course to investigate. A cartographer who sails past an
uncharted island without investigating is not, in Anchora&#x2019;s
professional opinion, a cartographer at all, but merely someone with a
boat and a collection of pens.</p>

<p>The island resolved itself as she approached. It was small &#x2014;
perhaps a quarter of a mile across, roughly circular, fringed with a
beach of white sand that was so bright in the morning sun it made her
squint. Behind the beach, a stand of coconut palms rose to a modest
height, their fronds rustling in a breeze that Anchora could not feel
from the water. At the island&#x2019;s centre, a low hill rose perhaps
fifty feet above sea level, covered in scrub grass and what appeared to
be a single, determined frangipani tree in full bloom.</p>

<p>It had, in short, all the attributes of an island. It had mass, it
had volume, it had geographic coordinates that Anchora carefully noted
from her sextant readings. It had a beach of white sand that squeaked
underfoot when she rowed the dinghy ashore and stepped out, which is a
very specific and tactile quality that imaginary places tend not to
possess.</p>

<p>It had a small but enthusiastic colony of penguins, which was
geographically improbable but zoologically undeniable. They were small
penguins &#x2014; about eighteen inches tall, with blue-grey backs and
white fronts and an air of having been expecting her. They stood in a
loose group near the tree line, regarding her with the intensity of
creatures who have opinions about visitors but lack the vocal apparatus
to express them clearly.</p>

<p>&#x201C;You&#x2019;re not supposed to be here,&#x201D; Anchora told
them. Penguins, she was fairly certain, belonged in the southern
hemisphere, on rocky coasts and ice shelves, in places where the water
was cold and the fish were plentiful and the nearest palm tree was
several thousand miles away. These penguins appeared not to have read
the relevant literature.</p>

<p>She walked the perimeter of the island, which took twenty-three
minutes at a brisk pace. The beach was continuous &#x2014; fine white
sand, no rocks, gently shelving into water that was a shade of turquoise
that postcards aspire to but rarely achieve. The interior was equally
unremarkable: the palm grove, the scrub hill, the frangipani, and an
absence of anything that might explain why this island was here. No
volcanic cone. No coral atoll. No geological justification
whatsoever.</p>

<p>She took careful measurements. She recorded the latitude and
longitude from three separate sextant shots, all of which agreed. She
sounded the water around the island&#x2019;s shore and found a uniform
depth of four fathoms, dropping sharply to twenty fathoms fifty metres
out. She noted the compass bearing to the nearest known landmark
&#x2014; the headland beyond the Twin Harbors, barely visible on the
horizon &#x2014; and calculated the distance.</p>

<p>All the data said the island was real. The numbers were precise and
consistent. The sand was tangible. The penguins were audible (they had
begun a low, conversational muttering among themselves that suggested
they were discussing her survey technique).</p>

<p>She reached for her pen to add it to the atlas, then paused.</p>

<p>The island was not in the table of contents. She checked &#x2014;
carefully, methodically, the way she checked everything. She ran her
finger down the list of chapters, sections, and appendices. The
Foreword was there. The five chapters were there. The appendices were
there. But between Chapter Three and Chapter Four, where this island
chronologically belonged, there was no entry. No heading, no
sub-heading, no footnote.</p>

<p>It was not listed in the index. It appeared in the reading order
&#x2014; she was, after all, reading about it right now, and so were
you &#x2014; but no navigation entry pointed to it. If you were using
the table of contents to move through this book, you would skip from
Chapter Three directly to Chapter Four and never know this island was
here. It existed in the spine but not in the navigation. It was
structurally present but navigationally invisible.</p>

<p>&#x201C;If a place has no entry,&#x201D; she asked one of the
penguins, &#x201C;does it really exist?&#x201D;</p>

<p>The penguin regarded her with the weary patience of a creature that
has heard this question before &#x2014; perhaps from other
cartographers who had stumbled upon the island and grappled with the
same ontological difficulty. It tilted its head to one side, blinked
once with each eye in sequence, and then waddled back to the surf with
the unhurried dignity of a creature whose existence does not depend on
being listed in a table of contents.</p>

<p>Anchora sat on the beach for a while, thinking about this. The
penguins went about their business around her: waddling, swimming,
standing in small groups and staring at the horizon with the focused
attention of creatures who are waiting for something but have forgotten
what. The waves lapped at the sand. The palm fronds rustled. The
frangipani released its scent into the warm air.</p>

<p>It was, she had to admit, a very pleasant island. Peaceful.
Unhurried. The kind of place where you could sit for an afternoon and
forget that you had charts to draw and depths to sound and harbors to
argue about. Perhaps that was the point. Perhaps some places are better
off uncharted &#x2014; hidden in the reading order, reachable only by
those patient enough to read every page rather than jumping from heading
to heading.</p>

<p>She considered this philosophical position for approximately four
minutes, then rejected it. She was a cartographer. Uncharted places
were not romantic; they were errors.</p>

<p>And yet.</p>

<p>She closed her notebook. She rowed back to the <i>Pagination</i>.
She hauled up the anchor, set the jib, and sailed away from the island
on a reaching course that would bring her to the coordinates she had
been given for the beginning of the Long Voyage.</p>

<p>She did not add the island to her chart. There was no heading for it
in the atlas, no entry in the navigation, no place for it in the
structure she had so carefully planned. An island without a table of
contents entry was, from a cartographic perspective, an orphan &#x2014;
and Anchora&#x2019;s atlas did not have room for orphans.</p>

<p>She did, however, add penguins to her growing list of things that
were not supposed to be there but were. The list, she reflected, was
getting longer with every day of the voyage. She was not sure whether
this said something about the world or about her list.</p>

<p>Behind her, the island shimmered in the heat haze and slowly sank
below the horizon, taking its penguins and its palm trees and its
existential questions with it. By evening it was gone. By morning she
would not be entirely sure she had seen it at all.</p>

<p>But the sand between her toes &#x2014; fine, white, faintly
squeaky &#x2014; suggested otherwise.</p>

</body>
</html>
"""

# ---------------------------------------------------------------------------
#  CHAPTER 4 — one TOC entry, THREE spine items
# ---------------------------------------------------------------------------

CHAPTER_4_PART1 = """\
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="en" lang="en">
<head><title>Chapter 4 (Day 1)</title>
<link rel="stylesheet" type="text/css" href="style.css"/></head>
<body>

<h1>Chapter 4<br/>The Long Voyage</h1>

<h2>Day One &#x2014; Departure</h2>

<p>The passage to the outer archipelago was, by all accounts, a
three-day sail. This estimate came from Rufus the pilot, who had never
made the passage himself but had spoken to several people who had, and
from Constance the harbourmaster, who had made it once, thirty years
ago, in a boat considerably faster than the <i>Pagination</i> and in
weather considerably better than the forecast was promising.</p>

<p>Anchora provisioned accordingly: three days of hard-tack, three days
of tinned sardines, and three days of the sort of instant coffee that
dissolves under protest. She also packed a fourth day&#x2019;s supplies,
because she had learned through experience that three-day passages have
a way of becoming four-day passages, and a sailor without coffee on
day four is a sailor without hope.</p>

<p>She reviewed her charts one more time before casting off. The outer
archipelago was marked on the Admiralty chart as a cluster of small
islands approximately seventy miles to the southeast &#x2014; close
enough to reach in three days of moderate sailing, far enough to
require planning, provisioning, and the acceptance that the nearest
help, should help be needed, was a day&#x2019;s sail behind her. The
islands themselves were drawn as rough outlines, their coastlines
sketched from satellite imagery and the reports of passing ships. No
one had surveyed them properly. That was why Anchora was going.</p>

<p>She cast off at first light, motoring through the harbor and out
past the breakwater into open water. The dawn was a thin line of amber
on the eastern horizon, and the sea was a flat expanse of grey that
merged, at the edges, with a sky of precisely the same shade. The wind
was light from the southeast &#x2014; a soldier&#x2019;s wind, steady
and reliable, the kind of wind that does not require constant attention
to the sheets.</p>

<p>Day one was, in the main, uneventful. The wind held steady, the seas
were moderate &#x2014; a long, low swell from the south that the
<i>Pagination</i> rode with an easy, rocking motion &#x2014; and the
boat made six knots on a beam reach without complaint. This was her
best point of sail: the wind on the beam, the sails drawing well, the
hull slicing through the water with the minimum of fuss and the maximum
of speed.</p>

<p>Anchora spent the daylight hours sketching coastline profiles from
the deck. The coast was still visible to the northwest, a low grey line
that rose occasionally into headlands and fell back into bays. She
drew each headland as she passed it, noting its shape, its height, its
distinguishing features. A lighthouse here. A radio mast there. A cliff
face that had collapsed into a jumble of rocks that would, she
suspected, not appear on any chart for another decade.</p>

<p>By mid-morning the coast had begun to recede, and by early
afternoon it was little more than a suggestion &#x2014; a slight
thickening of the horizon, a shade of grey that was fractionally darker
than the grey of the sea. Anchora took a last bearing on the highest
headland, plotted it on her chart, and acknowledged that she was, for
practical purposes, out of sight of land.</p>

<p>This was a moment that some sailors dreaded and others relished.
Anchora was in the second category. Out of sight of land, the world
simplified itself. There was the boat, there was the sea, and there was
the compass. Everything else &#x2014; the harbors and their arguments,
the charts and their disagreements, the penguins and their inexplicable
geography &#x2014; fell away. What remained was navigation in its purest
form: a vessel, a heading, and the conviction that the maths would
work out.</p>

<p>She spent the evening hours transferring her coastline sketches to
the master chart, working by the light of the oil lamp that swung from
a hook above the chart table. The lamp threw a warm, yellow circle of
light that moved gently with the boat&#x2019;s motion, and the shadows
it cast gave the chart a depth and texture that the flat light of day
did not. She added her soundings from the reef and current zones, drew
the four-hundred-fathom contour line she had found over the deep water,
and penciled in a tentative route to the archipelago.</p>

<p>By sunset, the Twin Harbors had sunk below the horizon and there was
nothing in any direction but water and the thin, bright line where it
met the sky. The sky turned from grey to amber to a deep blue-violet
that held the first stars in its upper reaches. The wind eased slightly
as the land breeze died and the sea breeze had not yet arrived. The
<i>Pagination</i> slowed to four knots, then three, and the water
around her hull changed from a hiss to a whisper.</p>

<p>Anchora set the self-steering gear &#x2014; a wind-vane device of
her own construction that held the boat on course without human
intervention, provided the wind did not shift by more than fifteen
degrees, which it did roughly once an hour &#x2014; and went below to
make dinner. Tinned sardines on hard-tack, with a cup of instant coffee
that tasted, as always, of ambition unfulfilled.</p>

<p>She marked her position on the chart with a small cross and the
time: <i>Day 1, 18:47, all well. 42 miles made good. Barometer steady.
Wind SE 8 kn. Coffee: adequate.</i></p>

</body>
</html>
"""

CHAPTER_4_PART2 = """\
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="en" lang="en">
<head><title>Chapter 4 (Day 2)</title>
<link rel="stylesheet" type="text/css" href="style.css"/></head>
<body>

<h2>Day Two &#x2014; Becalmed</h2>

<p>The wind died at dawn and did not return. It did not die gradually,
the way winds usually die &#x2014; fading through a series of
diminishing puffs until the last breath of air gives up and the sails
go slack. It died all at once, as though someone had closed a door.
One moment the <i>Pagination</i> was sailing; the next she was not.</p>

<p>The <i>Pagination</i> sat on water so flat it reflected the clouds
like a tray of mercury. Every ripple, every swell, every hint of motion
had been smoothed away. The sea was a mirror. The sky was reflected in
it so perfectly that the horizon vanished, and Anchora had the
unsettling sensation of floating in the centre of a sphere, with
identical views in every direction, up and down included.</p>

<p>The sails hung in dispirited loops. The main sagged against the
shrouds. The jib, unfurled and hopeful an hour ago, now hung from the
forestay like a bedsheet on a clothesline. Even the tell-tales &#x2014;
small strips of yarn attached to the shrouds to indicate wind direction
&#x2014; drooped vertically, pointing at the deck with the resigned
finality of arrows that have given up looking for a target.</p>

<p>Anchora tried whistling for wind (maritime tradition). She stood in
the cockpit and whistled a jaunty tune toward each of the four cardinal
points in turn, which was supposed to summon a breeze from the
direction you whistled at. The tradition did not specify what to do when
all four directions were equally windless. Anchora whistled at all of
them anyway, on the principle that comprehensive failure was preferable
to targeted failure.</p>

<p>She tried tapping the barometer (maritime superstition). The
barometer was a fine brass instrument mounted on the bulkhead in the
companionway, and it showed 1024 millibars, which was high and stable
and thoroughly uninterested in change. Tapping it &#x2014; a gentle,
respectful tap with the knuckle of the index finger, as tradition
demanded &#x2014; produced no change. She tapped it again, harder.
Still nothing. The barometer regarded her with the mechanical smugness
of a device that knows it is right.</p>

<p>She tried simply sitting in the cockpit and staring at the horizon
(maritime realism). This was, in many ways, the most honest response to
a calm: the acceptance that the wind would return when it chose and not
before, and that no amount of whistling, tapping, or staring would
hasten its arrival. Sailors who raged against calms accomplished nothing
except to make themselves hot and irritable. Sailors who accepted them
at least stayed cool.</p>

<p>The morning passed in a haze of heat and stillness. Anchora read a
chapter of the pilot book, made a cup of coffee (the second-to-last
packet of instant &#x2014; she was rationing now), and wrote a long
entry in her notebook about the geology of the continental shelf,
drawing on the soundings she had taken over the past few days. The
writing helped. It gave her hands something to do and her mind something
to chew on that was not the infuriating absence of wind.</p>

<p>By noon the heat was ferocious. The sun stood directly overhead,
pouring its energy into the flat sea and the flat boat and the flat
cartographer who was beginning to feel like a piece of hard-tack left
on a windowsill. The deck was too hot to touch barefoot. The cabin was
an oven. The cockpit, shaded by the boom, was merely oppressive rather
than unbearable, which was the best that could be said for it.</p>

<p>She rigged a sun-shade from the spare jib, draping the heavy
sailcloth over the boom and tying it to the shrouds on either side.
This created a rectangle of shade over the cockpit that was eight feet
by six feet and reduced the temperature from &#x201C;punitive&#x201D;
to &#x201C;merely unpleasant.&#x201D; She sat in this shade and spent
the afternoon measuring the depth with her lead-line, which was at
least productive even if it was not comfortable.</p>

<p>The bottom here was sixty fathoms of soft mud &#x2014; the same
fine grey clay she had found in the deep water, but closer to the
surface. The mud yielded to the lead with a reluctant sucking sound
that Anchora could feel through the line, a hundred and twenty metres
away. She noted this on the chart, adding a small sounding figure at
her current position, though she suspected no one would ever care.
Sixty fathoms of mud in the middle of nowhere was not the kind of
information that made it into the Admiralty Notices to Mariners.</p>

<p>She took several more soundings as the afternoon wore on, drifting
imperceptibly &#x2014; the current was weak here, barely a quarter of
a knot &#x2014; and recording each depth. Fifty-eight fathoms.
Sixty-two. Fifty-five. The seabed was gently undulating, rising and
falling by a few fathoms over a distance of perhaps half a mile. She
drew these contours on her chart, and the result looked like the gentle
hills of a drowned landscape, which is precisely what it was.</p>

<p>At sunset, a catspaw rippled the surface to the north &#x2014; a
small, dark patch of ruffled water that raced across the mirror-flat
sea like a whispered secret. Anchora leaped to her feet, freed the
jib sheet, and hauled in the main, her hands moving with the desperate
speed of someone who has been waiting all day for this exact moment.
By the time she had the sails trimmed, the catspaw had arrived,
touched the <i>Pagination</i>&#x2019;s sails with the lightest of
fingers, and moved on. The sails filled for a moment &#x2014; a
single, heartbreaking moment &#x2014; and then fell slack again.</p>

<p>The catspaw raced on to the south and disappeared. The sea
returned to its mirror state. The tell-tales drooped.</p>

<p>Anchora sat down in the cockpit and regarded the empty horizon with
an expression that, in a less disciplined cartographer, might have been
called despair. In Anchora it was merely a very thorough species of
disappointment.</p>

<p>She marked her position: <i>Day 2, 19:02, becalmed. 3 miles made
good by drift. Barometer 1024, steady. Wind: none. Morale stable but
coffee supply critical. Lead-line soundings taken; seabed contour added
to chart. If the wind does not return by morning I shall motor, which
is an admission of defeat but at least it is defeat with forward
progress.</i></p>

</body>
</html>
"""

CHAPTER_4_PART3 = """\
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="en" lang="en">
<head><title>Chapter 4 (Day 3)</title>
<link rel="stylesheet" type="text/css" href="style.css"/></head>
<body>

<h2>Day Three &#x2014; Arrival</h2>

<p>The wind returned overnight with the subtlety of a cannon shot. One
moment the <i>Pagination</i> was drifting, her hull barely creasing the
water, her sails hanging like theatre curtains after the final act. The
next moment she was heeled over at twenty degrees, spray flying from the
bow in white sheets, the rigging singing a chord in B-flat minor that
rose in pitch as the gusts built and fell again as they passed.</p>

<p>Anchora, who had been asleep in the quarterberth with her head on a
folded chart and her feet on a coil of rope, arrived on deck wearing
one boot and an expression of startled competence. The wind was from
the northwest &#x2014; exactly the direction she needed &#x2014; and
blowing at what she estimated was twenty knots, gusting twenty-five.
This was more wind than the <i>Pagination</i> wanted with full sail
set, and considerably more wind than Anchora wanted at three o&#x2019;clock
in the morning with one boot on.</p>

<p>She reefed the main in the dark, working by feel and muscle memory.
The reef points were where her fingers expected them to be; the lines
ran through the blocks without snagging; the sail came down to its
first reef in under two minutes. She trimmed the jib to match, easing
the sheet until the sail stopped flogging and settled into the shape it
wanted, and pointed the bow toward the coordinates she had been given
for the outer archipelago.</p>

<p>The <i>Pagination</i> responded immediately. With the reef in, she
was balanced, manageable, and making seven knots through the water
&#x2014; her best speed of the voyage. The bow wave hissed along the
hull. The wake stretched out behind in a long, white tail. The self-
steering gear held the course without complaint, and Anchora, who was
now fully awake and beginning to enjoy herself, went below to find the
other boot.</p>

<p>Dawn came slowly, the way it does at sea: a gradual lightening of
the eastern sky, from black to charcoal to a pale grey that spread
upward like water rising in a glass. The horizon emerged from the
darkness, sharp and clean, and Anchora scanned it with the practiced
eye of someone who has been looking for land from the deck of a small
boat for most of her adult life.</p>

<p>Nothing. Not yet. But the chart said the archipelago should be
visible by mid-morning at this speed, and Anchora trusted her chart
because she had drawn it herself and she knew exactly how much trust
it deserved, which was a moderate amount tempered by professional
humility.</p>

<p>She made coffee &#x2014; the last packet, which she regarded as both
a sacrifice and an incentive &#x2014; and drank it in the cockpit while
the <i>Pagination</i> sailed herself. The wind was settling down to a
steady eighteen knots, the gusts becoming less frequent, the sea
developing a regular pattern of waves that the boat cut through with
a satisfying, rhythmic motion. This was good sailing. The kind of
sailing you remember long after the voyage is over.</p>

<p>At ten o&#x2019;clock she saw something on the horizon that was not
cloud and not sea. It was a faint, dark irregularity in the line
where sky met water &#x2014; a slight bump, barely perceptible, that
could have been a distant ship or a trick of the light but wasn&#x2019;t.
She knew what it was. She had been looking for it for three days.</p>

<p>Land appeared at noon: a low, dark smudge that resolved slowly into
individual islands, then individual trees, then individual birds
perched in the individual trees. She counted seven major islands and
an uncountable number of rocks, shoals, and ambiguous features that
might have been either. The islands were spread across perhaps ten miles
of sea, arranged in a rough arc that opened toward the northwest
&#x2014; toward her, as it happened, which felt like a welcome even
though she knew it was merely geography.</p>

<p>She sailed closer, reducing speed as the water shallowed. The
depth sounder, which had been reading &#x201C;deep&#x201D; for two
days, suddenly began producing numbers: 80 metres, 60, 40, 30. She
could see the bottom now &#x2014; a sandy seabed dappled with shadows
from the islands above. Fish scattered as the <i>Pagination</i>&#x2019;s
shadow passed over them.</p>

<p>She rounded the northern tip of the largest island, staying well
clear of a line of rocks that extended from the shore like a broken
jetty, and found herself in a sheltered anchorage on the island&#x2019;s
lee side. The water was calm here, protected from the northwest wind
by the island&#x2019;s bulk. She dropped anchor in four fathoms of
sand, felt it bite, and paid out enough chain to hold in a gale.</p>

<p>Then she sat in the cockpit and looked at the islands spread before
her. Seven islands. Uncharted, unsurveyed, waiting. This was why she
had come. Not for the harbors with their naming disputes, not for the
reefs with their lurking coral heads, not for the currents with their
treacherous pull. She had come for this: blank space on the chart,
waiting to be filled.</p>

<p>She uncapped her pen. This, at last, was what she had come for.</p>

<p>She marked her position one final time: <i>Day 3, 12:15, landfall.
Anchored in the lee of the largest island, 4 fm sand. Seven islands
visible. Wind NW 18 kn. Coffee: exhausted. The atlas begins in
earnest.</i></p>

</body>
</html>
"""

# ---------------------------------------------------------------------------
#  CHAPTER 5 — one spine item, nested TOC (chapter + 3 sub-entries)
# ---------------------------------------------------------------------------

CHAPTER_5 = """\
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="en" lang="en">
<head><title>Chapter 5</title>
<link rel="stylesheet" type="text/css" href="style.css"/></head>
<body>

<h1>Chapter 5<br/>Archipelago of Wonders</h1>

<p>The outer archipelago comprised seven islands, but Anchora quickly
learned that three of them demanded most of her attention. The other
four were low, sandy, and profoundly uninteresting &#x2014; the kind of
islands that exist primarily to give seabirds somewhere to argue. She
surveyed them anyway, because a cartographer who skips the boring bits
produces an atlas with holes in it, and an atlas with holes is just a
collection of maps pretending to be complete.</p>

<p>The four minor islands took two days to chart. They were, as expected,
unremarkable: flat coral platforms, none more than ten feet above sea
level, covered in coarse grass and nesting terns. Each island had a
fringing reef, a scattering of rocks, and a beach that was pleasant
enough in theory but occupied in practice by large numbers of
territorial seabirds that regarded Anchora&#x2019;s survey equipment
with undisguised hostility.</p>

<p>She drew them quickly and moved on to the three islands that
mattered.</p>

<h2 id="isle-of-echoes">Isle of Echoes</h2>

<p>The first notable island was ringed by basalt cliffs so sheer they
formed a natural amphitheatre. The cliffs rose two hundred feet from
the waterline, sheer and dark, their faces carved into columns by
millennia of cooling and cracking. At the base, where the waves struck,
the rock had been hollowed into caves and arches that amplified every
sound into a symphony of percussion.</p>

<p>Every sound bounced from wall to wall in diminishing repetitions:
the crash of a wave became a series of claps; a shout became a
conversation with oneself; the rattle of an anchor chain became a
cascading diminuendo that lasted for eight or nine seconds before
finally fading into the ambient hiss of the sea.</p>

<p>Anchora discovered the echoes accidentally. She was approaching the
island from the northeast, looking for an anchorage, when she called
out &#x201C;Hello!&#x201D; to see if anyone was ashore. The island
replied: <i>Hello &#x2026; hello &#x2026; hello &#x2026; ello
&#x2026; lo &#x2026;</i> It was, she reflected, the most polite
island she had ever visited. Most islands ignored you entirely.</p>

<p>She tested the acoustics systematically, because that was her nature.
She stood in the cockpit, positioned the <i>Pagination</i> at measured
distances from the cliff face, and shouted a series of test words:
single syllables first, then multi-syllable words, then complete
sentences. She timed the echoes with her watch and recorded the results
in her notebook.</p>

<p>She tested the acoustics further by reading her coordinates aloud.
&#x201C;Fourteen degrees, thirty-seven minutes south,&#x201D; she
announced in a clear, carrying voice. The cliffs repeated it back,
each echo slightly garbled by the complex geometry of the rock face,
until by the seventh repetition the island appeared to be declaring
itself at forty-seven degrees north. She noted this as a navigational
hazard of a kind not typically covered in the pilot books.</p>

<p>The anchorage, when she found it, was on the island&#x2019;s
sheltered western side, where the cliffs gave way to a small bay with
a sandy bottom in three fathoms. She anchored here and spent two days
surveying the island from the dinghy, rowing along the cliff face with
a notebook in one hand and an oar in the other, measuring the height
of the cliffs by trigonometry and the depth of the water by lead-line.
The caves at the base of the cliffs were deep enough to row into
&#x2014; dark, dripping spaces where the sound of the oars echoed in
unsettling ways and the water was so still it reflected the cave roof
like polished obsidian.</p>

<p>She mapped each cave entrance, noting its position, width, height,
and the depth of water at its threshold. There were eleven caves in
total, ranging from barely large enough for the dinghy to a cathedral-
sized chamber that could have sheltered a small fishing fleet. In the
largest cave she found the remains of an old mooring ring, bolted into
the rock at waterline level and rusted to a deep orange-brown. Someone
had been here before. Someone had anchored in this cave, in this
darkness, and had thought it worthwhile to drill a hole in the basalt
and hammer in a ring.</p>

<p>She added this to the chart with a small symbol and the annotation:
<i>Old mooring ring, condition poor. Cave depth 14 m, width 8 m,
height 5 m at entrance. Adequate shelter in westerly weather.</i></p>

<p>The chart grew a detailed inset of the Isle of Echoes, complete with
soundings, anchorage notes, cave positions, and a warning about acoustic
anomalies that she phrased with care: <i>Echoes from basalt cliffs may
distort voice communications. Coordinates heard from echo may not
correspond to coordinates spoken. Fog signals unreliable within 0.5 nm
of cliff face.</i></p>

<h2 id="compass-rose-atoll">Compass Rose Atoll</h2>

<p>The second island was not, strictly speaking, an island at all. It
was an atoll: a ring of coral enclosing a shallow lagoon, with four
narrow passes at the cardinal points. From the air &#x2014; or from a
sufficiently tall mast &#x2014; it looked exactly like a compass rose,
its four passes aligned so precisely with north, south, east, and west
that Anchora suspected the coincidence was too perfect to be entirely
natural and too natural to be entirely coincidence.</p>

<p>The atoll was roughly circular, perhaps a mile across, its rim
barely six feet above the water at its highest point. The coral was
old and dense, covered with a thin layer of sand and scrub vegetation
that had taken hold wherever the salt spray was not too fierce. On the
wider sections of the rim, coconut palms grew in sparse lines, their
trunks curved by years of prevailing wind into identical
question-mark shapes.</p>

<p>Anchora sailed through the north pass and anchored in the lagoon.
The pass was narrow &#x2014; barely thirty metres wide, with coral
walls on either side that rose steeply from a bottom of clean white
sand &#x2014; and the current through it was brisk on the flood tide,
perhaps two knots, which required attention but not alarm. She entered
on the slack, when the current was negligible, and dropped anchor in
the centre of the lagoon.</p>

<p>The water was so clear she could see her anchor chain lying on the
bottom in gentle curves, like cursive script. The bottom was four
fathoms of white sand, undisturbed by current, and the anchor had dug
in with the easy confidence of a hook in soft ground. Fish moved
through the water column above the sand: small, bright, purposeful
fish that paid no attention to the boat or its occupant.</p>

<p>She spent a full day surveying the atoll, measuring each pass and
sounding the lagoon. The symmetry was remarkable: each pass was within
a boat-length of the same width, and the lagoon was uniformly four
fathoms deep. The north and south passes were slightly wider than the
east and west &#x2014; thirty-two metres versus twenty-eight &#x2014;
but the difference was so small it might have been within the margin of
error of her measuring method, which involved rowing the dinghy across
the pass and counting oar-strokes.</p>

<p>She sounded the lagoon on a grid pattern, rowing the dinghy in
north-south lines spaced fifty metres apart and dropping the lead
every twenty metres. The result was a bathymetric chart of extraordinary
regularity: a flat, featureless bowl of sand, uniformly four fathoms
deep, with no coral heads, no rocks, no obstacles of any kind. It was,
she reflected, the most boring piece of underwater terrain she had ever
surveyed, and also one of the most useful: a lagoon with a flat bottom
and predictable depth was a sailor&#x2019;s dream, a place where you
could anchor anywhere with equal confidence.</p>

<p>Nature, it seemed, had a taste for geometry. Anchora, who also had
a taste for geometry, appreciated this more than most.</p>

<p>She drew the atoll with particular care, using a compass to lay out
the circular rim and placing each pass at its precisely measured bearing.
The chart of Compass Rose Atoll was, she thought, the most beautiful
piece of cartography she had produced on the voyage: clean lines,
perfect symmetry, four passes opening like the petals of a flower.
She added a small compass rose in the corner of the inset chart, which
created the pleasing recursion of a compass rose drawn inside a
compass rose.</p>

<h2 id="whirlpool-narrows">The Whirlpool Narrows</h2>

<p>Between the second and third islands, the tidal flow compressed
through a gap barely a cable&#x2019;s length wide. The two islands were
close here &#x2014; so close that their reefs almost touched, leaving
only a narrow channel of deep water between them. Through this channel,
twice a day, the entire volume of water that filled and emptied the
lagoons and bays of the archipelago had to pass, and it did so with
considerable energy and very little patience.</p>

<p>The result was a whirlpool that spun with metronomic regularity:
clockwise on the ebb, counterclockwise on the flood, and in a state
of churning indecision at slack water. The whirlpool was not large
&#x2014; perhaps twenty metres across at its widest &#x2014; but it
was vigorous, and the water within it moved with a purposeful circular
motion that was both mesmerizing and slightly alarming.</p>

<p>Anchora observed the whirlpool from a safe distance, anchored in
the lee of the eastern island, and made detailed notes. The clockwise
rotation during the ebb was faster than the counterclockwise rotation
during the flood, which she attributed to the shape of the channel:
slightly wider at the southern end, which gave the ebb current &#x2014;
flowing from north to south &#x2014; more room to accelerate before
hitting the narrows. She estimated the rotational speed at three
revolutions per minute during the peak ebb and two during the
peak flood.</p>

<p>The whirlpool was accompanied by an array of secondary effects that
were equally interesting and equally dangerous. Standing waves formed
at the edges of the narrows, where the moving water met the still
water beyond, creating a line of breaking crests that would have been
at home on a surfing beach. Eddies spun off the main whirlpool like
sparks from a wheel, racing downstream in tight spirals before
dissipating in the calmer water beyond. And the sound &#x2014; a low,
continuous roar, punctuated by the slap of breaking waves &#x2014;
carried for half a mile in every direction.</p>

<p>Anchora timed the cycles, measured the diameter, and estimated the
rotational speed. She also measured the current through the narrows at
various states of the tide, using a timing float &#x2014; a sealed
bottle with a small flag attached &#x2014; that she dropped into the
current and tracked with bearings. The maximum current was four and a
half knots during the spring ebb, which was faster than the
<i>Pagination</i> could motor against and considerably faster than
anyone would want to sail through.</p>

<p>The slack water between the ebb and flood was brief &#x2014; eight
minutes by her measurement, during which the whirlpool lost its
coherence, the standing waves subsided, and the channel was briefly,
deceptively calm. This was the transit window: eight minutes in which
a careful boat could pass through the narrows without being spun,
swamped, or swept sideways into the reef.</p>

<p>Eight minutes was not a generous margin. It was, however, sufficient
for a boat that was ready and a skipper who was decisive. Anchora filed
this information under &#x201C;useful&#x201D; and drew the whirlpool
on her chart as a neat spiral with arrows, then added the note:
<i>Transit at slack water only. Allow margin for error. Do not bring
the good sextant.</i></p>

<p>She had now been in the archipelago for four days, and her chart was
beginning to look like something a real navigator might trust. The
blank spaces were filling in. The coastlines were taking shape. The
soundings and current data were building a picture of an archipelago
that was complex, challenging, and deeply satisfying to chart.</p>

<p>This pleased her more than she would have admitted to the
penguins.</p>

</body>
</html>
"""

# ---------------------------------------------------------------------------
#  APPENDIX — one spine item, THREE TOC entries via anchors
# ---------------------------------------------------------------------------

APPENDIX = """\
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="en" lang="en">
<head><title>Appendices</title>
<link rel="stylesheet" type="text/css" href="style.css"/></head>
<body>

<h1 id="appendix-a">Appendix A<br/>Knot Types Employed</h1>

<p>The following knots were tied at various points during the voyage
and are reproduced here for the edification of the reader. Each knot
is described in terms of its construction, its application aboard the
<i>Pagination</i>, and Anchora&#x2019;s personal assessment of its
character, because all knots have character, and some have more
character than their tyers would prefer.</p>

<p><b>Bowline.</b> The king of knots. Used to secure the
<i>Pagination</i> to docks, bollards, and on one occasion a
surprisingly cooperative palm tree on the Isle of Echoes. The bowline
forms a fixed loop at the end of a line that will not slip under load
and will not jam when you want to untie it, which is a combination of
virtues that very few knots can claim. Anchora tied bowlines
instinctively; her fingers would form the loop, pass the tail around
the standing part, and thread it back through the loop in a single
fluid motion that took approximately one and a half seconds. She had
once won a bowline-tying competition at a sailing club in the Leeward
Islands, beating a boatswain with forty years&#x2019; experience and
very large hands. The boatswain had been gracious in defeat and had
bought her a rum, which is the traditional currency of nautical
respect. Advantages: does not slip, does not jam, can be tied under
load if necessary. Disadvantages: requires two hands, which is one
more than a sailor holding a coffee cup has available.</p>

<p><b>Cleat hitch.</b> Used at every marina, every pontoon, and every
dock that the <i>Pagination</i> visited. The cleat hitch is not a
glamorous knot &#x2014; it is the workhorse of the marina, the knot
equivalent of a reliable sedan &#x2014; but it is satisfyingly quick
to tie and untie, and it holds with absolute reliability in any
conditions. Anchora could do it in four seconds. She timed herself,
because she was the sort of person who timed herself tying knots, and
she regarded this as a perfectly normal hobby. The cleat hitch involves
one full turn around the base of the cleat, followed by two
figure-eight turns over the horns, finished with a locking hitch. The
locking hitch is optional but recommended by Anchora, who had once seen
an unlocked cleat hitch work loose in a gale and release a thirty-foot
yacht into the fairway, where it drifted gently into a row of dinghies
and caused what the insurance industry would later describe as a
&#x201C;multi-vessel incident.&#x201D;</p>

<p><b>Figure-eight.</b> Used as a stopper knot on every sheet and
halyard aboard the <i>Pagination</i>. The figure-eight prevents the
line from running out through its block or fairlead, which is the kind
of thing that happens at the worst possible moment &#x2014; during a
tack, in heavy weather, when both hands are already occupied with
something else that is going wrong. The figure-eight is the kind of
knot that, once learned, the fingers tie without consulting the brain.
Anchora tied figure-eights the way other people blink: automatically,
unconsciously, and with a frequency that suggested a deep-seated
neurological commitment to the prevention of runaway lines.</p>

<p><b>Round turn and two half-hitches.</b> Used for securing the dinghy
painter to rocks, trees, and the occasional bollard that was too large
or too oddly shaped for a bowline. This knot is not elegant, but it is
secure, adjustable, and can be tied around objects of any shape, which
makes it the knot of last resort &#x2014; the one you reach for when
nothing else will work. Anchora used it approximately once per week and
thought about it approximately never, which is the highest compliment
a sailor can pay a knot.</p>

<p><b>Reef knot.</b> Used for tying reef points when shortening sail.
The reef knot is perhaps the most misunderstood knot in the nautical
lexicon: it is often taught as a general-purpose binding knot, which it
is not. It will slip if loaded unevenly, it will jam if loaded
heavily, and it will capsize if looked at sternly. For tying reef
points, however, it is perfect: quick to tie, quick to untie, and
adequate for the modest loads involved. Anchora tied reef knots in the
dark, in the rain, on a heaving deck, with numb fingers, and the reef
knot never once let her down, because she never once asked it to do
anything it was not designed to do.</p>

<p><b>Sheet bend.</b> Used for joining two lines of different diameter.
Anchora used this knot exactly once during the voyage, when she needed
to extend her lead-line with a lighter-weight extension over the deep
water. The sheet bend held, the sounding was taken, and the knot was
untied. It was, she reflected, the knot equivalent of a specialist
consultant: expensive to learn, rarely needed, but invaluable on the
one occasion you called upon it.</p>

<h1 id="appendix-b">Appendix B<br/>Signal Flags Observed</h1>

<p>During the voyage, the following International Code of Signals flags
were observed flying from other vessels. The International Code is a
system of flag signals that allows ships of different nationalities to
communicate without a common language, which is an admirable ambition
that works considerably better in theory than in practice, because the
system assumes that all parties can identify forty flags at a distance
and under conditions that typically include spray, glare, and a
rolling deck.</p>

<p><b>Alpha.</b> A white and blue swallowtail flag. &#x201C;I have a
diver down; keep well clear at slow speed.&#x201D; Observed near the
reef, flying from a battered workboat whose diver was, presumably,
somewhere below, doing whatever it is that divers do in two metres of
water over a coral reef. Anchora kept well clear, as instructed, and
made a note of the workboat&#x2019;s position so that she could add a
&#x201C;diving operations&#x201D; annotation to her chart of the reef.
She later removed the annotation on the grounds that the diving was
presumably temporary, whereas the chart was intended to be permanent, or
at least as permanent as a chart of a reef can be when the reef is
still growing.</p>

<p><b>Bravo.</b> A red swallowtail flag. &#x201C;I am taking in, or
discharging, or carrying dangerous goods.&#x201D; Observed at the Twin
Harbors, flying from a rusty coaster that was moored alongside the
north quay in East Harbor (or possibly West Harbor; the question, as
always, remained unresolved). The dangerous goods turned out to be a
crate of live chickens, which the harbourmaster&#x2019;s regulations
classified as &#x201C;livestock&#x201D; but which the chickens&#x2019;
behaviour &#x2014; aggressive, noisy, and apparently attempting to
escape &#x2014; suggested might more accurately have been classified as
&#x201C;hazardous materials.&#x201D;</p>

<p><b>Hotel.</b> A white flag with a red vertical stripe. &#x201C;I
have a pilot on board.&#x201D; Observed on Rufus&#x2019;s pilot boat,
naturally, and on several vessels entering and leaving the Twin Harbors
under his guidance. The Hotel flag is one of the more useful signals in
the code, as it tells other vessels that the flagged vessel is being
navigated by someone who knows the local waters, and that any unusual
manoeuvres it performs are probably intentional rather than accidental.
Anchora flew the Hotel flag exactly once, when Rufus came aboard to
guide her through the cut between the two harbors, and she removed it
the moment he left, because flying a pilot flag without a pilot aboard
is both improper and illegal, and Anchora was punctilious about such
things.</p>

<p><b>November.</b> A chequered blue and white flag. &#x201C;No&#x201D;
or &#x201C;negative.&#x201D; Observed on a yacht that was being hailed
by the coast guard and appeared to be declining to stop. Anchora watched
this exchange with professional interest and noted in her log that the
yacht was making nine knots downwind, which was probably insufficient
to outrun the coast guard cutter but was certainly a spirited attempt.
The outcome of the pursuit was not recorded, as the <i>Pagination</i>
had by then sailed out of visual range.</p>

<p><b>Quebec.</b> A solid yellow flag. &#x201C;My vessel is healthy and
I request free pratique.&#x201D; Flown by the <i>Pagination</i> herself
upon arrival at each port, mostly out of optimism. Free pratique is
permission to make contact with the shore, granted by the port health
authority after they have satisfied themselves that your vessel is not
carrying plague, cholera, or any other communicable disease. In modern
times, the flag is largely ceremonial &#x2014; most ports grant
pratique automatically &#x2014; but Anchora flew it anyway, because
traditions are traditions, and because a yellow flag on a small sloop
entering a foreign harbor has a certain jaunty charm that she privately
enjoyed.</p>

<h1 id="appendix-c">Appendix C<br/>Tidal Observations</h1>

<p>Tidal data collected during the voyage is summarized below. All
times are approximate, all heights are measured from chart datum, and
all predictions should be treated with the same confidence one extends
to a weather forecast &#x2014; which is to say, they are probably
correct, but you should not bet your keel on it.</p>

<p>Tides in the region are predominantly semi-diurnal, with two high
waters and two low waters in each lunar day. The tidal range varies
with the phase of the moon: greatest at springs (when the sun and moon
are aligned) and smallest at neaps (when they are at right angles).
Anchora recorded tidal observations at every anchorage, using a
graduated staff driven into the seabed at the waterline, which she
read at hourly intervals whenever she was aboard and awake, and at
less regular intervals when she was not.</p>

<p><b>Twin Harbors.</b> Semi-diurnal, range 1.8&#x2013;2.4 m at
springs, 0.8&#x2013;1.2 m at neaps. High water approximately coincides
with lunar transit, with a lag of roughly forty minutes that Anchora
attributed to the constriction of the harbor entrance and the friction
of the water flowing over the shallow bar at the approach channel. The
two basins exhibit a 12-minute phase lag &#x2014; high water in East
Harbor occurs twelve minutes before high water in West Harbor &#x2014;
which the locals blame on a submerged rock formation in the cut between
the two basins and Anchora blames on insufficient data. She measured
the phase lag on three consecutive tidal cycles and got three different
results: 10 minutes, 14 minutes, and 12 minutes. The average was 12
minutes. She reported 12 minutes. The truth, she suspected, was
considerably more complicated than a single number could express, but
a single number was what the chart demanded, and the chart was, for
better or worse, what people would rely on.</p>

<p><b>The Reef.</b> Tidal range similar to the Twin Harbors. The
critical observation for the reef is not the range but the height of
low water at springs, which determines how much of the coral is
exposed. At LAT (Lowest Astronomical Tide), several of the larger
coral heads break the surface, creating visible markers that are useful
for daylight navigation but invisible &#x2014; and therefore lethal
&#x2014; at night. Anchora marked these drying heights on her chart
with the standard symbol: a depth figure with a line underneath,
indicating that the number represents elevation above chart datum rather
than depth below it.</p>

<p><b>Whirlpool Narrows.</b> Tidal streams reach 4.5 kn at springs,
2.8 kn at neaps. The flood stream sets northward; the ebb sets
southward. Slack water occurs approximately 15 minutes before local
high and low water and lasts approximately 8 minutes at springs, 12
minutes at neaps. This is not a generous margin in either case, but
it is sufficient for a transit if the vessel is prepared and the skipper
decisive. Anchora recommended approaching the narrows under power
rather than sail, on the grounds that an engine provides consistent
thrust regardless of wind angle, whereas a sail provides consistent
anxiety regardless of everything.</p>

<p><b>Compass Rose Atoll.</b> Negligible tidal range inside the lagoon
(0.3 m at springs, barely measurable at neaps). This is because the
four passes, while wide enough for navigation, are narrow enough to
attenuate the tidal wave as it enters the lagoon, spreading the rise
and fall over a much longer period and reducing its amplitude to
almost nothing. The practical consequence is that the lagoon maintains
a nearly constant depth, which makes it an ideal anchorage in any
conditions &#x2014; a rare and valuable quality in an archipelago where
most of the water is trying to go somewhere in a hurry. Currents
through the passes reach 2 kn on springs but are predictable and
well-behaved. Anchora described them in her notes as &#x201C;the only
polite water in the archipelago.&#x201D;</p>

<p><b>Isle of Echoes.</b> Semi-diurnal, range 1.6&#x2013;2.0 m. The
tidal stream along the cliff face sets to the north on the flood and
to the south on the ebb, reaching 1.5 kn at springs. The stream is
strongest at the headlands and weakest in the embayments, which is the
normal pattern for tidal flow around a rocky island and was entirely
predictable from the chart. What was not predictable was the acoustic
effect of the tide on the cave systems at the base of the cliffs:
at certain states of the tide, the incoming water compressed the air
inside the caves and produced a low, resonant boom that could be heard
from the anchorage, half a mile away. Anchora noted this phenomenon
in her log but did not add it to the chart, on the grounds that
&#x201C;makes spooky noises at half tide&#x201D; was not a recognised
chart annotation.</p>

<h1 id="appendix-d">Appendix D<br/>Errata</h1>

<p>No errors have been found. This is, in itself, suspicious.</p>

<h1 id="appendix-e">Appendix E<br/>Acknowledgments</h1>

<p>The author wishes to thank the penguins.</p>

</body>
</html>
"""

# ---------------------------------------------------------------------------
#  BACKMATTER — TOC points to #colophon (mid-file), not file start
# ---------------------------------------------------------------------------

BACKMATTER = """\
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="en" lang="en">
<head><title>Back Matter</title>
<link rel="stylesheet" type="text/css" href="style.css"/></head>
<body>

<h2>Author&#x2019;s Note</h2>

<p>This section has no entry in the table of contents. If you have
arrived here by paging forward from the appendices, congratulations:
you are reading in spine order, which is the only reliable way to find
content that the navigation has chosen to ignore.</p>

<p>The Author&#x2019;s Note exists to test a specific edge case: a
spine item whose table-of-contents entry points not to the beginning
of the file, but to an anchor partway through it. Everything above the
anchor is &#x201C;dark matter&#x201D; &#x2014; present in the document,
reachable by paging, but invisible to the TOC.</p>

<p>Consider, for a moment, what this means for a reading application.
When a user taps &#x201C;Colophon&#x201D; in the table of contents,
the application must navigate not to the start of this file, but to
the <i>colophon</i> anchor partway through it. The content above the
anchor &#x2014; the text you are reading right now &#x2014; exists in
the spine, occupies space on the page, and can be reached by turning
pages forward from the appendices. But it cannot be reached by any
navigation element. It is, in the language of the EPUB specification,
part of the spine order but outside the navigation document.</p>

<p>This creates an interesting problem for any reader that attempts to
map TOC entries to page ranges. If the TOC says the Colophon begins
at anchor #colophon, what is the page range of the content before
that anchor? Does it belong to the previous TOC entry (Appendix C)?
Does it belong to the Colophon? Does it belong to no entry at all?
These are the kinds of questions that this test fixture is designed
to provoke.</p>

<p>Anchora would have had opinions about this. Cartographers do not
approve of places that exist but cannot be navigated to. An island
without a name on the chart is an island that will, sooner or later,
catch a keel.</p>

<p>But perhaps some content is meant to be found only by those who
read sequentially, page by page, without jumping ahead. Perhaps the
Author&#x2019;s Note is a reward for patience. Or perhaps it is
simply a test fixture dressed up in a narrative voice.</p>

<p>Either way, you have found it. Well done.</p>

<h2 id="colophon">Colophon</h2>

<p>This EPUB was generated by a Python script as a test fixture for the
Crosspoint Reader project. It is not a real book, though it contains
real sentences arranged in a real order, which is more than can be said
for some publications that aspire to the title.</p>

<p>The spine of this EPUB contains fourteen items. Its table of contents
contains eighteen entries. The relationship between the two is, by
design, entertainingly non-trivial. Specifically:</p>

<p>Three TOC entries point to anchors within <i>frontmatter.xhtml</i>,
making it a single spine item with multiple navigation targets. Four
TOC entries (one parent and three children) point to anchors within
<i>chapter3.xhtml</i>, and four more point into <i>chapter5.xhtml</i>,
testing nested TOC hierarchies within a single spine item. Three TOC
entries point to anchors within <i>appendix.xhtml</i>, the same
pattern without nesting. Two of these appendix entries (D and E)
are deliberately tiny &#x2014; a single sentence each &#x2014; to
test how the reader handles TOC sections too small to fill a
screen.</p>

<p>Chapter 2 occupies two spine items (<i>chapter2_part1.xhtml</i> and
<i>chapter2_part2.xhtml</i>) but appears as a single entry in the TOC.
Chapter 4 occupies three spine items and also appears as a single TOC
entry. These test the case where a reader must determine that consecutive
spine items belong to the same logical chapter.</p>

<p>The interlude (<i>interlude.xhtml</i>) appears in the spine between
Chapter 3 and Chapter 4 but has no TOC entry whatsoever, testing the
case where spine items exist outside the navigation hierarchy.</p>

<p>And this very file (<i>backmatter.xhtml</i>) has a TOC entry that
points to the #colophon anchor, which is not at the beginning of the
file. The Author&#x2019;s Note above occupies the first portion of the
file but is invisible to the TOC, testing the case where a navigation
target lands in the middle of a document.</p>

<p>The cover image was generated programmatically using the Bookerly
typeface at 536&#x00D7;800 pixels, rendered on a deep teal background
(RGB 15, 55, 65) with light beige text (RGB 225, 220, 205).</p>

<p>No penguins were harmed in the making of this book. Several were
mildly inconvenienced, but they bore it with their customary dignity.</p>

</body>
</html>
"""

# ---------------------------------------------------------------------------
#  EPUB boilerplate
# ---------------------------------------------------------------------------

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
<img src="cover.jpg" alt="Spine &amp; Anchor: A Nautical Misadventure"/>
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
    <item id="frontmatter" href="frontmatter.xhtml" media-type="application/xhtml+xml"/>
    <item id="ch1" href="chapter1.xhtml" media-type="application/xhtml+xml"/>
    <item id="ch2p1" href="chapter2_part1.xhtml" media-type="application/xhtml+xml"/>
    <item id="ch2p2" href="chapter2_part2.xhtml" media-type="application/xhtml+xml"/>
    <item id="ch3" href="chapter3.xhtml" media-type="application/xhtml+xml"/>
    <item id="interlude" href="interlude.xhtml" media-type="application/xhtml+xml"/>
    <item id="ch4p1" href="chapter4_part1.xhtml" media-type="application/xhtml+xml"/>
    <item id="ch4p2" href="chapter4_part2.xhtml" media-type="application/xhtml+xml"/>
    <item id="ch4p3" href="chapter4_part3.xhtml" media-type="application/xhtml+xml"/>
    <item id="ch5" href="chapter5.xhtml" media-type="application/xhtml+xml"/>
    <item id="appendix" href="appendix.xhtml" media-type="application/xhtml+xml"/>
    <item id="backmatter" href="backmatter.xhtml" media-type="application/xhtml+xml"/>
    <item id="toc" href="toc.xhtml" media-type="application/xhtml+xml" properties="nav"/>
  </manifest>
  <spine>
    <itemref idref="cover"/>
    <itemref idref="toc"/>
    <itemref idref="frontmatter"/>
    <itemref idref="ch1"/>
    <itemref idref="ch2p1"/>
    <itemref idref="ch2p2"/>
    <itemref idref="ch3"/>
    <itemref idref="interlude"/>
    <itemref idref="ch4p1"/>
    <itemref idref="ch4p2"/>
    <itemref idref="ch4p3"/>
    <itemref idref="ch5"/>
    <itemref idref="appendix"/>
    <itemref idref="backmatter"/>
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
<h1>Spine &#x26; Anchor</h1>
<nav epub:type="toc">
  <ol>
    <li><a href="frontmatter.xhtml#dedication">Dedication</a></li>
    <li><a href="frontmatter.xhtml#epigraph">Epigraph</a></li>
    <li><a href="frontmatter.xhtml#foreword">Foreword</a></li>
    <li><a href="chapter1.xhtml">Chapter 1 &#x2013; Setting Sail</a></li>
    <li><a href="chapter2_part1.xhtml">Chapter 2 &#x2013; The Twin Harbors</a></li>
    <li>
      <a href="chapter3.xhtml">Chapter 3 &#x2013; Charting the Depths</a>
      <ol>
        <li><a href="chapter3.xhtml#the-reef">The Reef</a></li>
        <li><a href="chapter3.xhtml#the-current">The Current</a></li>
        <li><a href="chapter3.xhtml#the-depths">The Depths</a></li>
      </ol>
    </li>
    <li><a href="chapter4_part1.xhtml">Chapter 4 &#x2013; The Long Voyage</a></li>
    <li>
      <a href="chapter5.xhtml">Chapter 5 &#x2013; Archipelago of Wonders</a>
      <ol>
        <li><a href="chapter5.xhtml#isle-of-echoes">Isle of Echoes</a></li>
        <li><a href="chapter5.xhtml#compass-rose-atoll">Compass Rose Atoll</a></li>
        <li><a href="chapter5.xhtml#whirlpool-narrows">The Whirlpool Narrows</a></li>
      </ol>
    </li>
    <li><a href="appendix.xhtml#appendix-a">Appendix A &#x2013; Knot Types</a></li>
    <li><a href="appendix.xhtml#appendix-b">Appendix B &#x2013; Signal Flags</a></li>
    <li><a href="appendix.xhtml#appendix-c">Appendix C &#x2013; Tidal Observations</a></li>
    <li><a href="appendix.xhtml#appendix-d">Appendix D &#x2013; Errata</a></li>
    <li><a href="appendix.xhtml#appendix-e">Appendix E &#x2013; Acknowledgments</a></li>
    <li><a href="backmatter.xhtml#colophon">Colophon</a></li>
  </ol>
</nav>
</body>
</html>
"""


# ---------------------------------------------------------------------------
#  Build
# ---------------------------------------------------------------------------

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
        zf.writestr("OEBPS/frontmatter.xhtml", FRONTMATTER)
        zf.writestr("OEBPS/chapter1.xhtml", CHAPTER_1)
        zf.writestr("OEBPS/chapter2_part1.xhtml", CHAPTER_2_PART1)
        zf.writestr("OEBPS/chapter2_part2.xhtml", CHAPTER_2_PART2)
        zf.writestr("OEBPS/chapter3.xhtml", CHAPTER_3)
        zf.writestr("OEBPS/interlude.xhtml", INTERLUDE)
        zf.writestr("OEBPS/chapter4_part1.xhtml", CHAPTER_4_PART1)
        zf.writestr("OEBPS/chapter4_part2.xhtml", CHAPTER_4_PART2)
        zf.writestr("OEBPS/chapter4_part3.xhtml", CHAPTER_4_PART3)
        zf.writestr("OEBPS/chapter5.xhtml", CHAPTER_5)
        zf.writestr("OEBPS/appendix.xhtml", APPENDIX)
        zf.writestr("OEBPS/backmatter.xhtml", BACKMATTER)
    print(f"EPUB written to {output_path}")


if __name__ == "__main__":
    project_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = os.path.join(project_root, "test", "epubs", "test_spine_toc_edges.epub")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    build_epub(out)
