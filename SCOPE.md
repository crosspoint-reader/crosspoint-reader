# Scope

A personal e-reader OS for Xteink (X3, X4, X4 Pro, X4 Classic), EEGO A4 and
Seeed reTerminal Sticky devices.

## Goal

A feature-rich but minimal OS for **reading and learning**. Features are
welcome when they help you read, understand, or remember what you read.

## In scope

- Reading: EPUB/TXT/XTC rendering, typography, fonts, navigation, bookmarks.
- Learning: dictionary, vocabulary, highlights/clippings, notes, review tools
  (for example flashcards), reading stats.
- Getting books on and notes off: file transfer, OPDS, sync.
- Small utilities that support reading (clock, sleep screens, settings).

## Rules

- Must run on the X3/X4 (ESP32-C3, no PSRAM). Bigger features may be gated
  to the PSRAM boards, but must cost nothing on the C3 build.
- Prefer offline features. Network features are fine, but keep them optional.
- Minimal means simple and focused, not few features: one clear way to do
  each thing, and no general-purpose apps unrelated to reading or learning.
