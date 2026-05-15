# Project Vision & Scope: Marginalia

Marginalia is a reader-first firmware and package ecosystem for Xteink X3/X4 devices.
It is based on CrossPoint Reader, but its scope is intentionally broader: the core firmware should remain a reliable
e-reader, while side-loaded packages let the community build themes, sleep screens, integrations, modules, and apps
without requiring every idea to become part of the firmware itself.

## 1. Core Mission

Provide a lightweight, high-performance e-reader firmware with a stable extension layer.
Reading stays the default experience; packages make the device more personal and experimental without weakening the
base reader.

## 2. Scope

### In Scope: Core Firmware

These features directly improve the default reader experience or keep the firmware maintainable.

- **Reading UX:** Book loading, navigation, bookmarks, page turns, settings, and predictable physical controls.
- **Document rendering:** EPUB parsing, layout, images, typography, hyphenation, and other rendering improvements.
- **Library management:** Simple local organisation for books and reading state.
- **E-ink behaviour:** Refresh strategy, ghosting management, sleep/wake behaviour, and display reliability.
- **Local transfer:** Web upload, SD-card workflows, OTA updates, and other simple local-first transfer paths.
- **Language support:** Interface translation and text rendering support for more languages.
- **Reader-adjacent tools:** Offline dictionaries, reading progress sync, and features that support reading without
  turning the base firmware into a general-purpose network client.
- **Board support:** Xteink X3/X4 first, with future ports accepted when they do not compromise the main X3/X4 path.

### In Scope: Package Ecosystem

These features are first-class in Marginalia, but should live behind a package boundary unless they are needed by the
base reader.

- **Themes:** System-wide appearance packages, including dark themes and typography/display presets.
- **Sleep screens:** Static or lightweight dynamic sleep-screen packages, including experiments like Game of Life.
- **Reader modules:** Optional behaviour that hooks into reading, library, sync, or settings workflows.
- **Standalone apps:** Local apps such as games, tools, or experiments that can be launched explicitly by the user.
- **Integrations:** Optional network or service integrations, gated by permissions and explicit user configuration.
- **Developer tooling:** SDKs, manifests, validators, examples, registry metadata, and hub workflows for packages.
- **Side loading:** Installing packages from SD card and, later, Wi-Fi/hub flows is core to the project.

### Out of Scope for Core Firmware

These are not rejected as ideas; they are rejected as mandatory firmware features. They should be packages when they
fit the hardware.

- Games, toys, and non-reading apps.
- Alternate sleep screens beyond the default built-ins.
- Opinionated themes beyond the default firmware theme.
- Optional online integrations.
- Experimental UI concepts that are useful to some people but too specialised for everyone.

### Out of Scope for Packages

Packages still run on constrained e-paper hardware. The package system should refuse or heavily sandbox ideas that
create unacceptable risk.

- Background Wi-Fi loops that drain the battery or wake the device unexpectedly.
- Unbounded CPU, RAM, or storage use.
- Unclear permission requests or silent network access.
- Packages that can corrupt books, reading progress, firmware state, or other packages.
- Features requiring hardware that the target board does not have.

### Technically Unsupported for Now

These ideas can be revisited, but should not block the first Marginalia package layer.

- **Native binary side loading:** The manifest and store should be designed for it, but the first implementation may
  list packages before executing third-party code.
- **PDF rendering:** Fixed-layout PDFs are a poor fit for small e-paper screens without heavy pan/zoom workflows.
- **Always-online apps:** The first package model should be local-first and wake-friendly.

## 3. Idea Evaluation

Marginalia should ask a different question than upstream CrossPoint:

> Does this belong in the base reader, or does it belong in a package?

The base reader should stay boring, fast, and dependable. The package ecosystem is where narrower, stranger, or more
personal ideas can live. If an idea needs hooks the package system does not expose yet, propose the hook and the package
together so maintainers can evaluate the real use case.

When in doubt, open a Discussion before a large implementation.
