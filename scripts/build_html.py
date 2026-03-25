import os
import re
import gzip

SRC_DIR = "src"


def strip_js_comments(js: str) -> str:
    """Remove JS comments while preserving string literals and URLs."""
    result = []
    i = 0
    length = len(js)
    while i < length:
        # String literals — pass through unchanged
        if js[i] in ('"', "'", "`"):
            quote = js[i]
            result.append(js[i])
            i += 1
            while i < length:
                if js[i] == "\\" and i + 1 < length:
                    result.append(js[i : i + 2])
                    i += 2
                elif js[i] == quote:
                    result.append(js[i])
                    i += 1
                    break
                else:
                    result.append(js[i])
                    i += 1
        # Block comment /* ... */
        elif js[i] == "/" and i + 1 < length and js[i + 1] == "*":
            end = js.find("*/", i + 2)
            i = end + 2 if end != -1 else length
        # Line comment // ...
        elif js[i] == "/" and i + 1 < length and js[i + 1] == "/":
            end = js.find("\n", i)
            if end == -1:
                i = length
            else:
                # Keep the newline to preserve line structure
                result.append("\n")
                i = end + 1
        # Regex literal — pass through unchanged
        # Heuristic: / after = ( , ; ! & | ? : [ { } ~ ^ or line start
        elif js[i] == "/" and i > 0:
            # Look back for operator context (skip whitespace)
            j = i - 1
            while j >= 0 and js[j] in " \t":
                j -= 1
            if j >= 0 and js[j] in "=(!,;:&|?[{}>~^+-*%":
                result.append(js[i])
                i += 1
                while i < length:
                    if js[i] == "\\" and i + 1 < length:
                        result.append(js[i : i + 2])
                        i += 2
                    elif js[i] == "/":
                        result.append(js[i])
                        i += 1
                        # Regex flags
                        while i < length and js[i].isalpha():
                            result.append(js[i])
                            i += 1
                        break
                    elif js[i] == "[":
                        # Character class — / doesn't end regex inside []
                        result.append(js[i])
                        i += 1
                        while i < length and js[i] != "]":
                            if js[i] == "\\" and i + 1 < length:
                                result.append(js[i : i + 2])
                                i += 2
                            else:
                                result.append(js[i])
                                i += 1
                    else:
                        result.append(js[i])
                        i += 1
            else:
                result.append(js[i])
                i += 1
        else:
            result.append(js[i])
            i += 1
    return "".join(result)


def minify_html(html: str) -> str:
    # Tags where whitespace should be preserved
    preserve_tags = ["pre", "code", "textarea"]
    script_style_tags = ["script", "style"]
    preserve_regex = "|".join(preserve_tags)
    script_style_regex = "|".join(script_style_tags)

    # Protect preserve blocks (pre/code/textarea) with placeholders
    preserve_blocks = []

    def preserve(match):
        preserve_blocks.append(match.group(0))
        return f"__PRESERVE_BLOCK_{len(preserve_blocks) - 1}__"

    html = re.sub(
        rf"<({preserve_regex})[\s\S]*?</\1>", preserve, html, flags=re.IGNORECASE
    )

    # Strip JS/CSS comments inside <script>/<style> blocks, then protect them
    def strip_and_preserve(match):
        tag = match.group(1).lower()
        full = match.group(0)
        # Extract content between opening and closing tags
        open_end = full.index(">") + 1
        close_start = full.rindex("<")
        opening = full[:open_end]
        content = full[open_end:close_start]
        closing = full[close_start:]
        if tag == "script":
            content = strip_js_comments(content)
        elif tag == "style":
            # Remove CSS comments
            content = re.sub(r"/\*.*?\*/", "", content, flags=re.DOTALL)
        preserve_blocks.append(f"{opening}{content}{closing}")
        return f"__PRESERVE_BLOCK_{len(preserve_blocks) - 1}__"

    html = re.sub(
        rf"<({script_style_regex})[\s\S]*?</\1>",
        strip_and_preserve,
        html,
        flags=re.IGNORECASE,
    )

    # Remove HTML comments
    html = re.sub(r"<!--.*?-->", "", html, flags=re.DOTALL)

    # Collapse all whitespace between tags
    html = re.sub(r">\s+<", "><", html)

    # Collapse multiple spaces inside tags
    html = re.sub(r"\s+", " ", html)

    # Restore preserved blocks
    for i, block in enumerate(preserve_blocks):
        html = html.replace(f"__PRESERVE_BLOCK_{i}__", block)

    return html.strip()


def sanitize_identifier(name: str) -> str:
    """Sanitize a filename to create a valid C identifier.

    C identifiers must:
    - Start with a letter or underscore
    - Contain only letters, digits, and underscores
    """
    # Replace non-alphanumeric characters (including hyphens) with underscores
    sanitized = re.sub(r"[^a-zA-Z0-9_]", "_", name)
    # Prefix with underscore if starts with a digit
    if sanitized and sanitized[0].isdigit():
        sanitized = f"_{sanitized}"
    return sanitized


for root, _, files in os.walk(SRC_DIR):
    for file in files:
        if file.endswith(".html") or file.endswith(".js"):
            file_path = os.path.join(root, file)
            with open(file_path, "r", encoding="utf-8") as f:
                content = f.read()

            # Only minify HTML files; JS files are typically pre-minified (e.g., jszip.min.js)
            if file.endswith(".html"):
                processed = minify_html(content)
            else:
                processed = content

            # Compress with gzip (compresslevel 9 is maximum compression)
            # IMPORTANT: we don't use brotli because Firefox doesn't support brotli with insecured context (only supported on HTTPS)
            compressed = gzip.compress(processed.encode("utf-8"), compresslevel=9)

            # Create valid C identifier from filename
            # Use appropriate suffix based on file type
            suffix = "Html" if file.endswith(".html") else "Js"
            base_name = sanitize_identifier(f"{os.path.splitext(file)[0]}{suffix}")
            header_path = os.path.join(root, f"{base_name}.generated.h")

            with open(header_path, "w", encoding="utf-8") as h:
                h.write(f"// THIS FILE IS AUTOGENERATED, DO NOT EDIT MANUALLY\n\n")
                h.write(f"#pragma once\n")
                h.write(f"#include <cstddef>\n\n")

                # Write the compressed data as a byte array
                h.write(f"constexpr char {base_name}[] PROGMEM = {{\n")

                # Write bytes in rows of 16
                for i in range(0, len(compressed), 16):
                    chunk = compressed[i : i + 16]
                    hex_values = ", ".join(f"0x{b:02x}" for b in chunk)
                    h.write(f"  {hex_values},\n")

                h.write(f"}};\n\n")
                h.write(
                    f"constexpr size_t {base_name}CompressedSize = {len(compressed)};\n"
                )
                h.write(
                    f"constexpr size_t {base_name}OriginalSize = {len(processed)};\n"
                )

            print(f"Generated: {header_path}")
            print(f"  Original: {len(content)} bytes")
            print(
                f"  Minified: {len(processed)} bytes ({100 * len(processed) / len(content):.1f}%)"
            )
            print(
                f"  Compressed: {len(compressed)} bytes ({100 * len(compressed) / len(content):.1f}%)"
            )
