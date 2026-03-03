#pragma once

class GfxRenderer;

namespace core {

class BuiltinFontRegistry {
 public:
  static void registerUiFonts(GfxRenderer& renderer);
  static bool registerAllFonts(GfxRenderer& renderer);
};

}  // namespace core
