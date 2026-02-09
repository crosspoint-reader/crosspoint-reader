# Proposal: Background Thumbnail & Async SD Tasks

## Current State

- **Thumb generation takes ~10s** per cover (JPEG decode + Atkinson dither → 1bpp BMP), blocking the home screen.
- We have similar issues with loading chapters (when entering a new chapter), generating sleep screen covers (blocks going to sleep, not critical).
- **One SPI bus** shared between SD (CS=12) and e-ink display (CS=21). This concurrency needs to be considered, as all background tasks need access to SD.
- **SdFat 2.3.1** has no thread safety (no internal locks, shared 512B block cache). No drop-in thread-safe alternative exists with exFAT + UTF-8 long filename support.
- SD access is single-threaded by convention, not enforcement. All activities use a `renderingMutex` for display tasks, which incidentally serializes SD access.
- The JPEG converter uses picojpeg with streaming I/O: 512B reads, ~32B row writes, 5-15ms CPU between I/O calls. A typical 2000x1000 cover (4:2:0) has ~63 MCU rows — natural chunk boundaries at ~150ms intervals.

## Requirements

1. Thumb generation must not block the UI.
2. User can navigate freely during generation (home, reader, settings, etc.).
3. Rapid re-triggering must be handled (user opens several books in a row).
4. Should generalize to other async SD tasks (see below).

## Future Use Cases

- **Sleep screen cover** — 2bpp JPEG→BMP, same pipeline, ~10s.
- **Chapter pre-caching** — `Section::createSectionFile()` parses HTML from EPUB ZIP, builds page layout cache. Currently blocks on chapter transitions.
- **Book metadata indexing** — scanning new books on SD for library view.

## Option A: Thread-safe SD Wrapper

Wrap all SD access (`SDCardManager` methods + every `FsFile.read()`/`write()`/`seek()`/`close()`) with a global mutex. The thumb converter runs as a low-priority FreeRTOS task with a work queue.

Locking must be at the individual read/write granularity — not open/close — because the JPEG-BMP converter keeps both input and output files open for the full 10s. Coarser locking would block all SD access for the entire conversion, defeating the purpose.

SPI bus timing is favorable: SD transactions take ~0.1ms with 5-15ms CPU gaps between them, so the mutex is released frequently and other tasks wait negligibly.

**Pros:**
- True background execution
- Simple task model (FreeRTOS task + work queue)
- Generalizes to any background task (chapter pre-caching, metadata indexing)
- One-time investment that makes the platform safely multi-task capable

**Cons:**
- Must audit & wrap every existing `FsFile` call site across the codebase
- Every future SD call must also go through the wrapper
- Risk of deadlocks if locking discipline isn't followed
- Fine-grained locking (per read/write) adds overhead and complexity

## Option B: Cooperative State Machine

Refactor the converter into a resumable state machine that processes one MCU row per call (~150ms of work). A singleton `ThumbGenerator` persists across activities. Each activity's `displayTaskLoop` calls `generator.doOneChunk()` during idle cycles (when `updateRequired` is false). No concurrency, no SD contention.

**Pros:**
- Zero SD contention — no mutex, no wrapper, no locking anywhere
- No changes to existing SD code or SDCardManager
- Cannot deadlock
- Natural fit for single-core ESP32-C3

**Cons:**
- Converter must be restructured into a resumable state machine
- picojpeg is not natively suspendable; yield granularity is MCU rows (~63 per image)
- Each activity must opt in (add ~3 lines to its display loop)
- Generation pauses during activity transitions (brief gaps, no display loop running)
- Future use cases (chapter pre-caching) require their own state machines; harder for deep call stacks (XML parser + layout engine). Possible (e.g. 1-10 pages per section indexing chunk), but more difficult.
