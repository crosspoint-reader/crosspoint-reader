# Project Vision & Scope: CrossPoint Reader

The goal of CrossPoint Reader is to create an efficient, open-source reading experience for the Xteink X4. We believe a
dedicated e-reader should do one thing exceptionally well: **facilitate focused reading.**

## 1. Core Mission

To provide a lightweight, high-performance firmware that maximizes the potential of the X4, prioritizing legibility and
usability over "swiss-army-knife" functionality.

## 2. Scope

### In-Scope: The "Core Reading Experience"

*These are features that directly improve the primary purpose of the device.*

* **Document Rendering:** E.g. Support for rendering documents (primarily EPUB) and improvements to the rendering 
  engine.
* **Format Optimization:** E.g. Efficiently parsing EPUB (CSS/Images) and other documents within the device's 
  capabilities.
* **Typography & Legibility:** E.g. Custom font support, hyphenation engines, and adjustable line spacing.
* **E-Ink Driver Refinement:** E.g. Reducing full-screen flashes (ghosting management) and improving general rendering.
* **Library Management:** E.g. Simple, intuitive ways to organize and navigate a collection of books.
* **Local Transfer:** E.g. Simple, "pull" based book loading via a basic web-server or public and widely-used standards.

### Out-of-Scope: The "Feature Creep" Guardrail

*These items are rejected because they compromise the device's stability or mission.*

* **Interactive Apps:** No Notepads, Calculators, or Games. This is a reader, not a PDA.
* **Active Connectivity:** No RSS readers, News aggregators, or Web browsers. Background Wi-Fi tasks drain the battery 
  and complicate the single-core CPU's execution.
* **Media Playback:** No Audio players or Audio-books

## 3. Idea Evaluation

While I appreciate the desire to add new and exciting features to CrossPoint Reader, CrossPoint Reader is designed to be 
a lightweight, reliable, and performant e-reader. Things which distract or compromise the device's core mission will not
be accepted. As a guiding question, consider if your idea improve the "core reading experience" for the average user, 
and, critically, not distract from that reading experience.

> **Note to Contributors:** If you are unsure if your idea fits the scope, please open a **Discussion** before you start
> coding!
