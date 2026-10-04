// Pocket Library for CrossPoint
// Copyright (C) 2026 Pocket Library contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. See LICENSE-GPL-3.0 at the repository root.

// zimindex: builds the title index the device searches (see ZimTitleIndex.h).
//
//   zimindex FILE.zim                  writes FILE.pltitles next to it
//   zimindex FILE.zim OUT.pltitles     writes OUT.pltitles
//   zimindex FILE.zim --search PREFIX [COUNT]
//                                      looks PREFIX up in FILE.pltitles
//
// FILE may also be a split set (name.zimaa) or the bare name.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "PosixSource.h"
#include "TitleIndexWriter.h"
#include "ZimArchive.h"
#include "ZimFold.h"
#include "ZimTitleIndex.h"

namespace {

using Clock = std::chrono::steady_clock;

double secondsSince(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }
double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

void usage() {
  std::fprintf(stderr,
               "usage: zimindex FILE [OUT.pltitles]\n"
               "       zimindex FILE --search PREFIX [COUNT]\n"
               "FILE may be name.zim, name.zimaa (split parts are joined) or the bare name.\n");
}

// name.zim, name.zimaa or name  ->  name.pltitles
std::string defaultIndexPath(std::string path) {
  for (const char* ext : {".zimaa", ".zim"}) {
    const std::string e = ext;
    if (path.size() > e.size() && path.compare(path.size() - e.size(), e.size(), e) == 0) {
      path.resize(path.size() - e.size());
      break;
    }
  }
  return path + ".pltitles";
}

void progress(uint32_t done, uint32_t total) {
  std::fprintf(stderr, "\r  reading titles: %u / %u entries", done, total);
  if (done == total) std::fputc('\n', stderr);
}

int search(zim::Archive& archive, const std::string& indexPath, const char* prefix, int count) {
  zim::TitleIndex index;
  zim::Error err = index.open(zim::PosixSource::open(indexPath));
  if (err != zim::Error::None) {
    std::fprintf(stderr, "zimindex: %s: %s\n", indexPath.c_str(), zim::errorName(err));
    return 2;
  }
  if (!index.matches(archive)) {
    std::fprintf(stderr, "zimindex: %s was built for a different ZIM\n", indexPath.c_str());
    return 2;
  }
  const std::string key = zim::foldKey(prefix);
  auto cursor = std::make_unique<zim::TitleIndex::Cursor>();
  const auto t = Clock::now();
  err = index.seek(key, *cursor);
  if (err != zim::Error::None) {
    std::fprintf(stderr, "zimindex: search: %s\n", zim::errorName(err));
    return 2;
  }
  std::fprintf(stderr, "[seek in %.1f ms; key \"%s\"]\n", msSince(t), key.c_str());
  zim::TitleRecord rec;
  zim::Entry e;
  for (int i = 0; i < count && index.next(*cursor, rec) == zim::Error::None; ++i) {
    if (!zim::keyHasPrefix(rec.key, key)) break;
    if (archive.entryAt(rec.entry, e) != zim::Error::None) break;
    if (e.isRedirect()) {
      zim::Entry target = e;
      if (archive.resolve(target) == zim::Error::None) {
        std::printf("%s  -> %s\n", e.title.c_str(), target.title.c_str());
        continue;
      }
    }
    std::printf("%s\n", e.title.c_str());
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    usage();
    return 2;
  }
  auto source = zim::openArchiveSource(argv[1]);
  if (!source) {
    std::fprintf(stderr, "zimindex: cannot open %s (no .zim or .zimaa found)\n", argv[1]);
    return 2;
  }
  zim::Archive archive;
  zim::Error err = archive.open(std::move(source));
  if (err != zim::Error::None) {
    std::fprintf(stderr, "zimindex: open: %s\n", zim::errorName(err));
    return 2;
  }
  if (argc >= 4 && std::string(argv[2]) == "--search") {
    return search(archive, defaultIndexPath(argv[1]), argv[3], argc > 4 ? std::atoi(argv[4]) : 10);
  }
  if (argc > 3 || (argc == 3 && argv[2][0] == '-')) {
    usage();
    return 2;
  }
  const std::string out = argc == 3 ? argv[2] : defaultIndexPath(argv[1]);

  const auto t0 = Clock::now();
  zim::TitleIndexWriter writer;
  err = zim::collectTitles(archive, writer, &progress);
  if (err != zim::Error::None) {
    std::fprintf(stderr, "\nzimindex: reading titles: %s\n", zim::errorName(err));
    return 2;
  }
  std::fprintf(stderr, "  %zu titles in %.1f s; sorting and writing...\n", writer.size(), secondsSince(t0));
  std::string why;
  // Popular tree: the best-known titles, so short prefixes find them first.
  constexpr size_t kPopularMax = 400000;
  if (!writer.write(out, archive.header().uuid, archive.entryCount(), &why, kPopularMax)) {
    std::fprintf(stderr, "zimindex: %s\n", why.c_str());
    return 2;
  }
  std::fprintf(stderr, "wrote %s (%zu popular) in %.1f s total\n", out.c_str(), writer.popularRecords(),
               secondsSince(t0));
  return 0;
}
