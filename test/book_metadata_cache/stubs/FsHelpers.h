#pragma once
#include <string>
namespace FsHelpers {
inline bool hasTxtExtension(const std::string& p) { return p.ends_with(".txt"); }
inline bool hasMarkdownExtension(const std::string& p) { return p.ends_with(".md"); }
inline std::string normalisePath(const std::string& p) { return p; }
}  // namespace FsHelpers
