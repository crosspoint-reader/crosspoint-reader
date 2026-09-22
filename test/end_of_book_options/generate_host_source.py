"""Compile the production menu and shared reader handler with host UI dependencies.

Only includes are replaced for the menu. ReaderActivity's constructor and shared
menu methods are copied verbatim; unrelated reader/SD operations are not linked.
Both class declarations still come from the production headers.
"""

from pathlib import Path
import sys


def function(source: str, signature: str) -> tuple[str, int]:
    start = source.index(signature)
    index = source.index("{", start)
    depth = 0
    state = "code"
    while index < len(source):
        char = source[index]
        next_char = source[index + 1:index + 2]
        if state in ("string", "char"):
            if char == "\\":
                index += 2
                continue
            if char == ('"' if state == "string" else "'"):
                state = "code"
        elif state == "line":
            if char == "\n":
                state = "code"
        elif state == "block":
            if char == "*" and next_char == "/":
                state = "code"
                index += 2
                continue
        elif char == "/" and next_char in ("/", "*"):
            state = "line" if next_char == "/" else "block"
            index += 2
            continue
        elif char == '"':
            state = "string"
        elif char == "'":
            state = "char"
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1], source.count("\n", 0, start) + 1
        index += 1
    raise ValueError(f"Unclosed function: {signature}")


def main() -> None:
    root, output = map(Path, sys.argv[1:])
    menu_path = root / "src/activities/reader/EndOfBookOptions.cpp"
    reader_path = root / "src/activities/reader/ReaderActivity.cpp"
    menu = menu_path.read_text(encoding="utf-8")
    reader = reader_path.read_text(encoding="utf-8")
    # Preserve source line numbers in compiler diagnostics.
    menu = "\n".join("" if line.startswith("#include") else line for line in menu.splitlines())
    chunks = ['#include "ReaderActivity.h"', '#include <cstring>',
              f'#line 1 "{menu_path.as_posix()}"', menu]
    for signature in ("ReaderActivity::ReaderActivity(",
                      "bool ReaderActivity::endOfBookMenuActive(",
                      "bool ReaderActivity::handleEndOfBookMenu("):
        body, line = function(reader, signature)
        chunks += [f'#line {line} "{reader_path.as_posix()}"', body]
    output.write_text("\n".join(chunks) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
