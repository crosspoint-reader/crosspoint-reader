#!/usr/bin/env python3
"""
Generate a small EPUB that exercises Thai text rendering edge cases.

Thai rendering features tested:
  - Basic Thai consonants (ก-ฮ) with normal advance
  - Above vowels (ิ ี ึ ื ั ็) — combining marks with advanceX=0, negative left
  - Below vowels (ุ ู ฺ) — combining marks below baseline
  - Tone marks (่ ้ ๊ ๋) — combining marks stacked above vowels
  - Mark-on-mark stacking: vowel + tone on same consonant (กี้, ดู่, etc.)
  - Leading vowels (เ แ โ ไ ใ) — positive advance, appear before consonant in Unicode
  - Sara Am (ำ) — special character that decomposes to nikhahit + sara aa
  - Follow vowels (ะ า) — appear after consonant, positive advance
  - Thai word breaking — long runs without spaces for line-break testing
  - Mixed Thai/English text — script transitions mid-paragraph
  - Thai digits (๐-๙) and punctuation (ฯ ๆ ฯลฯ)
  - Nikhahit + yamakkan (ํ ์) — less common combining marks
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
_NOTO_THAI_FONT = os.path.join(
    _PROJECT_ROOT, "lib", "EpdFont", "builtinFonts", "source",
    "NotoSansThai", "NotoSansThai-Regular.ttf",
)


def _get_font(size=20):
    """Get the NotoSansThai font at the requested size, with system fallbacks."""
    paths = [_NOTO_THAI_FONT]
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
    """Generate a cover image and return JPEG bytes."""
    width, height = 536, 800
    bg_color = (42, 58, 30)
    text_color = (225, 220, 205)

    img = Image.new("RGB", (width, height), bg_color)
    draw = ImageDraw.Draw(img)

    font_title = _get_font(64)
    font_subtitle = _get_font(26)
    font_author = _get_font(14)

    title_lines = ["ทดสอบ", "ภาษาไทย"]
    title_y = 140
    for line in title_lines:
        _draw_text_centered(draw, title_y, line, font_title, text_color, width)
        title_y += 100

    subtitle_y = title_y + 40
    _draw_text_centered(draw, subtitle_y, "Thai Rendering Edge Cases",
                        font_subtitle, text_color, width)

    _draw_text_centered(draw, height - 70, "CROSSPOINT TEST FIXTURES",
                        font_author, text_color, width)

    buf = io.BytesIO()
    img.save(buf, "JPEG", quality=90)
    return buf.getvalue()


BOOK_UUID = str(uuid.uuid4())
TITLE = "Thai Rendering Edge Cases"
AUTHOR = "Crosspoint Test Fixtures"
DATE = datetime.now().strftime("%Y-%m-%d")

# Chapter 1: Basic Thai consonants, simple vowels, common words.
# Exercises the normal rendering path (positive advanceX) for Thai base glyphs
# and the combining mark path (advanceX=0) for above/below vowels and tones.
CHAPTER_1 = """\
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="th" lang="th">
<head><title>บทที่ ๑ – อักษรพื้นฐาน</title>
<link rel="stylesheet" type="text/css" href="style.css"/></head>
<body>
<h1>บทที่ ๑<br/>อักษรพื้นฐาน</h1>

<h2>พยัญชนะไทย</h2>

<p>ก ข ฃ ค ฅ ฆ ง จ ฉ ช ซ ฌ ญ ฎ ฏ ฐ ฑ ฒ ณ ด ต ถ ท ธ น บ ป ผ ฝ พ ฟ ภ ม ย ร ล ว ศ ษ ส ห ฬ อ ฮ</p>

<h2>สระลอย</h2>

<p>สระ อะ อา อิ อี อึ อื อุ อู เอ แอ โอ ไอ ใอ</p>

<h2>วรรณยุกต์</h2>

<p>กา ก่า ก้า ก๊า ก๋า — กี กี่ กี้ กี๊ กี๋ — กู กู่ กู้ กู๊ กู๋</p>

<h2>คำง่ายๆ</h2>

<p>สวัสดีครับ สวัสดีค่ะ ขอบคุณ ขอโทษ ยินดีที่ได้รู้จัก</p>

<p>วันนี้อากาศดีมาก ท้องฟ้าสีครามสดใส ลมพัดเย็นสบาย
ต้นไม้ใบหญ้าเขียวขจี ดอกไม้บานสะพรั่งหลากสี
นกร้องเพลงไพเราะ เด็กๆ วิ่งเล่นในสวนอย่างสนุกสนาน</p>

<p>ครอบครัวของฉันมีสมาชิกห้าคน คือ พ่อ แม่ พี่ชาย น้องสาว
และฉัน พ่อเป็นหมอ แม่เป็นครู พี่ชายเรียนมหาวิทยาลัย
น้องสาวยังเรียนอยู่ชั้นประถม ฉันทำงานที่กรุงเทพฯ</p>
</body>
</html>
"""

# Chapter 2: Stacked combining marks — the hardest rendering test.
# Every Thai consonant-vowel-tone combination produces two zero-advance
# marks on the same base, requiring correct mark-on-mark stacking.
CHAPTER_2 = """\
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="th" lang="th">
<head><title>บทที่ ๒ – สระผสมวรรณยุกต์</title>
<link rel="stylesheet" type="text/css" href="style.css"/></head>
<body>
<h1>บทที่ ๒<br/>สระผสมวรรณยุกต์</h1>

<h2>สระบน + วรรณยุกต์ (Mark-on-Mark Stacking)</h2>

<p>Each row stacks an above-vowel with each of the four tone marks
on a single base consonant. Both marks have advanceX=0; the second
must be raised above the first.</p>

<p><b>สระ อิ + วรรณยุกต์:</b><br/>
กิ่ กิ้ กิ๊ กิ๋ — ขิ่ ขิ้ ขิ๊ ขิ๋ — ดิ่ ดิ้ ดิ๊ ดิ๋ — ปิ่ ปิ้ ปิ๊ ปิ๋ — ริ่ ริ้ ริ๊ ริ๋</p>

<p><b>สระ อี + วรรณยุกต์:</b><br/>
กี่ กี้ กี๊ กี๋ — ขี่ ขี้ ขี๊ ขี๋ — ดี่ ดี้ ดี๊ ดี๋ — มี่ มี้ มี๊ มี๋ — ลี่ ลี้ ลี๊ ลี๋</p>

<p><b>สระ อึ + วรรณยุกต์:</b><br/>
กึ่ กึ้ กึ๊ กึ๋ — ขึ่ ขึ้ ขึ๊ ขึ๋ — ดึ่ ดึ้ ดึ๊ ดึ๋ — ถึ่ ถึ้ ถึ๊ ถึ๋</p>

<p><b>สระ อื + วรรณยุกต์:</b><br/>
กื่ กื้ กื๊ กื๋ — ขื่ ขื้ ขื๊ ขื๋ — ดื่ ดื้ ดื๊ ดื๋ — มื่ มื้ มื๊ มื๋</p>

<p><b>สระ อั + วรรณยุกต์:</b><br/>
กั่ กั้ กั๊ กั๋ — ขั่ ขั้ ขั๊ ขั๋ — ดั่ ดั้ ดั๊ ดั๋ — ลั่ ลั้ ลั๊ ลั๋</p>

<p><b>สระ อ็ + วรรณยุกต์:</b><br/>
ก็่ ก็้ ก็๊ ก็๋ — ข็่ ข็้ ข็๊ ข็๋ — ด็่ ด็้ ด็๊ ด็๋</p>

<h2>สระล่าง + วรรณยุกต์</h2>

<p>Below-vowels sit below the baseline. The tone mark sits above the
base consonant (not above the below-vowel). raiseAboveBase should
return 0 for the below-vowel.</p>

<p><b>สระ อุ + วรรณยุกต์:</b><br/>
กุ่ กุ้ กุ๊ กุ๋ — ขุ่ ขุ้ ขุ๊ ขุ๋ — ดุ่ ดุ้ ดุ๊ ดุ๋ — ปุ่ ปุ้ ปุ๊ ปุ๋</p>

<p><b>สระ อู + วรรณยุกต์:</b><br/>
กู่ กู้ กู๊ กู๋ — ขู่ ขู้ ขู๊ ขู๋ — ดู่ ดู้ ดู๊ ดู๋ — ปู่ ปู้ ปู๊ ปู๋</p>

<h2>คำตัวอย่างที่ใช้สระผสม</h2>

<p>มี้นิ่ง ปี้น ลื้อ ดื้อ ดึ๋ง กุ้ง ปู้ กี๋ นี่ นี้ นึ้ กั้น ลั่น ดั้ง ตั๋ว</p>

<p>ยิ่งใหญ่ ชิ้นส่วน ลิ้นจี่ ปิ้งย่าง กลิ้งไป ดิ้นรน
มื้อเที่ยง ลื้อค้า ซื้อขาย ดื้อด้าน ยื้อเวลา
คึ้ก ซึ้ง ปรึ้ง ดึ้ง ฮึ้ม</p>
</body>
</html>
"""

# Chapter 3: Word breaking / line wrapping.
# Long Thai text without spaces to exercise ThaiWordBreak cluster
# segmentation in ParsedText::addWord and WordJoin::JOINED.
CHAPTER_3 = """\
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="th" lang="th">
<head><title>บทที่ ๓ – การตัดคำ</title>
<link rel="stylesheet" type="text/css" href="style.css"/></head>
<body>
<h1>บทที่ ๓<br/>การตัดคำ</h1>

<h2>ข้อความยาวไม่มีช่องว่าง</h2>

<p>กรุงเทพมหานครเป็นเมืองหลวงของประเทศไทยตั้งอยู่บริเวณที่ราบลุ่มแม่น้ำเจ้าพระยามีประชากรมากกว่าสิบล้านคนเป็นศูนย์กลางทางเศรษฐกิจการเมืองและวัฒนธรรมของประเทศ</p>

<p>ภาษาไทยเป็นภาษาที่มีระบบเสียงวรรณยุกต์ห้าเสียงคือสามัญเอกโทตรีจัตวาการเขียนภาษาไทยไม่เว้นวรรคระหว่างคำทำให้การตัดคำเป็นปัญหาสำคัญในการประมวลผลภาษาธรรมชาติ</p>

<p>สถาปัตยกรรมไทยเป็นศิลปะที่สืบทอดมาจากอดีตอันยาวนานมีลักษณะเฉพาะตัวที่โดดเด่นด้วยความประณีตงดงามของลวดลายหลังคาซ้อนชั้นยอดแหลมสูงเสาลอยและบัวหัวเสาที่ตกแต่งอย่างวิจิตรบรรจง</p>

<h2>ข้อความยาวผสมสระและวรรณยุกต์</h2>

<p>น้ำตกเอราวัณเป็นน้ำตกที่สวยงามที่สุดแห่งหนึ่งในจังหวัดกาญจนบุรีมีทั้งหมดเจ็ดชั้นแต่ละชั้นมีลักษณะเฉพาะตัวที่แตกต่างกันออกไปชั้นที่หนึ่งเป็นแอ่งน้ำขนาดใหญ่ที่ใสสะอาดสามารถมองเห็นปลาว่ายน้ำได้อย่างชัดเจน</p>

<p>อาหารไทยมีชื่อเสียงไปทั่วโลกด้วยรสชาติที่กลมกล่อมลงตัวทั้งเปรี้ยวหวานเค็มเผ็ดผัดไทยเป็นอาหารที่คนทั่วโลกรู้จักดีส่วนต้มยำกุ้งเป็นซุปรสจัดที่เป็นเอกลักษณ์ของไทยแกงเขียวหวานมีกะทิข้นหอมมันส้มตำเป็นอาหารอีสานที่ได้รับความนิยมอย่างแพร่หลาย</p>

<h2>ย่อหน้าหลายย่อหน้า</h2>

<p>ประเทศไทยมีภูมิอากาศแบบร้อนชื้นฝนตกชุกในช่วงฤดูมรสุม
อุณหภูมิเฉลี่ยตลอดปีอยู่ที่ประมาณยี่สิบเจ็ดถึงสามสิบสามองศาเซลเซียส
ฤดูร้อนเริ่มตั้งแต่เดือนมีนาคมถึงพฤษภาคม
ฤดูฝนเริ่มตั้งแต่เดือนมิถุนายนถึงตุลาคม
ฤดูหนาวเริ่มตั้งแต่เดือนพฤศจิกายนถึงกุมภาพันธ์</p>

<p>ช้างเป็นสัตว์สัญลักษณ์ของประเทศไทย ช้างไทยมีสองชนิดคือช้างป่าและช้างบ้าน
ช้างเผือกถือเป็นสัตว์มงคลตั้งแต่สมัยโบราณ ในอดีตช้างใช้ในการรบและขนส่ง
ปัจจุบันช้างไทยได้รับการคุ้มครองตามกฎหมาย</p>
</body>
</html>
"""

# Chapter 4: Mixed Thai/English and special characters.
# Exercises script transitions, Thai digits, and punctuation.
CHAPTER_4 = """\
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="th" lang="th">
<head><title>บทที่ ๔ – ไทยผสมอังกฤษ</title>
<link rel="stylesheet" type="text/css" href="style.css"/></head>
<body>
<h1>บทที่ ๔<br/>ไทยผสมอังกฤษ</h1>

<h2>Mixed Script</h2>

<p>กรุงเทพฯ หรือ Bangkok เป็นเมืองหลวงของไทย (Thailand)
มีประชากรประมาณ ๑๐ ล้านคน เป็นเมืองที่ใหญ่ที่สุดใน
Southeast Asia หลังจากที่ Jakarta ผ่านการแยกส่วน</p>

<p>ระบบ GPS ของ smartphone ช่วยให้การนำทางในกรุงเทพฯ สะดวกขึ้น
แอป Google Maps รองรับภาษาไทย สามารถค้นหาสถานที่ด้วย
ชื่อไทยหรือชื่ออังกฤษก็ได้ เช่น ค้นหา "วัดพระแก้ว" หรือ
"Wat Phra Kaew" ก็จะพบสถานที่เดียวกัน</p>

<h2>เลขไทย</h2>

<p>๐ ๑ ๒ ๓ ๔ ๕ ๖ ๗ ๘ ๙</p>

<p>ปี พ.ศ. ๒๕๖๙ ตรงกับ ค.ศ. 2026
อุณหภูมิวันนี้ ๓๒°ซ. (32°C)
ราคาข้าวจานละ ๔๐-๖๐ บาท</p>

<h2>เครื่องหมายพิเศษ</h2>

<p>ฯ ใช้เป็นเครื่องหมายย่อ เช่น กรุงเทพฯ (กรุงเทพมหานคร)
นายกฯ (นายกรัฐมนตรี) รมว.ฯ (รัฐมนตรีว่าการฯ)</p>

<p>ๆ ใช้เป็นเครื่องหมายไม้ยมก เช่น เด็กๆ (เด็กเด็ก)
คนๆ นั้น (คนคนนั้น) วันๆ หนึ่ง (วันวันหนึ่ง)</p>

<p>ฯลฯ ใช้แทนคำว่า "และอื่นๆ" เช่น ผลไม้ไทยมีหลายชนิด
เช่น มะม่วง ทุเรียน มังคุด ลำไย ฯลฯ</p>

<h2>Sara Am (ำ)</h2>

<p>น้ำ คำ ทำ จำ ลำ ดำ สำ รำ กำ ถำ</p>

<p>น้ำตาลทรายขาวหนึ่งกิโลกรัมราคาสามสิบบาท
คำถามข้อนี้ไม่มีคำตอบที่ถูกต้อง
ทำงานหนักทำให้เหนื่อย
จำได้ว่าเคยมาที่นี่เมื่อปีก่อน</p>

<h2>Nikhahit (ํ) and Yamakkan (์)</h2>

<p>ธรรม์ กรรม์ สัมมนา อัมพาต</p>

<p>เบียร์ คอมพิวเตอร์ อินเทอร์เน็ต โปรแกรมเมอร์
ฟุตบอล์ แฮมเบอร์เกอร์ ช็อกโกแลต์</p>
</body>
</html>
"""

# Chapter 5: Leading vowels and extended prose.
# Leading vowels (เ แ โ ไ ใ) have positive advanceX and render normally,
# but appear before their consonant in Unicode order. This chapter
# verifies they don't get mishandled by the advanceX==0 combining path.
CHAPTER_5 = """\
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="th" lang="th">
<head><title>บทที่ ๕ – สระนำ</title>
<link rel="stylesheet" type="text/css" href="style.css"/></head>
<body>
<h1>บทที่ ๕<br/>สระนำ</h1>

<h2>สระ เ-</h2>
<p>เก เข เค เง เจ เช เซ เด เต เท เน เบ เป เพ เม เย เร เล เว เส เห</p>
<p>เกาะ เขียน เคลื่อน เงียบ เจ็บ เช้า เซ็น เดิน เต้น เท่า เนื้อ เบื่อ เปิด เพลง เมือง เย็น เรือ เล่น เวลา เสื้อ เหนือ</p>

<h2>สระ แ-</h2>
<p>แก แข แค แง แจ แช แซ แด แต แท แน แบ แป แพ แม แย แร แล แว แส แห</p>
<p>แกง แข็ง แคระ แง่ แจก แช่ แซ่บ แดง แต่ แท้ แนะ แบน แปรง แพง แม่ แยก แรง แล้ว แวว แสง แหลม</p>

<h2>สระ โ-</h2>
<p>โกง โข โค โง โจ โช โซ โด โต โท โน โบ โป โพ โม โย โร โล โว โส โห</p>
<p>โกรธ โขดหิน โคลง โง่ โจร โชค โซ่ โดน โต้ โทร โน้ต โบราณ โปร โพธิ์ โมง โยก โรง โลก โวย โสม โหด</p>

<h2>สระ ไ- และ ใ-</h2>
<p>ไก่ ไข่ ไค ไง ไจ ไช ไซ ไทย ไป ไฟ ไม้ ไร่ ไหม ไหน</p>
<p>ใกล้ ใคร ใจ ใช้ ใต้ ใน ใบ ใส ใหญ่ ใหม่</p>

<h2>สระนำผสมในข้อความยาว</h2>

<p>เมื่อเช้าวันนี้แม่ไปตลาดแต่เช้าเพื่อซื้อผักสดๆ มาทำแกงเขียวหวาน
แม่เลือกซื้อเนื้อไก่สดแพงเล็กน้อยแต่โคตรอร่อย ไก่ไม่ใช้โตเกินไป
เพราะใช้ไก่ตัวเล็กจะได้เนื้อนุ่มกว่า แม่ไม่ชอบเนื้อแข็งๆ</p>

<p>ใครก็ได้ที่ไม่เคยไปเที่ยวเชียงใหม่ควรลองไปสักครั้ง
เชียงใหม่เป็นเมืองที่ใหญ่เป็นอันดับสองของไทย
โอบล้อมด้วยภูเขาสูงใหญ่ เมืองเก่าแก่มีโบราณสถานมากมาย
ไม่ว่าจะเป็นวัดเจดีย์หลวง วัดพระสิงห์ หรือวัดเชียงมั่น
แต่ละแห่งมีเสน่ห์เฉพาะตัวไม่เหมือนใคร</p>
</body>
</html>
"""

# Chapter 6: Pangram-style comprehensive text.
CHAPTER_6 = """\
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="th" lang="th">
<head><title>บทที่ ๖ – บทความยาว</title>
<link rel="stylesheet" type="text/css" href="style.css"/></head>
<body>
<h1>บทที่ ๖<br/>บทความยาว</h1>

<p>เป็นมนุษย์สุดประเสริฐเลิศคุณค่า กว่าบรรดาฝูงสัตว์เดรัจฉาน
จงฝ่าฟันพัฒนาวิชาการ อย่าล้างผลาญฤๅเข่นฆ่าบีฑาใคร
ไม่ถeli่อตัวดีกว่าใครหมิ่นแหนงน้อยใจ ถึงเพิ่งeli่ำก็ไม่ใช่กึ๋น
เหมือนแeli่ลโซ่ที่คอยแeli่ง</p>

<p>ภาษาไทยเป็นภาษาที่มีความงดงามทั้งในด้านเสียงและตัวอักษร
ตัวอักษรไทยมีรูปร่างอ่อนช้อยสวยงาม ระบบการเขียนเป็นแบบอักษรสระประกอบ
คือมีทั้งพยัญชนะและสระที่ประกอบกันเป็นพยางค์ สระบางตัวเขียนไว้ข้างบน
บางตัวเขียนไว้ข้างล่าง บางตัวเขียนไว้ข้างหน้า และบางตัวเขียนไว้ข้างหลัง
ของพยัญชนะ ทำให้ระบบการเขียนภาษาไทยมีความซับซ้อนและน่าสนใจ</p>

<p>วรรณคดีไทยมีประวัติยาวนานหลายร้อยปี ผลงานเด่นๆ ได้แก่
สุนทรภู่ผู้ประพันธ์พระอภัยมณี รามเกียรติ์ซึ่งดัดแปลงมาจาก
รามายณะของอินเดีย อิเหนาซึ่งเป็นวรรณคดีที่ได้รับอิทธิพลจากชวา
และขุนช้างขุนแผนซึ่งเป็นวรรณคดีพื้นบ้าน ผลงานเหล่านี้แสดงให้เห็น
ถึงความสามารถในการใช้ภาษาและจินตนาการอันยอดเยี่ยมของกวีไทย</p>

<p>ดนตรีไทยมีเอกลักษณ์เฉพาะตัวที่โดดเด่น เครื่องดนตรีไทยแบ่งเป็น
สี่ประเภทหลักคือ เครื่องดีด เครื่องสี เครื่องตี และเครื่องเป่า
ระนาดเอกเป็นเครื่องตีที่มีเสียงไพเราะ ซอด้วงเป็นเครื่องสีที่มี
เสียงนุ่มนวล ขลุ่ยเป็นเครื่องเป่าที่ให้เสียงอ่อนหวาน
และจะเข้เป็นเครื่องดีดที่มีสามสาย</p>

<blockquote><p>ในน้ำมีปลา ในนามีข้าว — สุภาษิตไทย</p></blockquote>

<p>อาหารไทยแต่ละภาคมีรสชาติเฉพาะตัว ภาคเหนือมีน้ำพริกอ่อง ขนมจีนน้ำเงี้ยว
และไส้อั่ว ภาคอีสานมีส้มตำ ลาบ และก้อย ภาคกลางมีแกงเขียวหวาน ต้มยำ
และผัดไทย ภาคใต้มีแกงเหลือง แกงไตปลา และข้าวยำ แต่ละจานมีส่วนผสมของ
สมุนไพรไทยที่หลากหลาย เช่น ตะไคร้ ข่า ใบมะกรูด พริกขี้หนู กระเทียม
หอมแดง มะนาว และน้ำปลา</p>
</body>
</html>
"""

COVER_XHTML = """\
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="th" lang="th">
<head><title>Cover</title>
<style>
body { margin: 0; padding: 0; text-align: center; }
img { max-width: 100%; max-height: 100%; }
</style>
</head>
<body>
<img src="cover.jpg" alt="Thai Rendering Edge Cases"/>
</body>
</html>
"""

STYLESHEET = """\
body {
  font-family: serif;
  margin: 2em;
  line-height: 1.8;
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
    <dc:language>th</dc:language>
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
  </spine>
</package>
"""

TOC_XHTML = """\
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops"
      xml:lang="th" lang="th">
<head><title>สารบัญ</title>
<link rel="stylesheet" type="text/css" href="style.css"/></head>
<body>
<h1>สารบัญ</h1>
<nav epub:type="toc">
  <ol>
    <li><a href="chapter1.xhtml">บทที่ ๑ – อักษรพื้นฐาน</a></li>
    <li><a href="chapter2.xhtml">บทที่ ๒ – สระผสมวรรณยุกต์</a></li>
    <li><a href="chapter3.xhtml">บทที่ ๓ – การตัดคำ</a></li>
    <li><a href="chapter4.xhtml">บทที่ ๔ – ไทยผสมอังกฤษ</a></li>
    <li><a href="chapter5.xhtml">บทที่ ๕ – สระนำ</a></li>
    <li><a href="chapter6.xhtml">บทที่ ๖ – บทความยาว</a></li>
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
    print(f"EPUB written to {output_path}")


if __name__ == "__main__":
    project_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = os.path.join(project_root, "test", "epubs", "test_thai.epub")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    build_epub(out)
