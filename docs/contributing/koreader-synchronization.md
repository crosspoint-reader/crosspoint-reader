# KOReader Synchronization Architecture

This document explains the intent and internal structure of the KOReader synchronization code in CrossPoint.

Scope:
- Synchronization logic that maps between CrossPoint reading position and KOReader sync payloads.
- Module boundaries and responsibilities.
- Matching rules, fallback strategy, and expected behavior.

For XPath-specific details and examples, see [koreader-sync-xpath-mapping.md](koreader-sync-xpath-mapping.md).

## Goals

The synchronization layer is designed to:
- Be robust on constrained devices (ESP32-C3 memory constraints).
- Be deterministic and debuggable when mapping positions.
- Keep transport/client logic separated from parsing/mapping logic.
- Prefer precise anchors when available, but degrade gracefully.

## Standalone Sync Lifecycle

KOReader sync no longer runs as a child activity on top of a live EPUB reader.
Instead, the reader persists a compact handoff record in `APP_STATE`, then the
activity stack is replaced with `KOReaderSyncActivity`.

Why this exists:
- HTTPS/TLS setup on ESP32-C3 is sensitive to heap fragmentation.
- Keeping the full reader activity alive underneath sync reclaimed too little
  memory and made failures harder to reason about.
- A standalone sync screen makes memory snapshots and failure modes easier to
  interpret.

Current lifecycle:
1. `EpubReaderActivity` stores the EPUB path, current local position, sync
   intent, and any future sync result slots in `APP_STATE.koReaderSyncSession`.
2. The reader activity stack is replaced with `KOReaderSyncActivity`.
3. `KOReaderSyncActivity` lazily reloads EPUB data only for mapping work and
   releases it again before network-heavy phases.
4. On completion, cancel, or failure, sync persists an outcome and reopens the
   book through the normal `ReaderActivity -> EpubReaderActivity` path.
5. `EpubReaderActivity::applyPendingSyncSession()` consumes that outcome:
   - remote-apply writes the reopen position into `progress.bin` before normal
     reader startup loads it
   - upload-complete keeps the existing local `progress.bin` unchanged
   - paragraph-level correction metadata is still carried separately because
     `progress.bin` stores only spine/page/pageCount

Memory notes:
- Reader-owned state is reclaimed by fully exiting the reader before sync.
- Sync still trims its own renderer/font caches right before TLS because sync UI
  rendering can repopulate those caches after the reader is gone.

## Data Model Mismatch

CrossPoint stores position as chapter/page-centric state.
KOReader sync payload stores position as XPath-like anchor plus percentage.

Because layout engines differ, page equality cannot be guaranteed across devices.
The synchronization strategy therefore combines:
- Structural anchor mapping (XPath).
- Percent-based fallback.
- Paragraph LUT refinement when available.

## Module Responsibilities

### Client / orchestration

- [lib/KOReaderSync/KOReaderSyncClient.cpp](../../lib/KOReaderSync/KOReaderSyncClient.cpp)
  - HTTP calls and payload exchange.

- [lib/KOReaderSync/ProgressMapper.cpp](../../lib/KOReaderSync/ProgressMapper.cpp)
  - High-level mapping from app state to KOReader payload and back.
  - Chooses XPath path or percentage fallback.

### XPath indexing facade

- [lib/KOReaderSync/ChapterXPathIndexer.h](../../lib/KOReaderSync/ChapterXPathIndexer.h)
- [lib/KOReaderSync/ChapterXPathIndexer.cpp](../../lib/KOReaderSync/ChapterXPathIndexer.cpp)
  - Public API consumed by ProgressMapper.
  - Thin facade over forward/reverse mapper internals.
  - Utility extraction helpers (DocFragment index, paragraph index).

### Forward mapping engine

- [lib/KOReaderSync/ChapterXPathForwardMapper.cpp](../../lib/KOReaderSync/ChapterXPathForwardMapper.cpp)
  - Maps intra-spine progress to XPath.
  - Emits /text()[N].M for body-level text-node locations.

### Reverse mapping engine

- [lib/KOReaderSync/ChapterXPathReverseMapper.cpp](../../lib/KOReaderSync/ChapterXPathReverseMapper.cpp)
  - Maps XPath to intra-spine progress.
  - Supports exact and tolerant matching tiers.
  - Handles /text()[N].M codepoint offsets.

### Shared parser/state/utilities

- [lib/KOReaderSync/ChapterXPathIndexerInternal.cpp](../../lib/KOReaderSync/ChapterXPathIndexerInternal.cpp)
- [lib/KOReaderSync/ChapterXPathIndexerInternal.h](../../lib/KOReaderSync/ChapterXPathIndexerInternal.h)
  - UTF-8 helpers, XPath normalization, parse runner, and chapter text-byte counting.

- [lib/KOReaderSync/ChapterXPathIndexerState.h](../../lib/KOReaderSync/ChapterXPathIndexerState.h)
  - Shared stack model and generic Expat callback adapters.
  - Common parser code pattern used by both forward/reverse engines.

## Core Logic

### Forward (CrossPoint -> KOReader)

1. Decompress one spine XHTML to a temporary file.
2. Count total visible text bytes.
3. Cache that total per spine (cache-path + spine index + href) so repeated
  mappings for the same chapter can skip the expensive counting pass.
4. Convert intra-spine progress to target visible-byte offset.
5. Stream parse and stop at target.
6. Emit anchor:
   - element XPath, or
   - /text()[N].M when in body-level text-node context.

### Reverse (KOReader -> CrossPoint)

1. Decompress one spine XHTML to a temporary file.
2. Stream parse chapter while evaluating candidate matches.
3. Resolve best tier in this order:
   - exact
   - exact-no-index
   - ancestor
   - ancestor-no-index
4. Convert resolved byte offset to intra-spine progress.

For text-node anchors /text()[N].M:
- N is treated as 1-based text node index.
- M is treated as 0-based codepoint offset.

## Fallback Strategy

When XPath mapping fails or is ambiguous:
- Fall back to percentage-driven chapter/page estimation.
- Use paragraph LUT refinement where available.

This guarantees user progress continuity even for malformed or sparse content.

## Constraints and Non-Goals

- No full DOM materialization for entire books.
- Parse only one spine item on demand.
- Keep memory usage bounded and transient.
- Do not attempt pixel-perfect page parity with KOReader.
