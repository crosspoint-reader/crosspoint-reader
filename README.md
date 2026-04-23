# CrossPoint Reader ++

This firmware is based on the [crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader) for the XTEINK X4, a great piece of software by Dave Allie and others

# Why this fork

Unfortunately the official repository suffers from too many good ideas floating around and a lack of clear governance how to deal 
with these contributions, so it's lacking fundamental fixes for a proper reading experience (rendering issues, sub-par sync 
capabilities with KOReader, a popular multi-platform open-source epub reader). 

Trying to contribute enhancements / fixes became increasingly difficult as the lack of progress effectively prevented further development.

Therefore this branch focuses on real fixes and real improvements while trying to keep up to pace with developments in the main branch.

# What's different

- Proper KOReader Snychronisation (including https TLS OOM fix)
- Fixes for a lot of css rendering issues
- Additional sleep screens support (information overlay, transparent pictures over current reader screen)
- Clock-Support 
- Weather information panel
- Multiple under-the-hood performance improvements
- Book information screen
- Markdown-support
- WiFi captive portal support
- ...

# Choosing the _right_ reader...

Your usecase might be completely different from mine, so I try to give an overview of the different reader flavors to my best knowledge. If you know of more variants / have more information, then let me know

Last update: April, 12th, 2026

| Reader                               | Visual appeal | Functionality | Formats | Pros | Cons | Custom fonts | CJK | Bluetooth |
| ------------------------------------ | ------------- | ------------- | ------- | ---- | ---- | ------------ | --- | --- |
| [Stock](https://www.xteink.com)       | Okay          | Reader        | XTC, EPUB, TXT | Frequent official updates | Lot of rendering issues | Yes | Yes | Yes |
| [THIS FORK: CrossPoint ++](https://github.com/jpirnay/crosspoint-reader) | Okay | Calibre Wireless support; Proper KOReader progress sync; Wi-Fi transfer; Book Info; Clock, Weather Info | EPUB, XTC, TXT, MD | Faster integration of functionality | Small team | No | No | No |
| [CrossPoint](https://github.com/crosspoint-reader/crosspoint-reader) | Okay | Calibre Wireless support; KOReader progress sync; Wi-Fi transfer | EPUB, XTC, TXT | Biggest community | Least common denominator approach | No | No | No |
| [CrossPet](https://github.com/trilwu/crosspet) | Excellent, Playful | Virtual pet motivator; mini-games | EPUB, TXT | A lot of additional apps | Higher battery drain, Small team, bloaty | Yes | Yes | Yes |
| [Papyrix](https://github.com/bigbag/papyrix-reader) | Minimalist | Calibre Wireless support; exFAT support | EPUB, FB2, MD, TXT | A lot of good ideas | Small team | Yes | Yes | No |
| [Inx](https://github.com/obijuankenobiii/inx) | Nice | mainly crosspoint functionality plus reading statistics | EPUB, XTC, TXT | Good reading stats | Small team | No | No | No |
| [vCodex](https://github.com/franssjz/cpr-vcodex) | Nice | mainly crosspoint functionality plus reading statistics | EPUB, XTC, TXT | Good reading stats | Small team | No | No | No |
| [PlusPoint](https://github.com/ngxson/pluspoint-reader) | Okay | Experimental | EPUB, TXT, JS Apps | Support for custom JS apps; better RTC | Based on older code, small team | Yes | Yes | No |
| [SUMI](https://github.com/psychoplath9450/SUMI) | Nice | Lua custom apps; Bluetooth keyboard support for notes; Game Boy emulator; Bluetooth Transfer | EPUB, TXT, XTC, MD | Many apps, customizable, Papyrix fork | Single Contributor, **No Wi-Fi** | Yes | Yes | Yes |
