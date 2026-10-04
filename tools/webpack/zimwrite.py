# Pocket Library for CrossPoint
# Copyright (C) 2026 Pocket Library contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""A small ZIM writer in plain Python (no libzim): enough for a pack of a few
hundred web pages. Written from the openZIM file format description
(https://wiki.openzim.org/wiki/ZIM_file_format): version 6.1 ("C" namespace
for content, "M" for metadata), one uncompressed cluster, the header's title
pointer list, and the MD5 checksum at the end.
"""
from __future__ import annotations

import hashlib
import struct
import uuid as uuidlib
from dataclasses import dataclass, field

MAGIC = 0x044D495A
NO_PAGE = 0xFFFFFFFF


@dataclass
class _Item:
    ns: str
    path: str
    title: str
    mime: str = ""
    data: bytes = b""
    redirect_to: tuple[str, str] | None = None  # (ns, path)


@dataclass
class ZimWriter:
    items: list[_Item] = field(default_factory=list)
    main_path: str | None = None

    def add(self, path: str, title: str, mime: str, data: bytes, ns: str = "C") -> None:
        self.items.append(_Item(ns, path, title, mime, data))

    def add_redirect(self, path: str, title: str, target_path: str, ns: str = "C") -> None:
        self.items.append(_Item(ns, path, title, redirect_to=("C", target_path)))

    def metadata(self, name: str, value: str) -> None:
        self.items.append(_Item("M", name, "", "text/plain", value.encode("utf-8")))

    def write(self, out_path: str) -> None:
        items = sorted(self.items, key=lambda i: (i.ns, i.path.encode("utf-8")))
        keys = [(i.ns, i.path) for i in items]
        if len(set(keys)) != len(keys):
            raise ValueError("two items share a path")
        index = {k: n for n, k in enumerate(keys)}
        mimes = sorted({i.mime for i in items if i.redirect_to is None})
        mime_index = {m: n for n, m in enumerate(mimes)}

        # One cluster, uncompressed: every blob in path order.
        blobs = [i.data for i in items if i.redirect_to is None]
        offsets, pos = [], 4 * (len(blobs) + 1)
        for b in blobs:
            offsets.append(pos)
            pos += len(b)
        offsets.append(pos)
        cluster = bytes([1]) + b"".join(struct.pack("<I", o) for o in offsets) + b"".join(blobs)

        dirents, blob_no = [], 0
        for i in items:
            path_title = i.path.encode("utf-8") + b"\0" + (i.title.encode("utf-8") if i.title != i.path else b"") + b"\0"
            if i.redirect_to is not None:
                target = index.get(i.redirect_to)
                if target is None:
                    raise ValueError(f"redirect {i.path} -> missing {i.redirect_to}")
                dirents.append(struct.pack("<HBcI", 0xFFFF, 0, i.ns.encode(), 0) + struct.pack("<I", target) + path_title)
            else:
                dirents.append(struct.pack("<HBcI", mime_index[i.mime], 0, i.ns.encode(), 0) +
                               struct.pack("<II", 0, blob_no) + path_title)
                blob_no += 1

        mime_list = b"".join(m.encode("ascii") + b"\0" for m in mimes) + b"\0"
        header_size = 80
        mime_pos = header_size
        path_ptr_pos = mime_pos + len(mime_list)
        title_ptr_pos = path_ptr_pos + 8 * len(items)
        dirent_pos = title_ptr_pos + 4 * len(items)
        dirent_offsets, p = [], dirent_pos
        for d in dirents:
            dirent_offsets.append(p)
            p += len(d)
        cluster_ptr_pos = p
        cluster_pos = cluster_ptr_pos + 8
        checksum_pos = cluster_pos + len(cluster)

        def title_key(n: int):
            it = items[n]
            return (it.ns, (it.title or it.path).encode("utf-8"))

        title_order = sorted(range(len(items)), key=title_key)
        main = NO_PAGE
        if self.main_path is not None:
            main = index[("C", self.main_path)]

        header = struct.pack("<IHH16sIIQQQQIIQ", MAGIC, 6, 1, uuidlib.uuid4().bytes, len(items), 1,
                             path_ptr_pos, title_ptr_pos, cluster_ptr_pos, mime_pos, main, NO_PAGE, checksum_pos)
        body = (header + mime_list + b"".join(struct.pack("<Q", o) for o in dirent_offsets) +
                b"".join(struct.pack("<I", n) for n in title_order) + b"".join(dirents) +
                struct.pack("<Q", cluster_pos) + cluster)
        assert len(body) == checksum_pos
        with open(out_path, "wb") as f:
            f.write(body)
            f.write(hashlib.md5(body).digest())
