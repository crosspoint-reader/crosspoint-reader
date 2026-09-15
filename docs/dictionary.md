# Dictionary lookup

You can look up words in an EPUB without an internet connection. A dictionary is a set of word definitions that you save on the SD card.

## Get a dictionary

CrossPoint uses **StarDict** dictionaries. Search for "StarDict format" or for files named `.dict`, `.idx`, and `.ifo`.

A dictionary folder must contain:

- `.idx` is the word list. It is required and must not be compressed. A `.idx.gz` file does not work.
- `.dict` or `.dict.dz` contains the definitions.
- `.syn` is optional. It links alternative spellings and word forms to a main word.
- `.ifo` is optional. It describes the dictionary.

CrossPoint does not support dictionaries with `idxoffsetbits=64` in the `.ifo` file.

## Put it on the reader

1. Copy each dictionary folder to `/dictionaries/` on the SD card. Use one dictionary per folder. For example, use `/dictionaries/webster/webster.idx` and `webster.dict.dz`.
2. Open **Settings → Reader → Dictionary** on the device.
3. Select a dictionary from the list, or **None** to disable lookups.

The Dictionary setting appears only when CrossPoint finds a usable dictionary. CrossPoint ignores a folder that contains more than one dictionary.

## Look up a word

You can start a lookup in two ways:

- Open the reader menu (**Confirm**) and choose **Look Up**.
- Or set **Settings → Controls → Long-press Menu** to "Dictionary", then hold **Confirm** (~0.4s) on the reading page.

CrossPoint highlights one word on the page:

1. Use **Left/Right** to move between words in reading order, and the side **Up/Down** buttons to jump between lines.
2. Press **Confirm** to look up the highlighted word.
3. Press **Back** to return to the reader.

The first lookup can show *"Indexing dictionary…"*. CrossPoint creates small helper files beside the dictionary. This can take a few seconds for a large dictionary. Later lookups are faster. You can delete the `.qidx` and `.sidx` helper files at any time. CrossPoint creates them again when needed.

### If the word is not found

CrossPoint ignores letter case and punctuation around a word. If the dictionary includes a `.syn` file, it also tries the alternatives in that file. CrossPoint then tries common English forms, such as `dogs` for `dog` and `walked` for `walk`. If it cannot find the word, it shows a short message and returns to word selection.

## Read a definition

The definition screen shows the matched word at the top. Long definitions have a page number.

Some HTML dictionaries show headings, bold text, italics, lists, and line breaks. Images and style details do not appear. A very large definition can appear as plain text.

- **Left/Right** or side **Up/Down**: Previous or next page.
- **Back**: Return to word selection.



## Find dictionaries

The KOReader Dictionary Support page helped compile this list.

- The [reader.dict](https://www.reader-dict.com) (ex "BoboTiG/ebook-reader-dict") project provides StarDict version of daily dumps of [Wiktionary](https://www.wiktionary.org/) monolingual dictionaries for a variety of languages. It also provides [non-free multilingual](https://www.reader-dict.com) dictionaries.
- The [WikDict](https://www.wikdict.com) project provides free bilingual dictionaries based on [Wiktionary](https://www.wiktionary.org/) for a lot of language pairs. StarDict versions can be [downloaded from here](https://download.wikdict.com/dictionaries/stardict/).
- The [`Vuizur/Wiktionary-Dictionaries`](https://github.com/Vuizur/Wiktionary-Dictionaries) repository contains dictionaries based on [Wiktionary](https://www.wiktionary.org/) from many languages to English, including English-English.
- The [DictInfo](https://www.dictinfo.com/) website provides outdated monolingual dictionaries based on [Wiktionary](https://www.wiktionary.org/).
- The [Firedict site](https://tuxor1337.frama.io/firedict/dictionaries.html) contains a list of freely available dictionaries.
- [wiktionary_stardict](https://xxyzz.github.io/wiktionary_stardict/): update monthly.
- [Fictionaries](https://fictionary.gumroad.com/) provides dictionaries for various speculative fiction books and series.
- [World Factbooks Archive](https://github.com/MilkMp/CIA-World-Factbooks-Archive-1990-2025) provides 36 years of CIA's World Factbook dictionaries in StarDict format.
- [StarDict-Hebrew](https://github.com/Uri-Tauber/StarDict-Hebrew) Hebrew-English StarDict versions of Babylon dictionaries.
