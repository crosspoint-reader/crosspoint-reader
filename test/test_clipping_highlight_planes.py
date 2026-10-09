"""Run with python3 test/test_clipping_highlight_planes.py (requires a host C++ compiler)."""

from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "src/activities/reader/EpubReaderActivity.cpp").read_text()
fill = "const auto fillHighlight =" + source.split("const auto fillHighlight =", 1)[1].split("\n  uint16_t pageWordIndex", 1)[0]
render = "auto renderGrayscalePass =" + source.split("auto renderGrayscalePass =", 1)[1].split("\n\n  if (pageHasImagesNeedingDecode)", 1)[0]
harness = r"""
#include <cassert>
#include <memory>
struct BoardConfig {
  static inline bool a4 = false;
  static bool isEegoA4() { return a4; }
};
enum class Color { LightGray };
struct GfxRenderer {
  enum RenderMode { BW, GRAYSCALE_LSB, GRAYSCALE_MSB };
  RenderMode mode = BW;
  unsigned dots = 0, ditherRects = 0;
  RenderMode getRenderMode() const { return mode; }
  void drawPixel(int x, int y, bool state) {
    assert((x & 1) == 0 && (y & 1) == 0 && !state);
    ++dots;
  }
  void fillRectDither(int, int, int, int, Color) { ++ditherRects; }
};
int main() {
  GfxRenderer renderer;
""" + fill + r"""
  for (bool a4 : {false, true}) {
    BoardConfig::a4 = a4;
    for (auto mode : {GfxRenderer::BW, GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
      renderer.mode = mode;
      renderer.dots = renderer.ditherRects = 0;
      fillHighlight(1, 1, 4, 4);
      assert(renderer.dots == (a4 && mode != GfxRenderer::BW ? 4u : 0u));
      assert(renderer.ditherRects == (a4 && mode != GfxRenderer::BW ? 0u : 1u));
    }
  }
  int order = 0;
  struct Page {
    int& order;
    void render(GfxRenderer&, int, int, int) { assert(++order == 2); }
    void renderImages(GfxRenderer&, int, int, int) { assert(++order == 2); }
  };
  auto page = std::make_unique<Page>(order);
  const int fontId = 0, orientedMarginTop = 0, orientedMarginLeft = 0;
  const bool absoluteImageGrayscale = false;
  bool needsTextGrayscale = true;
  auto drawClippingHighlights = [&](Page&, int, int, int) { assert(++order == 1); };
  auto renderStatusBar = [&] { assert(++order == 3); };
""" + render + r"""
  BoardConfig::a4 = true;
  renderGrayscalePass();
  assert(order == 3);
  order = 0;
  needsTextGrayscale = false;
  renderGrayscalePass();
  assert(order == 3);
}
"""
with tempfile.TemporaryDirectory() as directory:
    cpp = Path(directory) / "highlight.cpp"
    executable = Path(directory) / "highlight"
    cpp.write_text(harness)
    subprocess.run(["c++", "-std=c++20", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
print("Clipping highlight plane regression checks passed")
