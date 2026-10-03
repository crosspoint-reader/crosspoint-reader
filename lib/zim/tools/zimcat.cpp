// Pocket Library for CrossPoint
// Copyright (C) 2026 Pocket Library contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. See LICENSE-GPL-3.0 at the repository root.

// zimcat: print an article from a Kiwix ZIM file (or a split .zimaa/.zimab
// set) using the same reader the device runs.
//
//   zimcat FILE --info
//   zimcat FILE "Forbidden City"          article HTML by exact title
//   zimcat FILE --path C/Forbidden_City   by path (namespace/path)
//   zimcat FILE --search "Forb" [N]       first N titles at or after a prefix
//   zimcat FILE --main                    the main page
//
// Redirects are followed; a note goes to stderr. Exit status: 0 found,
// 1 not found, 2 the file could not be read.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "PosixSource.h"
#include "ZimArchive.h"

namespace {

using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

void usage() {
  std::fprintf(stderr,
               "usage: zimcat FILE --info\n"
               "       zimcat FILE TITLE\n"
               "       zimcat FILE --path NS/PATH\n"
               "       zimcat FILE --search PREFIX [COUNT]\n"
               "       zimcat FILE --main\n"
               "FILE may be name.zim, name.zimaa (split parts are joined) or the bare name.\n");
}

int fail(zim::Error err, const char* what) {
  std::fprintf(stderr, "zimcat: %s: %s\n", what, zim::errorName(err));
  return err == zim::Error::NotFound ? 1 : 2;
}

int printEntry(zim::Archive& archive, zim::Entry entry) {
  if (entry.isRedirect()) {
    const std::string from = entry.title;
    const zim::Error err = archive.resolve(entry);
    if (err != zim::Error::None) return fail(err, "redirect");
    std::fprintf(stderr, "(Redirected from %s to %s)\n", from.c_str(), entry.title.c_str());
  }
  std::string body;
  const auto t = Clock::now();
  const zim::Error err = archive.read(entry, body);
  if (err != zim::Error::None) return fail(err, "read");
  std::fprintf(stderr, "%c/%s  [%s, %zu bytes, cluster %u, %.1f ms]\n", entry.ns, entry.path.c_str(),
               archive.mimeType(entry.mime).c_str(), body.size(), entry.cluster, msSince(t));
  std::fwrite(body.data(), 1, body.size(), stdout);
  if (!body.empty() && body.back() != '\n') std::fputc('\n', stdout);
  return 0;
}

int info(zim::Archive& archive, size_t parts) {
  const auto& h = archive.header();
  std::printf("version        %u.%u (%s namespaces)\n", h.majorVersion, h.minorVersion,
              archive.usesNewNamespaces() ? "new C/M/W/X" : "old A/I/-/M");
  std::printf("parts          %zu\n", parts);
  std::printf("entries        %u\n", h.entryCount);
  std::printf("clusters       %u\n", h.clusterCount);
  const char* source = "";
  switch (archive.titleSource()) {
    case zim::TitleSource::Listing:
      source = " (X/listing/titleOrdered/v0)";
      break;
    case zim::TitleSource::Header:
      source = " (header title list)";
      break;
    case zim::TitleSource::Articles:
      source = " (front articles only, v1)";
      break;
    case zim::TitleSource::None:
      break;
  }
  std::printf("title index    %u%s\n", archive.titleCount(), source);
  if (archive.hasArticleList()) std::printf("articles (v1)  %u\n", archive.articleListCount());
  std::printf("mime types     %zu\n", archive.mimeTypeCount());
  for (size_t i = 0; i < archive.mimeTypeCount(); ++i) {
    std::printf("  [%zu] %s\n", i, archive.mimeType(static_cast<uint16_t>(i)).c_str());
  }
  for (const char* key : {"Title", "Name", "Language", "Date", "Creator", "Publisher", "Flavour", "Description"}) {
    std::string value;
    if (archive.metadata(key, value) == zim::Error::None) std::printf("%-14s %s\n", key, value.c_str());
  }
  zim::Entry main;
  if (archive.mainEntry(main) == zim::Error::None) std::printf("main page      %c/%s\n", main.ns, main.path.c_str());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    usage();
    return 2;
  }
  size_t parts = 0;
  auto source = zim::openArchiveSource(argv[1], &parts);
  if (!source) {
    std::fprintf(stderr, "zimcat: cannot open %s (no .zim or .zimaa found)\n", argv[1]);
    return 2;
  }
  zim::Archive archive;
  const auto t0 = Clock::now();
  zim::Error err = archive.open(std::move(source));
  if (err != zim::Error::None) return fail(err, "open");
  std::fprintf(stderr, "[opened in %.1f ms]\n", msSince(t0));

  const std::string cmd = argv[2];
  if (cmd == "--info") return info(archive, parts);

  zim::Entry entry;
  if (cmd == "--main") {
    err = archive.mainEntry(entry);
    if (err != zim::Error::None) return fail(err, "main page");
    return printEntry(archive, entry);
  }
  if (cmd == "--path") {
    if (argc < 4 || std::strlen(argv[3]) < 3 || argv[3][1] != '/') {
      usage();
      return 2;
    }
    err = archive.findByPath(argv[3][0], argv[3] + 2, entry);
    if (err != zim::Error::None) return fail(err, argv[3]);
    return printEntry(archive, entry);
  }
  if (cmd == "--search") {
    if (argc < 4) {
      usage();
      return 2;
    }
    const int count = argc > 4 ? std::atoi(argv[4]) : 10;
    const char ns = archive.contentNamespace();
    uint32_t pos = 0;
    const auto t = Clock::now();
    err = archive.lowerBoundTitle(ns, argv[3], pos);
    if (err != zim::Error::None) return fail(err, "search");
    std::fprintf(stderr, "[lower bound in %.1f ms]\n", msSince(t));
    for (int i = 0; i < count && pos < archive.titleCount(); ++i, ++pos) {
      if (archive.titleEntryAt(pos, entry) != zim::Error::None || entry.ns != ns) break;
      if (entry.isRedirect()) {
        zim::Entry target = entry;
        if (archive.resolve(target) == zim::Error::None) {
          std::printf("%s  -> %s\n", entry.title.c_str(), target.title.c_str());
          continue;
        }
      }
      std::printf("%s\n", entry.title.c_str());
    }
    return 0;
  }

  // Default: exact title in the content namespace. Wikipedia paths are titles
  // with spaces as underscores, so try that when the title index misses.
  const auto t = Clock::now();
  err = archive.findByTitle(archive.contentNamespace(), cmd, entry);
  if (err == zim::Error::NotFound) {
    std::string path = cmd;
    for (char& c : path) {
      if (c == ' ') c = '_';
    }
    err = archive.findByPath(archive.contentNamespace(), path, entry);
    if (err == zim::Error::None) std::fprintf(stderr, "[found by path, not title]\n");
  }
  if (err != zim::Error::None) return fail(err, cmd.c_str());
  std::fprintf(stderr, "[title lookup in %.1f ms]\n", msSince(t));
  return printEntry(archive, entry);
}
