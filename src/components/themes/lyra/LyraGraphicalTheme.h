#pragma once

#include "components/themes/lyra/LyraTheme.h"

/**
 * LyraGraphicalTheme
 *
 * Variante gráfica del tema Lyra:
 * - Home con categorías en grid (iconos grandes)
 * - Card de “Continue reading” con resumen del libro activo
 *
 * Implementado como tema independiente para evitar regresiones.
 */
class LyraGraphicalTheme : public LyraTheme {
 public:
  // Menú Home: renderizado en grid (tiles)
  void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                      const std::function<std::string(int)>& buttonLabel,
                      const std::function<UIIcon(int)>& rowIcon) const override;

  // Libro activo: card con portada + resumen
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           const int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           std::function<bool()> storeCoverBuffer) const override;

 private:
  static std::string basename(const std::string& path);
  static std::string fileExtLower(const std::string& path);
};
