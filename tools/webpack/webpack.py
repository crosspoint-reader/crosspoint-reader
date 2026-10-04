#!/usr/bin/env python3
# Pocket Library for CrossPoint
# Copyright (C) 2026 Pocket Library contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Makes a small offline collection (a ZIM file) from pages of a website, for
your own card: the reader opens it like the Kiwix collections.

    python3 webpack.py recipes/pets.toml "/Volumes/SSK Drive"

A recipe names the section pages to start from and which links to follow.
Each page is fetched once (politely: one a second), reduced to its article
(headings, paragraphs, lists, tables; menus, scripts and pictures dropped),
and stored under its address without "https://www." (so
msdvetmanual.com/special-pet-topics/emergencies/wound-management), with a
contents page listing them all. Plain Python: nothing to install.

Pages copied this way are for your own use; copyright stays with the site.
"""
from __future__ import annotations

import html
import posixpath
import re
import sys
import time
import tomllib
import urllib.error
import urllib.parse
import urllib.request
from datetime import date
from html.parser import HTMLParser
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from zimwrite import ZimWriter  # noqa: E402

USER_AGENT = "PocketLibrary-webpack/1 (personal offline copy; one request a second)"

# Elements dropped with everything inside them.
DROP = {"script", "style", "noscript", "svg", "header", "nav", "footer", "aside", "form", "button", "iframe",
        "template", "select", "input", "img", "picture", "video", "audio", "canvas", "figure", "object"}
# Elements kept (without attributes, except a link's href).
KEEP = {"h1", "h2", "h3", "h4", "h5", "h6", "p", "ul", "ol", "li", "b", "strong", "i", "em", "a", "table", "thead",
        "tbody", "tr", "td", "th", "blockquote", "dl", "dt", "dd", "sup", "sub", "caption"}
# Elements with no closing tag: never opened on the stack.
VOID = {"br", "img", "input", "hr", "meta", "link", "source", "wbr", "area", "base", "col", "embed", "track"}
# Class or id words that mark page furniture rather than the article.
FURNITURE = re.compile(r"(^|[-_ ])(nav|menu|breadcrumbs?|footer|header|sidebar|share|social|cookie|banner|"
                       r"advert|ads?|promo|related|newsletter|skip|search|modal|popup|toolbar|print)([-_ ]|$)", re.I)


class Extract(HTMLParser):
    """The article of a page: <main> (else <article>, else <body>) without
    furniture, as simple HTML; the <h1> as its title; the links it holds."""

    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.depth_drop = 0
        self.stack: list[tuple[str, bool, bool]] = []  # tag, dropped here, opened a root
        self.root_depth = 0
        self.has_main = False
        self.out: list[str] = []
        self.title = ""
        self.in_h1 = False
        self.links: list[str] = []
        self.doc_title = ""
        self.in_title = False

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if tag == "title":
            self.in_title = True
        drop = tag in DROP or bool(FURNITURE.search(f"{a.get('class') or ''} {a.get('id') or ''} {a.get('role') or ''}"))
        if tag in ("main",) or a.get("role") == "main":
            self.has_main = True
            drop = False
        opened_root = tag in ("main", "article", "body") or a.get("role") == "main"
        if tag in VOID:
            if tag == "br" and not self.depth_drop and self.root_depth:
                self.out.append("<br/>")
            return
        self.stack.append((tag, drop, opened_root))
        if drop:
            self.depth_drop += 1
            return
        if opened_root:
            self.root_depth += 1
        if self.depth_drop or not self.root_depth:
            return
        if tag == "h1":
            self.in_h1 = True
        if tag in KEEP:
            if tag == "a":
                href = a.get("href") or ""
                if href:
                    self.links.append(href)
                self.out.append(f'<a href="{html.escape(href, quote=True)}">')
            else:
                self.out.append(f"<{tag}>")

    def handle_endtag(self, tag):
        if tag == "title":
            self.in_title = False
        if tag in VOID:
            return
        # Close up to the matching open tag (forgiving of sloppy HTML).
        for k in range(len(self.stack) - 1, -1, -1):
            if self.stack[k][0] == tag:
                while len(self.stack) > k:
                    t, dropped, opened_root = self.stack.pop()
                    if dropped:
                        self.depth_drop -= 1
                        continue
                    if not self.depth_drop and self.root_depth and t in KEEP and t not in VOID:
                        self.out.append(f"</{t}>")
                    if t == "h1":
                        self.in_h1 = False
                    if opened_root:
                        self.root_depth -= 1
                return

    def handle_data(self, data):
        if self.in_title:
            self.doc_title += data
        if self.depth_drop or not self.root_depth:
            return
        if self.in_h1:
            self.title += data
        self.out.append(html.escape(data, quote=False))


def fetch(url: str, retries: int = 3) -> str:
    for attempt in range(retries + 1):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
            with urllib.request.urlopen(req, timeout=30) as r:
                charset = r.headers.get_content_charset() or "utf-8"
                return r.read().decode(charset, "replace")
        except (urllib.error.URLError, OSError) as e:
            if attempt == retries:
                raise
            time.sleep(2 ** attempt)
    return ""


def clean_url(base: str, href: str) -> str:
    u = urllib.parse.urljoin(base, href)
    p = urllib.parse.urlparse(u)
    return urllib.parse.urlunparse((p.scheme, p.netloc.lower(), p.path.rstrip("/") or "/", "", "", ""))


def zim_path(url: str) -> str:
    p = urllib.parse.urlparse(url)
    host = p.netloc.lower()
    if host.startswith("www."):
        host = host[4:]
    return host + p.path


def extract(page_html: str) -> tuple[str, str, list[str]]:
    # A page whose <main> holds the article: take only that.
    ex = Extract()
    ex.feed(page_html)
    body = "".join(ex.out)
    title = re.sub(r"\s+", " ", ex.title).strip() or re.sub(r"\s+", " ", ex.doc_title).split(" - ")[0].strip()
    return title, body, ex.links


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    recipe = tomllib.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
    out_dir = Path(sys.argv[2])
    pack = recipe["pack"]
    follow = re.compile(pack["follow"])
    title_suffix = re.compile(pack.get("strip_title", r"$^"))
    max_pages = int(pack.get("max_pages", 300))

    # Start pages, then the links on them that the recipe follows (one level),
    # plus any pages named outright.
    queue: list[str] = []
    seen: set[str] = set()
    pages: dict[str, tuple[str, str]] = {}
    order: list[str] = []

    def want(u: str) -> None:
        if u not in seen and len(seen) < max_pages:
            seen.add(u)
            queue.append(u)

    for u in pack.get("start", []) + pack.get("pages", []):
        want(clean_url(u, u))
    depth = {u: 0 for u in queue}
    while queue:
        url = queue.pop(0)
        try:
            text = fetch(url)
        except Exception as e:  # noqa: BLE001 (report and go on)
            print(f"  skipped {url}: {e}")
            continue
        title, body, links = extract(text)
        title = title_suffix.sub("", title).strip() or zim_path(url)
        words = len(re.sub(r"<[^>]+>", " ", body).split())
        print(f"  {words:6d} words  {title}")
        pages[url] = (title, body)
        order.append(url)
        if depth[url] < int(pack.get("depth", 1)):
            for href in links:
                u = clean_url(url, href)
                if follow.search(u) and u not in seen:
                    depth[u] = depth[url] + 1
                    want(u)
        time.sleep(float(pack.get("delay", 1.0)))

    if not pages:
        print("Nothing fetched: check the addresses in the recipe.")
        return 1

    # Links between packed pages become links inside the file; others plain text.
    paths = {u: zim_path(u) for u in pages}
    href_re = re.compile(r'<a href="([^"]*)">')
    w = ZimWriter()
    for url, (title, body) in pages.items():
        here = paths[url]

        def relink(m: re.Match) -> str:
            target = clean_url(url, html.unescape(m.group(1)))
            if target in paths:
                # Relative to this page's folder; a page and a folder can share a
                # name (".../emergencies" and ".../emergencies/wound-management").
                t = paths[target]
                rel = posixpath.join(posixpath.relpath(posixpath.dirname(t), posixpath.dirname(here)),
                                     posixpath.basename(t))
                return f'<a href="{html.escape(rel, quote=True)}">'
            return "<a>"

        body = href_re.sub(relink, body)
        doc = (f'<!DOCTYPE html><html><head><meta charset="utf-8"><title>{html.escape(title)}</title></head>'
               f"<body>{body}<p><i>From {html.escape(pack.get('source', url))}: {html.escape(url)} "
               f"(copied {date.today().isoformat()}, for personal use).</i></p></body></html>")
        w.add(here, title, "text/html", doc.encode("utf-8"))

    # A contents page, in the order the pages were found.
    items = "".join(f'<li><a href="{html.escape(paths[u], quote=True)}">'
                    f"{html.escape(pages[u][0])}</a></li>" for u in order)
    index = (f'<!DOCTYPE html><html><head><meta charset="utf-8"><title>{html.escape(pack["title"])}</title></head>'
             f"<body><h1>{html.escape(pack['title'])}</h1><p>{html.escape(pack.get('description', ''))}</p>"
             f"<ul>{items}</ul></body></html>")
    w.add("index", pack["title"], "text/html", index.encode("utf-8"))
    w.main_path = "index"
    w.metadata("Title", pack["title"])
    w.metadata("Description", pack.get("description", ""))
    w.metadata("Language", pack.get("language", "eng"))
    w.metadata("Creator", pack.get("source", ""))
    w.metadata("Publisher", "Pocket Library webpack (personal copy)")
    w.metadata("Date", date.today().isoformat())
    w.metadata("Name", pack["name"])
    out = out_dir / f"{pack['name']}_{date.today().strftime('%Y-%m')}.zim"
    w.write(str(out))
    print(f"Wrote {out} ({len(pages)} pages, {out.stat().st_size // 1024} KB).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
