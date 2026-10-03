// Pocket Library for CrossPoint
// Copyright (C) 2026 Pocket Library contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. See LICENSE-GPL-3.0 at the repository root.

#include "PosixSource.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace zim {

std::unique_ptr<PosixSource> PosixSource::open(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return nullptr;
  struct stat st{};
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    ::close(fd);
    return nullptr;
  }
  return std::unique_ptr<PosixSource>(new PosixSource(fd, static_cast<uint64_t>(st.st_size)));
}

PosixSource::~PosixSource() { ::close(fd_); }

bool PosixSource::read(uint64_t offset, void* dst, size_t len) {
  if (offset > size_ || len > size_ - offset) return false;
  auto* out = static_cast<char*>(dst);
  while (len > 0) {
    const ssize_t n = ::pread(fd_, out, len, static_cast<off_t>(offset));
    if (n <= 0) return false;
    out += n;
    offset += static_cast<uint64_t>(n);
    len -= static_cast<size_t>(n);
  }
  return true;
}

namespace {
bool isFile(const std::string& path) {
  struct stat st{};
  return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}
bool endsWith(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}
}  // namespace

std::vector<std::string> findParts(const std::string& path) {
  std::string base = path;
  if (endsWith(base, ".zim")) {
    if (isFile(base)) return {base};
    base.resize(base.size() - 4);
  } else if (base.size() > 6 && base.compare(base.size() - 6, 4, ".zim") == 0) {
    base.resize(base.size() - 6);  // name.zimXY -> name
  } else if (isFile(base)) {
    return {base};
  }
  if (isFile(base + ".zim")) return {base + ".zim"};

  std::vector<std::string> parts;
  for (char a = 'a'; a <= 'z'; ++a) {
    for (char b = 'a'; b <= 'z'; ++b) {
      std::string candidate = base + ".zim" + a + b;
      if (!isFile(candidate)) return parts;
      parts.push_back(std::move(candidate));
    }
  }
  return parts;
}

std::unique_ptr<Source> openArchiveSource(const std::string& path, size_t* partCount) {
  const std::vector<std::string> parts = findParts(path);
  if (partCount) *partCount = parts.size();
  if (parts.empty()) return nullptr;
  if (parts.size() == 1) return PosixSource::open(parts[0]);
  auto split = std::make_unique<SplitSource>();
  for (const auto& p : parts) {
    if (!split->addPart(PosixSource::open(p))) return nullptr;
  }
  return split;
}

}  // namespace zim
