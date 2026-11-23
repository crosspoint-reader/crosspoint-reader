#pragma once
#include <string_view>
namespace FsHelpers {
inline bool hasPngExtension(std::string_view name) { return name.ends_with(".png"); }
inline bool hasJpgExtension(std::string_view name) { return name.ends_with(".jpg") || name.ends_with(".jpeg"); }
inline bool hasGifExtension(std::string_view name) { return name.ends_with(".gif"); }
}  // namespace FsHelpers
