#!/usr/bin/env python3
"""
Generate a full-style English→Spanish StarDict dictionary.

Output: dictionaries/en-es/
  en-es.ifo        — metadata
  en-es.dict       — definitions (Spanish translations, plain text)
  en-es.idx        — index (English headwords, sorted)
  en-es.idx.oft    — index offset table (stride 32)
  en-es.syn        — synonyms (alternate English forms → canonical headword ordinal)
  en-es.syn.oft    — synonym offset table (stride 32)

Format references (from project memory / verified against real dicts):
  .ifo   : key=value text; version=stardict-2.4.2
  .idx   : [word\0][uint32 offset BE][uint32 size BE], sorted lexicographically
  .syn   : [synonym\0][uint32 idx_ordinal BE], sorted lexicographically
  .oft   : 38-byte header + LE uint32 offsets at stride=32
            header = b"StarDict's Cache, Version: 0.2" (30 bytes, no null)
                   + b"\xc1\xd1\xa4\x51\x00\x00\x00\x00" (8 bytes fixed magic)
            entry N = byte offset of word N*32 in .idx (or .syn)
            word 0 is NOT stored; first entry covers ordinal 32
  sametypesequence=m : plain text; size from .idx size field (no null terminator)
"""

import os
import struct
import gzip

# ---------------------------------------------------------------------------
# Headword list: (english_headword, spanish_translation)
# Spread across the alphabet; 60 entries ensures stride-32 OFT has 1 data entry.
# ---------------------------------------------------------------------------
ENTRIES = [
    ("afternoon",   "tarde (f)"),
    ("airplane",    "avión (m)"),
    ("angry",       "enojado / enfadado"),
    ("animal",      "animal (m)"),
    ("answer",      "respuesta (f) / responder"),
    ("apple",       "manzana (f)"),
    ("arm",         "brazo (m)"),
    ("arrive",      "llegar"),
    ("ask",         "preguntar"),
    ("autumn",      "otoño (m)"),
    ("baby",        "bebé (m/f)"),
    ("beautiful",   "hermoso / bello"),
    ("begin",       "comenzar / empezar"),
    ("bird",        "pájaro (m)"),
    ("blue",        "azul"),
    ("book",        "libro (m)"),
    ("bread",       "pan (m)"),
    ("brother",     "hermano (m)"),
    ("buy",         "comprar"),
    ("car",         "coche (m) / carro (m)"),
    ("cat",         "gato (m)"),
    ("child",       "niño (m) / niña (f)"),
    ("city",        "ciudad (f)"),
    ("clean",       "limpio / limpiar"),
    ("cloud",       "nube (f)"),
    ("cold",        "frío"),
    ("colour",      "color (m)"),
    ("come",        "venir"),
    ("cook",        "cocinar / cocinero (m)"),
    ("country",     "país (m) / campo (m)"),
    ("cry",         "llorar"),
    ("dance",       "bailar / baile (m)"),
    ("dark",        "oscuro"),
    ("daughter",    "hija (f)"),
    ("day",         "día (m)"),
    ("death",       "muerte (f)"),
    ("door",        "puerta (f)"),
    ("dream",       "sueño (m) / soñar"),
    ("drink",       "beber / bebida (f)"),
    ("drive",       "conducir / manejar"),
    ("ear",         "oreja (f) / oído (m)"),
    ("earth",       "tierra (f)"),
    ("easy",        "fácil"),
    ("eat",         "comer"),
    ("evening",     "noche (f) / tarde (f)"),
    ("eye",         "ojo (m)"),
    ("face",        "cara (f) / rostro (m)"),
    ("fall",        "caer / otoño (m)"),
    ("family",      "familia (f)"),
    ("fast",        "rápido"),
    ("father",      "padre (m)"),
    ("fear",        "miedo (m) / temer"),
    ("finger",      "dedo (m)"),
    ("fire",        "fuego (m) / incendio (m)"),
    ("fish",        "pez (m) / pescado (m)"),
    ("flower",      "flor (f)"),
    ("fly",         "volar / mosca (f)"),
    ("food",        "comida (f) / alimento (m)"),
    ("forest",      "bosque (m)"),
    ("friend",      "amigo (m) / amiga (f)"),
    ("garden",      "jardín (m)"),
    ("give",        "dar"),
    ("glass",       "vaso (m) / vidrio (m)"),
    ("go",          "ir"),
    ("good",        "bueno"),
    ("green",       "verde"),
    ("grow",        "crecer"),
    ("hand",        "mano (f)"),
    ("happy",       "feliz / contento"),
    ("hate",        "odiar / odio (m)"),
    ("head",        "cabeza (f)"),
    ("hear",        "oír / escuchar"),
    ("heart",       "corazón (m)"),
    ("help",        "ayudar / ayuda (f)"),
    ("high",        "alto"),
    ("home",        "hogar (m) / casa (f)"),
    ("horse",       "caballo (m)"),
    ("hot",         "caliente / caluroso"),
    ("house",       "casa (f)"),
    ("hundred",     "cien / ciento"),
    ("husband",     "esposo (m) / marido (m)"),
    ("ice",         "hielo (m)"),
    ("idea",        "idea (f)"),
    ("important",   "importante"),
    ("interesting", "interesante"),
    ("job",         "trabajo (m) / empleo (m)"),
    ("jump",        "saltar"),
    ("keep",        "guardar / mantener"),
    ("key",         "llave (f) / clave (f)"),
    ("king",        "rey (m)"),
    ("know",        "saber / conocer"),
    ("language",    "idioma (m) / lengua (f)"),
    ("large",       "grande"),
    ("laugh",       "reír / risa (f)"),
    ("learn",       "aprender"),
    ("leave",       "salir / dejar"),
    ("light",       "luz (f) / ligero"),
    ("love",        "amor (m) / amar"),
    ("man",         "hombre (m)"),
    ("money",       "dinero (m)"),
    ("moon",        "luna (f)"),
    ("morning",     "mañana (f)"),
    ("mother",      "madre (f)"),
    ("mountain",    "montaña (f)"),
    ("mouth",       "boca (f)"),
    ("name",        "nombre (m)"),
    ("new",         "nuevo"),
    ("night",       "noche (f)"),
    ("nose",        "nariz (f)"),
    ("ocean",       "océano (m)"),
    ("old",         "viejo / antiguo"),
    ("open",        "abrir / abierto"),
    ("orange",      "naranja (f)"),
    ("pain",        "dolor (m)"),
    ("peace",       "paz (f)"),
    ("person",      "persona (f)"),
    ("play",        "jugar / tocar / obra (f)"),
    ("please",      "por favor / agradar"),
    ("poor",        "pobre"),
    ("power",       "poder (m) / potencia (f)"),
    ("put",         "poner"),
    ("question",    "pregunta (f)"),
    ("quick",       "rápido"),
    ("quiet",       "silencioso / tranquilo"),
    ("rain",        "lluvia (f) / llover"),
    ("read",        "leer"),
    ("red",         "rojo"),
    ("remember",    "recordar"),
    ("rich",        "rico"),
    ("right",       "derecho / correcto"),
    ("river",       "río (m)"),
    ("road",        "camino (m) / carretera (f)"),
    ("run",         "correr"),
    ("sad",         "triste"),
    ("say",         "decir"),
    ("sea",         "mar (m/f)"),
    ("see",         "ver"),
    ("sell",        "vender"),
    ("send",        "enviar / mandar"),
    ("short",       "corto / bajo"),
    ("sing",        "cantar"),
    ("sister",      "hermana (f)"),
    ("sit",         "sentarse"),
    ("sky",         "cielo (m)"),
    ("sleep",       "dormir / sueño (m)"),
    ("slow",        "lento"),
    ("small",       "pequeño"),
    ("smile",       "sonreír / sonrisa (f)"),
    ("snow",        "nieve (f) / nevar"),
    ("son",         "hijo (m)"),
    ("song",        "canción (f)"),
    ("speak",       "hablar"),
    ("spring",      "primavera (f) / muelle (m)"),
    ("star",        "estrella (f)"),
    ("stop",        "parar / dejar de"),
    ("strong",      "fuerte"),
    ("summer",      "verano (m)"),
    ("sun",         "sol (m)"),
    ("swim",        "nadar"),
    ("table",       "mesa (f)"),
    ("talk",        "hablar / conversar"),
    ("tall",        "alto"),
    ("thank",       "agradecer"),
    ("think",       "pensar"),
    ("time",        "tiempo (m) / vez (f)"),
    ("tired",       "cansado"),
    ("today",       "hoy"),
    ("tree",        "árbol (m)"),
    ("true",        "verdadero / cierto"),
    ("understand",  "entender / comprender"),
    ("wait",        "esperar"),
    ("walk",        "caminar / pasear"),
    ("want",        "querer / desear"),
    ("warm",        "cálido / tibio"),
    ("water",       "agua (f)"),
    ("weak",        "débil"),
    ("white",       "blanco"),
    ("wife",        "esposa (f) / mujer (f)"),
    ("wind",        "viento (m)"),
    ("window",      "ventana (f)"),
    ("winter",      "invierno (m)"),
    ("woman",       "mujer (f)"),
    ("word",        "palabra (f)"),
    ("work",        "trabajar / trabajo (m)"),
    ("world",       "mundo (m)"),
    ("write",       "escribir"),
    ("year",        "año (m)"),
    ("yellow",      "amarillo"),
    ("young",       "joven"),
    ("zebra",       "cebra (f)"),
]

# ---------------------------------------------------------------------------
# Synonyms: (synonym_form, canonical_headword)
# Alternate English spellings, verb forms, British variants, etc.
# At least 33 entries so .syn.oft has 1 non-header data entry.
# ---------------------------------------------------------------------------
SYNONYMS = [
    ("aerial",      "airplane"),
    ("aged",        "old"),
    ("air",         "fly"),
    ("angry",       "angry"),       # self-ref — will be filtered if same idx ordinal
    ("appreciate",  "thank"),
    ("auto",        "car"),
    ("automobile",  "car"),
    ("autumn",      "fall"),        # BrE "autumn" → headword "fall"
    ("bird",        "fly"),         # a bird can fly
    ("blonde",      "yellow"),
    ("boat",        "sea"),
    ("bored",       "tired"),       # close enough for a test dict
    ("boy",         "son"),
    ("brave",       "strong"),
    ("chat",        "talk"),
    ("child",       "son"),
    ("chilly",      "cold"),
    ("choose",      "want"),
    ("color",       "colour"),      # AmE → BrE headword
    ("commence",    "begin"),
    ("comprehend",  "understand"),
    ("converse",    "talk"),
    ("correct",     "right"),
    ("cottage",     "house"),
    ("dad",         "father"),
    ("depart",      "leave"),
    ("desire",      "want"),
    ("difficult",   "important"),   # approximate for testing
    ("dim",         "dark"),
    ("discover",    "know"),
    ("dislike",     "hate"),
    ("dog",         "animal"),
    ("drought",     "rain"),
    ("dwell",       "home"),
    ("elderly",     "old"),
    ("emotion",     "love"),
    ("employ",      "job"),
    ("enjoy",       "happy"),
    ("enormous",    "large"),
    ("exhausted",   "tired"),
    ("express",     "say"),
    ("eye",         "face"),
    ("fall",        "autumn"),      # AmE "fall" → headword "autumn"
    ("father",      "man"),
    ("female",      "woman"),
    ("finish",      "stop"),
    ("fog",         "cloud"),
    ("freeze",      "cold"),
    ("gaze",        "see"),
    ("girl",        "daughter"),
    ("glad",        "happy"),
    ("globe",       "world"),
    ("grin",        "smile"),
    ("halt",        "stop"),
    ("home",        "house"),
    ("huge",        "large"),
    ("hungry",      "eat"),
    ("ill",         "pain"),
    ("income",      "money"),
    ("infant",      "baby"),
    ("joyful",      "happy"),
    ("journey",     "road"),
    ("kid",         "child"),
    ("lady",        "woman"),
    ("lake",        "water"),
    ("lamp",        "light"),
    ("laughter",    "laugh"),
    ("lengthy",     "large"),
    ("listen",      "hear"),
    ("little",      "small"),
    ("lively",      "fast"),
    ("mad",         "angry"),
    ("male",        "man"),
    ("mama",        "mother"),
    ("massive",     "large"),
    ("melody",      "song"),
    ("mom",         "mother"),
    ("mum",         "mother"),
    ("narrative",   "story"),
    ("ocean",       "sea"),
    ("odor",        "smell"),
    ("offspring",   "child"),
    ("old",         "autumn"),      # just for extra coverage
    ("papa",        "father"),
    ("path",        "road"),
    ("pet",         "animal"),
    ("planet",      "earth"),
    ("pond",        "water"),
    ("purchase",    "buy"),
    ("question",    "ask"),
    ("quit",        "stop"),
    ("race",        "run"),
    ("rapid",       "fast"),
    ("recollect",   "remember"),
    ("reply",       "answer"),
    ("reside",      "home"),
    ("respond",     "answer"),
    ("scarlet",     "red"),
    ("school",      "learn"),
    ("shout",       "cry"),
    ("sibling",     "brother"),
    ("slumber",     "sleep"),
    ("solar",       "sun"),
    ("sorrow",      "sad"),
    ("speak",       "say"),
    ("sprint",      "run"),
    ("start",       "begin"),
    ("stay",        "wait"),
    ("story",       "book"),
    ("stream",      "river"),
    ("street",      "road"),
    ("swift",       "fast"),
    ("symbol",      "word"),
    ("tiny",        "small"),
    ("toddler",     "baby"),
    ("town",        "city"),
    ("track",       "road"),
    ("travel",      "walk"),
    ("twilight",    "evening"),
    ("unhappy",     "sad"),
    ("upset",       "angry"),
    ("vehicle",     "car"),
    ("village",     "city"),
    ("wage",        "money"),
    ("wail",        "cry"),
    ("weary",       "tired"),
    ("weep",        "cry"),
    ("wet",         "rain"),
    ("wide",        "large"),
    ("wish",        "want"),
    ("woodland",    "forest"),
    ("yell",        "cry"),
]


# ---------------------------------------------------------------------------
# StarDict format helpers
# ---------------------------------------------------------------------------
OFT_HEADER = b"StarDict's Cache, Version: 0.2" + b"\xc1\xd1\xa4\x51\x00\x00\x00\x00"
assert len(OFT_HEADER) == 38
STRIDE = 32


def build_oft(offsets_at_stride):
    """Build .oft file bytes given list of byte offsets at stride boundaries."""
    data = OFT_HEADER
    for off in offsets_at_stride:
        data += struct.pack("<I", off)
    return data


def write_idx(entries_sorted):
    """
    Build .idx binary and return (idx_bytes, list_of_(offset_in_dict, size)).
    entries_sorted: list of (headword_str, definition_str) already sorted.
    Returns: (idx_bytes, dict_offsets) where dict_offsets[i] = (off, size).
    """
    idx = b""
    dict_bytes = b""
    dict_offsets = []
    for word, defn in entries_sorted:
        defn_b = defn.encode("utf-8")
        off = len(dict_bytes)
        size = len(defn_b)
        dict_bytes += defn_b
        idx += word.encode("utf-8") + b"\x00"
        idx += struct.pack(">II", off, size)
        dict_offsets.append((off, size))
    return idx, dict_bytes, dict_offsets


def write_syn(syn_sorted, headword_to_ordinal):
    """
    Build .syn binary.
    syn_sorted: list of (synonym_str, canonical_headword_str) sorted by synonym.
    headword_to_ordinal: dict mapping headword → its 0-based position in sorted .idx.
    Skips synonyms whose canonical headword isn't in the index.
    """
    syn = b""
    valid = []
    for syn_word, canonical in syn_sorted:
        if canonical not in headword_to_ordinal:
            print(f"  SKIP synonym '{syn_word}' → '{canonical}' (canonical not in idx)")
            continue
        ordinal = headword_to_ordinal[canonical]
        syn += syn_word.encode("utf-8") + b"\x00"
        syn += struct.pack(">I", ordinal)
        valid.append((syn_word, canonical, ordinal))
    return syn, valid


def oft_offsets_for(binary, stride):
    """
    Walk through the binary data (idx or syn format) and collect byte offsets
    of every stride-th entry (0-indexed: entry 0 not stored, first entry = #stride).
    Returns list of offsets.
    """
    offsets = []
    entry_count = 0
    pos = 0
    while pos < len(binary):
        # Find null terminator for the word
        null = binary.index(b"\x00", pos)
        pos = null + 1 + 8  # skip word\0 + uint32 offset + uint32 size (for idx)
        entry_count += 1
        if entry_count % stride == 0:
            offsets.append(pos)
    return offsets


def oft_offsets_for_syn(binary, stride):
    """Same as oft_offsets_for but syn entries have word\0 + uint32 (4 bytes)."""
    offsets = []
    entry_count = 0
    pos = 0
    while pos < len(binary):
        null = binary.index(b"\x00", pos)
        pos = null + 1 + 4  # skip word\0 + uint32 ordinal
        entry_count += 1
        if entry_count % stride == 0:
            offsets.append(pos)
    return offsets


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    out_dir = os.path.join(os.path.dirname(__file__), "..", "dictionaries", "en-es")
    os.makedirs(out_dir, exist_ok=True)
    stem = os.path.join(out_dir, "en-es")

    # Sort entries lexicographically (case-sensitive, as StarDict requires)
    entries_sorted = sorted(ENTRIES, key=lambda e: e[0])

    # Remove duplicate headwords (keep first occurrence after sort)
    seen = set()
    unique_entries = []
    for e in entries_sorted:
        if e[0] not in seen:
            seen.add(e[0])
            unique_entries.append(e)
    entries_sorted = unique_entries

    # Build headword → ordinal map
    headword_to_ordinal = {e[0]: i for i, e in enumerate(entries_sorted)}

    print(f"Headwords: {len(entries_sorted)}")

    # Build .idx and .dict
    idx_bytes, dict_bytes, dict_offsets = write_idx(entries_sorted)

    # Sort synonyms
    syn_sorted = sorted(SYNONYMS, key=lambda s: s[0])
    # Remove duplicate synonym keys (keep first)
    seen_syn = set()
    unique_syn = []
    for s in syn_sorted:
        if s[0] not in seen_syn:
            seen_syn.add(s[0])
            unique_syn.append(s)
    syn_sorted = unique_syn

    # Build .syn
    syn_bytes, valid_syn = write_syn(syn_sorted, headword_to_ordinal)

    print(f"Synonyms:  {len(valid_syn)}")

    # Build .oft files
    idx_oft_offsets = oft_offsets_for(idx_bytes, STRIDE)
    syn_oft_offsets = oft_offsets_for_syn(syn_bytes, STRIDE)

    print(f"idx.oft data entries: {len(idx_oft_offsets)}")
    print(f"syn.oft data entries: {len(syn_oft_offsets)}")

    # Write .dict
    with open(stem + ".dict", "wb") as f:
        f.write(dict_bytes)

    # Write .idx
    with open(stem + ".idx", "wb") as f:
        f.write(idx_bytes)

    # Write .idx.oft
    with open(stem + ".idx.oft", "wb") as f:
        f.write(build_oft(idx_oft_offsets))

    # Write .syn
    with open(stem + ".syn", "wb") as f:
        f.write(syn_bytes)

    # Write .syn.oft
    with open(stem + ".syn.oft", "wb") as f:
        f.write(build_oft(syn_oft_offsets))

    # Write .ifo
    ifo_lines = [
        "StarDict's dict ifo file",
        "version=stardict-2.4.2",
        f"wordcount={len(entries_sorted)}",
        f"idxfilesize={len(idx_bytes)}",
        f"synwordcount={len(valid_syn)}",
        "bookname=English to Spanish",
        "sametypesequence=m",
        "author=CrossPoint Test Suite",
        "description=Common English words with Spanish translations. Includes British/American spelling synonyms and alternate word forms.",
    ]
    with open(stem + ".ifo", "w", encoding="utf-8") as f:
        f.write("\n".join(ifo_lines) + "\n")

    # Verify file sizes
    print(f"\nFiles written to {out_dir}/")
    for ext in [".ifo", ".dict", ".idx", ".idx.oft", ".syn", ".syn.oft"]:
        path = stem + ext
        size = os.path.getsize(path)
        print(f"  en-es{ext:10s}  {size:6d} bytes")


if __name__ == "__main__":
    main()
