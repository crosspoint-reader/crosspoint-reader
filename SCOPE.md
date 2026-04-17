# Project Vision & Scope: CrossPoint Reader

The goal of CrossPoint Reader is to create an efficient, open-source reading experience for the Xteink X4. We believe a
dedicated e-reader should do one thing exceptionally well: **facilitate focused reading.**

## 1. Core Mission

To provide a lightweight, high-performance firmware that maximizes the potential of the X4, prioritizing legibility and
usability over "swiss-army-knife" functionality.

## 2. Scope

### In-Scope

*These are features that directly improve the primary purpose of the device.*

* **User Experience:** E.g. User-friendly interfaces, and interactions, both inside the reader and navigating the
  firmware. This includes things like button mapping, book loading, and book navigation like bookmarks.
* **Document Rendering:** E.g. Support for rendering documents (primarily EPUB) and improvements to the rendering
  engine.
* **Format Optimization:** E.g. Efficiently parsing EPUB (CSS/Images) and other documents within the device's
  capabilities.
* **Typography & Legibility:** E.g. Custom font support, hyphenation engines, and adjustable line spacing.
* **E-Ink Driver Refinement:** E.g. Reducing full-screen flashes (ghosting management) and improving general rendering.
* **Library Management:** E.g. Simple, intuitive ways to organize and navigate a collection of books.
* **Local Transfer:** E.g. Simple, "pull" based book loading via a basic web-server or public and widely-used standards.
* **Language Support:** E.g. Support for multiple languages both in the reader and in the interfaces.
* **Reference Tools:** E.g. Local dictionary lookup. Providing quick, offline definitions to enhance comprehension 
  without breaking focus.
* **Clock Display (device dependent):** 

| Device | Scope |
| -- | -- |
| X3 | The X3 uses a dedicated DS3231 RTC, which maintains accurate time across sleep cycles and can be treated as a reliable wall clock. |
| X4 | The X4 relies on the ESP32-C3's internal RTC, which drifts significantly during deep sleep. NTP sync could correct this, with an appropriate user experience around connecting to the internet on wake or on demand. This causes some tension with the **Active Connectivity** section below, so please open a discussion about this UX if it's a feature you would find useful. |

### Out-of-Scope

*These items are rejected because they compromise the device's stability or mission.*

* **Interactive Apps:** No Notepads, Calculators, or Games. This is a reader, not a PDA.
* **Active Connectivity:** No RSS readers, News aggregators, or Web browsers. Background Wi-Fi tasks drain the battery
  and complicate the single-core CPU's execution.
* **Media Playback:** No Audio players or Audio-books.
* **Complex Annotation:** No typed out notes. These features are better suited for devices with better input
  capabilities and more powerful chips.

### In-scope — Technically Unsupported

*These features align with CrossPoint's goals but are impractical on the current hardware or produce poor UX.*

* **PDF Rendering:** PDFs are fixed-layout documents, so rendering them requires displaying pages as images rather than reflowable text — resulting in constant panning and zooming that makes for a poor reading experience on e-ink.

## 3. Idea Evaluation

While I appreciate the desire to add new and exciting features to CrossPoint Reader, CrossPoint Reader is designed to be
a lightweight, reliable, and performant e-reader. Things which distract or compromise the device's core mission will not
be accepted. As a guiding question, consider if your idea improve the "core reading experience" for the average user,
and, critically, not distract from that reading experience.

> **Note to Contributors:** If you are unsure if your idea fits the scope, please open a **Discussion** before you start
> coding!
