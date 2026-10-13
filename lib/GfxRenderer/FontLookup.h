#pragma once

#include <map>
#include <optional>

/// A font ID the renderer does not know yet can be supplied on demand. The resolver returns the
/// family for `fontId`, or std::nullopt when the ID is not one it serves. Used for TTF size-ladder
/// rungs, which exist as IDs long before anything draws with them.
template <typename Family>
struct FontResolverHook {
  std::optional<Family> (*resolve)(void* ctx, int fontId) = nullptr;
  void* ctx = nullptr;
};

/// Looks `fontId` up in `fonts`. On a miss, asks `hook` once; a family it supplies is registered
/// and returned. Returns nullptr when neither knows the ID. A hit costs exactly a map find.
template <typename Family>
const Family* findOrResolveFont(std::map<int, Family>& fonts, const int fontId, const FontResolverHook<Family>& hook) {
  const auto it = fonts.find(fontId);
  if (it != fonts.end()) return &it->second;
  if (!hook.resolve) return nullptr;
  std::optional<Family> family = hook.resolve(hook.ctx, fontId);
  if (!family) return nullptr;
  return &fonts.emplace(fontId, *family).first->second;
}
