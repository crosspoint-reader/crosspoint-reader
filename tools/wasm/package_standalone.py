#!/usr/bin/env python3
"""Package compiled previews as self-contained HTML files that open from disk."""

import argparse
import base64
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
WEB = Path(__file__).resolve().parent / "web"
DEVICES = ("x4", "x3", "x4pro")
STATIC_IMPORT = re.compile(r"^(import\s+[^;\n]+?\s+from\s+)(['\"])(\./[\w.-]+\.js)\2;", re.MULTILINE)


def encoded(data):
    return base64.b64encode(data).decode("ascii")


def data_url(data, mime):
    return f"data:{mime};base64,{encoded(data)}"


def inline_modules(name, cache):
    """Keep native ES modules, substituting data URLs for local static imports."""
    if name not in cache:
        source = (WEB / name).read_text()

        def replace(match):
            dependency = match[3][2:]
            return match[1] + json.dumps(inline_modules(dependency, cache)) + ";"

        source = STATIC_IMPORT.sub(replace, source)
        source += f"\n//# sourceURL=crosspoint-preview/{name}\n"
        cache[name] = data_url(source.encode(), "text/javascript")
    return cache[name]


def replace_once(source, old, new):
    if source.count(old) != 1:
        raise ValueError(f"Standalone template changed; expected one occurrence of {old!r}")
    return source.replace(old, new)


def inline_page(name, cache):
    html = (WEB / f"{name}.html").read_text()
    css_name = "preview" if name == "index" else name
    html = replace_once(html, f'<link rel="stylesheet" href="{css_name}.css">',
                        "<style>" + (WEB / f"{css_name}.css").read_text() + "</style>")
    script_name = "preview" if name == "index" else name
    html = replace_once(html, f'<script type="module" src="{script_name}.js"></script>',
                        f'<script type="module" src="{inline_modules(script_name + ".js", cache)}"></script>')
    return html


def package(output, device):
    target = output / device
    info = json.loads((target / "build.json").read_text())
    if info["device"] != device:
        raise ValueError("Build manifest does not match the device")
    modules = {}
    frame = inline_page("frame", modules)
    frame = replace_once(frame, "<head>", '<head><script>globalThis.crosspointBundle = parent.crosspointBundle;</script>')
    bundle = {
        "device": device,
        "info": info,
        "frame": frame,
        "firmware": data_url((target / "firmware.js").read_bytes() +
                             f"\n//# sourceURL=crosspoint-preview/{device}/firmware.js\n".encode(), "text/javascript"),
        "wasm": encoded((target / "firmware.wasm").read_bytes()),
        "data": encoded((target / "firmware.data").read_bytes()),
    }
    # JSON is embedded in a script element, so even literal </script> must not end it.
    payload = json.dumps(bundle, separators=(",", ":"), ensure_ascii=True).replace("<", "\\u003c")
    bootstrap = ("<script>globalThis.crosspointBundle = " + payload + ";\n"
                 "crosspointBundle.origin = location.protocol === 'file:' ? 'null' : location.origin;</script>")
    html = inline_page("index", modules)
    html = replace_once(html, '<script type="module"', bootstrap + '<script type="module"')
    html = replace_once(html, 'href="./"', 'href="#"')
    for filename in ("LICENSE.txt", "LICENSE.crossplay.txt"):
        url = data_url((output / filename).read_bytes(), "text/plain;charset=utf-8")
        html = replace_once(html, f'href="{filename}"', f'href="{url}" download="{filename}"')
    destination = output / f"crosspoint-{device}.html"
    destination.write_text(html, encoding="utf-8")
    print(f"Standalone preview: {destination} ({destination.stat().st_size / 1024 / 1024:.1f} MiB)")
    return destination


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", choices=(*DEVICES, "all"), default="all")
    parser.add_argument("--output", type=Path, default=ROOT / "build/preview")
    args = parser.parse_args()
    for device in DEVICES if args.device == "all" else (args.device,):
        package(args.output, device)


if __name__ == "__main__":
    main()
