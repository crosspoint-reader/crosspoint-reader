#!/usr/bin/env python3
"""Generate lib/OtShaper/OtSyllableMachines.h: the syllable grammars of
HarfBuzz's Indic and USE shapers (hb-ot-shaper-indic-machine.rl,
hb-ot-shaper-use-machine.rl) compiled to DFA tables.

HarfBuzz compiles these grammars with Ragel into scanners: at each position
the longest match of any pattern wins, and on a tie the pattern listed first.
This script builds the same scanner (regex -> NFA -> DFA, minimized) so the
firmware segments syllables with one table lookup per character.

    python3 scripts/gen_ot_shaper_machines.py
"""

from pathlib import Path

OUTPUT = Path(__file__).resolve().parent.parent / "lib" / "OtShaper" / "OtSyllableMachines.h"


# --- Regular expressions over category sets -------------------------------------

class Re:
    def __add__(self, other):  # concatenation
        return Seq(self, other)

    def __or__(self, other):
        return Alt(self, other)


class Sym(Re):
    def __init__(self, *cats):
        self.cats = frozenset(cats)


class Seq(Re):
    def __init__(self, *parts):
        self.parts = parts


class Alt(Re):
    def __init__(self, *parts):
        self.parts = parts


class Star(Re):
    def __init__(self, inner):
        self.inner = inner


class Eps(Re):
    pass


ANY = object()


def opt(r):
    return Alt(r, Eps())


def star(r):
    return Star(r)


def plus(r):
    return Seq(r, Star(r))


# --- Thompson NFA -------------------------------------------------------------------

class Nfa:
    def __init__(self):
        self.eps = []    # state -> [state]
        self.edges = []  # state -> [(cats or ANY, state)]

    def state(self):
        self.eps.append([])
        self.edges.append([])
        return len(self.eps) - 1

    def build(self, r):
        """Returns (start, end) states of a fragment for `r`."""
        s, e = self.state(), self.state()
        if isinstance(r, Sym):
            self.edges[s].append((r.cats, e))
        elif r is ANY:
            self.edges[s].append((ANY, e))
        elif isinstance(r, Eps):
            self.eps[s].append(e)
        elif isinstance(r, Seq):
            prev = s
            for part in r.parts:
                ps, pe = self.build(part)
                self.eps[prev].append(ps)
                prev = pe
            self.eps[prev].append(e)
        elif isinstance(r, Alt):
            for part in r.parts:
                ps, pe = self.build(part)
                self.eps[s].append(ps)
                self.eps[pe].append(e)
        elif isinstance(r, Star):
            ps, pe = self.build(r.inner)
            self.eps[s] += [ps, e]
            self.eps[pe] += [ps, e]
        else:
            raise TypeError(r)
        return s, e

    def closure(self, states):
        stack, seen = list(states), set(states)
        while stack:
            for t in self.eps[stack.pop()]:
                if t not in seen:
                    seen.add(t)
                    stack.append(t)
        return frozenset(seen)


def compile_scanner(patterns, alphabet):
    """patterns: [(regex, token)] in priority order. Returns (classes, table,
    accept) of a minimized DFA over `alphabet` (category values)."""
    nfa = Nfa()
    start = nfa.state()
    finals = {}
    for priority, (regex, token) in enumerate(patterns):
        ps, pe = nfa.build(regex)
        nfa.eps[start].append(ps)
        finals[pe] = (priority, token)

    # Categories that behave identically share one input class; categories
    # outside the grammar (only matched by ANY) form class 0.
    def signature(cat):
        return tuple(sorted((i, j) for i, es in enumerate(nfa.edges) for j, (cats, _) in enumerate(es)
                            if cats is ANY or cat in cats))
    classes = {}
    class_of = {}
    other_sig = signature(-1)
    classes[other_sig] = 0
    for cat in alphabet:
        sig = signature(cat)
        if sig not in classes:
            classes[sig] = len(classes)
        class_of[cat] = classes[sig]
    reps = {0: -1}
    for cat, cls in class_of.items():
        reps.setdefault(cls, cat)
    nclasses = len(classes)

    def step(states, cls):
        cat = reps[cls]
        out = set()
        for s in states:
            for cats, t in nfa.edges[s]:
                if cats is ANY or cat in cats:
                    out.add(t)
        return nfa.closure(out)

    def accept(states):
        best = None
        for s in states:
            if s in finals and (best is None or finals[s][0] < best[0]):
                best = finals[s]
        return best[1] if best else None

    init = nfa.closure({start})
    dstates = {init: 0}
    order = [init]
    trans = []
    i = 0
    while i < len(order):
        row = []
        for cls in range(nclasses):
            nxt = step(order[i], cls)
            if not nxt:
                row.append(-1)
                continue
            if nxt not in dstates:
                dstates[nxt] = len(order)
                order.append(nxt)
            row.append(dstates[nxt])
        trans.append(row)
        i += 1
    accepts = [accept(s) for s in order]

    # Minimize (Moore): split states by acceptance, then by transitions.
    part = {s: accepts[s] for s in range(len(order))}
    while True:
        keys = {s: (part[s], tuple(part[t] if t >= 0 else None for t in trans[s])) for s in range(len(order))}
        ids = {}
        newpart = {s: ids.setdefault(keys[s], len(ids)) for s in range(len(order))}
        if len(set(newpart.values())) == len(set(part.values())):
            break
        part = newpart
    remap = {}
    for s in range(len(order)):
        remap.setdefault(part[s], len(remap))
    # State 0 must stay the start state.
    first = remap[part[0]]
    perm = {v: v for v in remap.values()}
    perm[first], perm[0] = 0, first
    n = len(remap)
    table = [[-1] * nclasses for _ in range(n)]
    acc = [None] * n
    for s in range(len(order)):
        m = perm[remap[part[s]]]
        table[m] = [perm[remap[part[t]]] if t >= 0 else -1 for t in trans[s]]
        acc[m] = accepts[s]
    return class_of, nclasses, table, acc


# --- Grammars ----------------------------------------------------------------------

def indic_grammar():
    X, C, V, N, H, ZWNJ, ZWJ, M, SM, A, VD, PLACEHOLDER, DOTTEDCIRCLE, RS, MPst, Repha, Ra, CM, Symbol, CS, SMPst = (
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 57)
    c = Sym(C, Ra)
    n = opt(opt(Sym(ZWNJ)) + Sym(RS)) + opt(Sym(N) + opt(Sym(N)))
    z = Sym(ZWJ, ZWNJ)
    reph = (Sym(Ra) + Sym(H)) | Sym(Repha)
    sm = Sym(SM, SMPst)
    cn = c + opt(Sym(ZWJ)) + opt(n)
    symbol = Sym(Symbol) + opt(Sym(N))
    matra_group = star(z) + (Sym(M) | (opt(sm) + Sym(MPst))) + opt(Sym(N)) + opt(Sym(H))
    syllable_tail = opt(opt(z) + sm + opt(sm) + opt(Sym(ZWNJ))) + star(Sym(A, VD))
    halant_group = opt(z) + Sym(H) + opt(Sym(ZWJ) + opt(Sym(N)))
    final_halant_group = halant_group | (Sym(H) + Sym(ZWNJ))
    medial_group = opt(Sym(CM))
    halant_or_matra_group = final_halant_group | star(matra_group)
    complex_syllable_tail = star(halant_group + cn) + medial_group + halant_or_matra_group + syllable_tail
    consonant_syllable = opt(Sym(Repha, CS)) + cn + complex_syllable_tail
    vowel_syllable = opt(reph) + Sym(V) + opt(n) + (Sym(ZWJ) | complex_syllable_tail)
    standalone_cluster = ((opt(Sym(Repha, CS)) + Sym(PLACEHOLDER)) | (opt(reph) + Sym(DOTTEDCIRCLE))) + opt(n) + \
        complex_syllable_tail
    symbol_cluster = symbol + syllable_tail
    broken_cluster = opt(reph) + opt(n) + complex_syllable_tail
    # Token values: indic_syllable_type_t.
    patterns = [(consonant_syllable, 0), (vowel_syllable, 1), (standalone_cluster, 2), (symbol_cluster, 3),
                (Sym(SMPst), 5), (broken_cluster, 4), (ANY, 5)]
    return patterns, sorted({X, C, V, N, H, ZWNJ, ZWJ, M, SM, A, PLACEHOLDER, DOTTEDCIRCLE, RS, MPst, Repha, Ra, CM,
                             Symbol, CS, SMPst})


def use_grammar():
    O, B, N, GB, CGJ, SUB, H, HN, ZWNJ, WJ, R, CS, IS, Sk, G, J, SB, SE, HVM, HM, HR, RK = (
        0, 1, 4, 5, 6, 11, 12, 13, 14, 16, 18, 43, 44, 48, 49, 50, 51, 52, 53, 54, 55, 56)
    FAbv, FBlw, FPst, MAbv, MBlw, MPst, MPre, CMAbv, CMBlw, VAbv, VBlw, VPst, VPre, VMAbv, VMBlw, VMPst, VMPre = (
        24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 22, 37, 38, 39, 23)
    SMAbv, SMBlw, FMAbv, FMBlw, FMPst = 41, 42, 45, 46, 47
    h = Sym(H, HVM, IS, Sk)
    consonant_modifiers = star(Sym(CMAbv)) + star(Sym(CMBlw)) + \
        star(((h + Sym(B)) | Sym(SUB)) + star(Sym(CMAbv)) + star(Sym(CMBlw)))
    medial_consonants = opt(Sym(MPre)) + opt(Sym(MAbv)) + opt(Sym(MBlw)) + opt(Sym(MPst))
    dependent_vowels = (star(Sym(VPre)) + star(Sym(VAbv)) + star(Sym(VBlw)) + star(Sym(VPst))) | Sym(H)
    vowel_modifiers = opt(Sym(HVM)) + star(Sym(VMPre)) + star(Sym(VMAbv)) + star(Sym(VMBlw)) + star(Sym(VMPst))
    final_consonants = star(Sym(FAbv)) + star(Sym(FBlw)) + star(Sym(FPst))
    final_modifiers = (star(Sym(FMAbv)) + star(Sym(FMBlw))) | opt(Sym(FMPst))
    complex_syllable_start = opt(Sym(R, CS)) + Sym(B, GB)
    complex_syllable_middle = consonant_modifiers + medial_consonants + dependent_vowels + vowel_modifiers + \
        star(Sym(Sk) + Sym(B))
    complex_syllable_tail = complex_syllable_middle + final_consonants + final_modifiers
    number_joiner_terminated_cluster_tail = star(Sym(HN) + Sym(N)) + Sym(HN)
    numeral_cluster_tail = plus(Sym(HN) + Sym(N))
    symbol_cluster_tail = (plus(Sym(SMAbv)) + star(Sym(SMBlw))) | plus(Sym(SMBlw))
    virama_terminated_cluster_tail = consonant_modifiers + Sym(IS, RK)
    virama_terminated_cluster = complex_syllable_start + virama_terminated_cluster_tail
    sakot_terminated_cluster_tail = complex_syllable_middle + Sym(Sk)
    sakot_terminated_cluster = complex_syllable_start + sakot_terminated_cluster_tail
    standard_cluster = complex_syllable_start + complex_syllable_tail
    tail = complex_syllable_tail | sakot_terminated_cluster_tail | symbol_cluster_tail | virama_terminated_cluster_tail
    broken_cluster = opt(Sym(R)) + (tail | number_joiner_terminated_cluster_tail | numeral_cluster_tail)
    number_joiner_terminated_cluster = Sym(N) + number_joiner_terminated_cluster_tail
    numeral_cluster = Sym(N) + opt(numeral_cluster_tail)
    symbol_cluster = Sym(O, GB, SB) + opt(tail)
    hieroglyph_unit = Sym(G) + opt(Sym(HR)) + opt(Sym(HM)) + star(Sym(SE))
    hieroglyph_cluster = star(Sym(SB)) + hieroglyph_unit + star(Sym(J) + star(Sym(SB)) + opt(hieroglyph_unit))
    z = opt(Sym(ZWNJ))
    # Token values: use_syllable_type_t.
    patterns = [(virama_terminated_cluster + z, 0), (sakot_terminated_cluster + z, 1), (standard_cluster + z, 2),
                (number_joiner_terminated_cluster + z, 3), (numeral_cluster + z, 4), (symbol_cluster + z, 5),
                (hieroglyph_cluster + z, 6), (Sym(FMPst), 8), (broken_cluster + z, 7), (ANY, 8)]
    alphabet = sorted({O, B, N, GB, CGJ, SUB, H, HN, ZWNJ, WJ, R, CS, IS, Sk, G, J, SB, SE, HVM, HM, HR, RK, FAbv,
                       FBlw, FPst, MAbv, MBlw, MPst, MPre, CMAbv, CMBlw, VAbv, VBlw, VPst, VPre, VMAbv, VMBlw,
                       VMPst, VMPre, SMAbv, SMBlw, FMAbv, FMBlw, FMPst})
    return patterns, alphabet


def used_categories(shift):
    """Categories (Indic: shift 13, USE: shift 23) OtUnicodeData.h assigns,
    so input classes are only spent on categories that occur."""
    import re
    text = (OUTPUT.parent / "OtUnicodeData.h").read_text()
    props = text.split("constexpr uint32_t PROPS[] = {")[1].split("struct Decomposition")[0]
    values = [int(v, 16) for v in re.findall(r"0x[0-9A-F]{8}", props)]
    return {(v >> shift) & 0x3F for v in values}


def emit(name, patterns, alphabet, source, extra=()):
    shift = 13 if name == "INDIC" else 23
    alphabet = sorted((set(alphabet) & used_categories(shift)) | set(extra))
    class_of, nclasses, table, acc = compile_scanner(patterns, alphabet)
    classes = [0] * 64
    for cat, cls in class_of.items():
        classes[cat] = cls
    lines = [f"// {source}: {len(table)} states x {nclasses} input classes.",
             f"constexpr uint8_t {name}_CLASSES[CATEGORY_COUNT] = {{{', '.join(map(str, classes))}}};",
             f"constexpr uint8_t {name}_CLASS_COUNT = {nclasses};",
             f"constexpr uint8_t {name}_TRANSITIONS[] = {{"]
    for row in table:
        lines.append("    " + ", ".join(str(t if t >= 0 else 255) for t in row) + ",")
    lines.append("};")
    lines.append(f"constexpr uint8_t {name}_ACCEPT[] = {{{', '.join(str(a if a is not None else 255) for a in acc)}}};")
    assert len(table) < 255
    return "\n".join(lines)


def main():
    # Dotted circles inserted into broken clusters carry DOTTEDCIRCLE (Indic)
    # or B (USE); repha recorded after GSUB is R, pref is VPre.
    indic = emit("INDIC", *indic_grammar(), "hb-ot-shaper-indic-machine.rl", extra=(11,))
    use = emit("USE", *use_grammar(), "hb-ot-shaper-use-machine.rl", extra=(1, 18, 22))
    OUTPUT.write_text(f"""#pragma once

// Generated by scripts/gen_ot_shaper_machines.py. Do not edit.
//
// Syllable scanners: per state and input class the next state
// (NO_TRANSITION ends the scan), per state the syllable type accepted there
// (NO_ACCEPT: none).
// Categories map to input classes through *_CLASSES.

#include <cstdint>

namespace ot::machines {{

constexpr unsigned CATEGORY_COUNT = 64;  // *_CLASSES entries; categories are below this
constexpr uint8_t NO_TRANSITION = 255;
constexpr uint8_t NO_ACCEPT = 255;

// clang-format off
{indic}

{use}
// clang-format on

}}  // namespace ot::machines
""")
    print(f"wrote {OUTPUT}")


if __name__ == "__main__":
    main()
