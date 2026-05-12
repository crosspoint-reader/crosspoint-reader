# Supported Markdown Syntax

This document describes the subset of Markdown that the built-in reader renders correctly.
It is implemented in MdReaderActivity.cpp

---/

## Block Elements

### Headings

ATX-style headings only (leading `#` characters). Levels 1–3 are distinct; levels 4–6 are rendered as H3.

```
# Heading 1
## Heading 2
### Heading 3
```

Setext-style headings (`===` / `---` underlines) are **not** supported.

### Horizontal Rule

Three or more `-`, `*`, or `_` characters on a line by themselves (spaces between them are allowed).

```
---
***
___
```

### Blockquote

Lines beginning with `>` (optionally followed by a space).

```
> This is a quoted passage.
```

Nested blockquotes are **not** supported.

### Unordered List

Lines beginning with `-`, `*`, or `+` followed by a space. A bullet square is drawn to the left of each item.

```
- First item
* Second item
+ Third item
```

Ordered lists and nested lists are **not** supported.

### Paragraph

Any line that does not match the above block prefixes is treated as a plain paragraph. Long lines are word-wrapped to fit the viewport. Blank lines produce a blank display line between paragraphs.

---

## Inline Elements

Inline markup is parsed inside all block types.

### Bold

```
**bold text**
__bold text__
```

### Italic

```
*italic text*
_italic text_
```

### Bold-Italic

```
***bold and italic***
___bold and italic___
```

### Inline Code

Rendered in a monospace font. No further inline parsing is performed inside backticks.

```
`inline code`
```

### Links

Only the link label is rendered; the URL is silently discarded.

```
[link label](https://example.com)
```

---

## Not Supported

The following common Markdown features are not rendered:

- Setext headings
- Ordered lists (`1.`, `2.`, …)
- Nested or indented lists
- Fenced or indented code blocks
- Images (`![alt](url)`)
- Tables
- Strikethrough (`~~text~~`)
- Task list checkboxes (`- [ ]`)
- Footnotes
- HTML blocks / inline HTML
- Nested blockquotes
