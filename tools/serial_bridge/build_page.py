#!/usr/bin/env python3
"""Builds tools/serial_bridge/crosspoint-serial.html: the real CrossPoint file
manager page, with CSS/icons inlined and the network layer replaced by shim.js
(Web Serial). Open the result in Chrome or Edge. Run after changing the web UI."""
import gzip, json, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
HTML_DIR = os.path.join(HERE, "..", "..", "src", "network", "html")


def asset(symbol):
    for root, _, files in os.walk(HTML_DIR):
        for f in files:
            if not f.endswith(".generated.h"):
                continue
            text = open(os.path.join(root, f), encoding="utf-8").read()
            if f"char {symbol}[]" not in text:
                continue
            body = text.split(f"char {symbol}[]", 1)[1].split("};", 1)[0]
            raw = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", body))
            return (gzip.decompress(raw) if raw[:2] == b"\x1f\x8b" else raw).decode("utf-8")
    raise SystemExit("asset not found: " + symbol)


def main():
    import base64
    css = asset("webCss")
    icons = asset("iconsSvg")
    logo = "data:image/svg+xml;base64," + base64.b64encode(asset("crosspoint_iconSvg").encode()).decode()
    embedded = {
        "/js/jszip.min.js": asset("jszip_minJs"),
        "/js/epub-optimizer.js": asset("epub_optimizerJs"),
    }
    shim = open(os.path.join(HERE, "shim.js"), encoding="utf-8").read()

    def prepare(symbol):
        page = asset(symbol)
        page = re.sub(r'<link[^>]*href="/assets/web\.css"[^>]*>', lambda m: "<style>" + css + "</style>", page)
        page = re.sub(r'<link[^>]*rel="icon"[^>]*>', "", page)
        page = page.replace("/assets/icons.svg#", "#")
        page = page.replace("/assets/crosspoint-icon.svg", logo)
        page = page.replace('<script src="/js/plugin-host.js"></script>', "")
        # No real URL inside the page host: navigation and reload go through the shim.
        page = page.replace("window.location.search", "window.__cpxSearch")
        page = page.replace("window.location.reload()", "window.__cpxReload()")
        page = re.sub(r"(<body[^>]*>)", lambda m: m.group(1) + '<div style="display:none">' + icons + "</div>",
                      page, count=1)
        return page

    pages = {"/": prepare("HomePageHtml"), "/files": prepare("FilesPageHtml"),
             "/settings": prepare("SettingsPageHtml"), "/fonts": prepare("FontsPageHtml")}
    bs = "<" + chr(92) + "/"
    def js(obj):
        return json.dumps(obj).replace("</", bs)
    shell = ('<!DOCTYPE html><html><head><meta charset="utf-8">'
             '<meta name="viewport" content="width=device-width,initial-scale=1">'
             '<title>CrossPoint serial file manager</title></head><body style="margin:0">'
             "<script>window.__cpxScripts=" + js(embedded) + ";window.__cpxPages=" + js(pages) + ";</script>"
             "<script>" + shim.replace("</script", bs + "script") + "</script></body></html>")
    out = os.path.join(HERE, "crosspoint-serial.html")
    open(out, "w", encoding="utf-8").write(shell)
    print("wrote", out, len(shell), "bytes")


if __name__ == "__main__":
    main()
