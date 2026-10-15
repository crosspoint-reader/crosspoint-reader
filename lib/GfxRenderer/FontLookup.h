#pragma once

#include <map>
#include <optional>

/// What a resolver supplies for a font ID: the family, and the scale it is drawn at. A scale other than 1 means the
/// family is another size standing in for the one the ID names (that size could not be made), so text measured and
/// drawn under the ID keeps its size, as with a font registered scaled up front (GfxRenderer::insertScaledFont).
template <typename Family>
struct ResolvedFont {
  Family family;
  float baseScale = 1.0f;
};

/// A font ID the renderer does not know yet can be supplied on demand. The resolver returns the
/// family for `fontId`, or std::nullopt when the ID is not one it serves. Used for a TTF family's
/// native sizes, which exist as IDs long before anything draws with them.
template <typename Family>
struct FontResolverHook {
  std::optional<ResolvedFont<Family>> (*resolve)(void* ctx, int fontId) = nullptr;
  void* ctx = nullptr;
};

/// Looks `fontId` up in `fonts`. On a miss, asks `hook` once; a family it supplies is registered
/// (its scale in `scales`, when not 1) and returned. Returns nullptr when neither knows the ID. A
/// hit costs exactly a map find.
template <typename Family>
const Family* findOrResolveFont(std::map<int, Family>& fonts, std::map<int, float>& scales, const int fontId,
                                const FontResolverHook<Family>& hook) {
  const auto it = fonts.find(fontId);
  if (it != fonts.end()) return &it->second;
  if (!hook.resolve) return nullptr;
  std::optional<ResolvedFont<Family>> resolved = hook.resolve(hook.ctx, fontId);
  if (!resolved) return nullptr;
  if (resolved->baseScale != 1.0f) scales[fontId] = resolved->baseScale;
  return &fonts.emplace(fontId, resolved->family).first->second;
}

/// The scale `fontId` is drawn at. An ID neither map knows yet is resolved first: layout often asks
/// for a font's scale before anything else about it, and a stand-in's scale must count from the
/// first measurement.
template <typename Family>
float resolvedBaseScale(std::map<int, Family>& fonts, std::map<int, float>& scales, const int fontId,
                        const FontResolverHook<Family>& hook) {
  auto it = scales.find(fontId);
  if (it != scales.end()) return it->second;
  if (!hook.resolve || fonts.count(fontId) != 0) return 1.0f;
  findOrResolveFont(fonts, scales, fontId, hook);
  it = scales.find(fontId);
  return it != scales.end() ? it->second : 1.0f;
}
