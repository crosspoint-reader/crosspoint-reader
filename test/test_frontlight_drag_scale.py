"""Exercise the production touch transform and SDK slider routing during physical rendering."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "lib/GfxRenderer/GfxRenderer.cpp").read_text()
start = source.index("void GfxRenderer::tapToLogical(")
end = source.index("\n// Translate a logical rect", start)
code = r"""
#include <cassert>
#include <cmath>
#include <cstdio>
#include <FreeInkUICore.h>
class GfxRenderer {
public:
  enum Orientation { Portrait, PortraitInverted, LandscapeClockwise, LandscapeCounterClockwise };
  int panelWidth=1216, panelHeight=684;
  float uiScale_=1.6f;
  mutable bool uiScaleActive_=true;
  Orientation orientation=Portrait;
  int lp(int v) const { return static_cast<int>(lroundf(v/uiScale_)); }
  struct PhysicalScope {
    const GfxRenderer& renderer; bool previous;
    PhysicalScope(const GfxRenderer& r):renderer(r),previous(r.uiScaleActive_) { r.uiScaleActive_=false; }
    ~PhysicalScope() { renderer.uiScaleActive_=previous; }
  };
  void tapToLogical(float,float,int&,int&) const;
};
""" + source[start:end] + r"""
int main() {
  using namespace freeink::ui;
  GfxRenderer renderer;
  InteractionBuffer<8> slider;
  slider.addInteraction(Interaction{Rect{100,170,301,40},1,0,
      static_cast<uint16_t>(InputTouch|InputDrag),StateNormal,0});
  const auto drag = [&] {
    int x,y;
    renderer.tapToLogical(.25f,.25f,x,y);
    InputSnapshot input;
    input.touchHeld=true; input.touchX=x; input.touchY=y;
    return slider.route(input).dragPermille;
  };
  assert(drag()==733);
  {
    const GfxRenderer::PhysicalScope painting(renderer);
    const int value=drag();
    if(value!=733) std::fprintf(stderr,"Same finger: expected 733 permille, got %d during rendering\n",value);
    assert(value==733);
    assert(!renderer.uiScaleActive_);
  }
  assert(drag()==733 && renderer.uiScaleActive_);
  InputSnapshot release;
  release.touchReleased=true; release.touchX=release.touchY=-1;
  assert(slider.route(release).dragPermille==733);

  // Orientation mapping stays logical, even during physical rendering.
  int x,y;
  {
    const GfxRenderer::PhysicalScope painting(renderer);
    renderer.orientation=GfxRenderer::PortraitInverted;
    renderer.tapToLogical(.25f,.25f,x,y); assert(x==107 && y==569);
    renderer.orientation=GfxRenderer::LandscapeClockwise;
    renderer.tapToLogical(.25f,.25f,x,y); assert(x==569 && y==320);
    renderer.orientation=GfxRenderer::LandscapeCounterClockwise;
    renderer.tapToLogical(.25f,.25f,x,y); assert(x==190 && y==107);
  }
  // Unscaled boards and input clamping keep their existing behavior.
  renderer.uiScale_=1; renderer.uiScaleActive_=false; renderer.orientation=GfxRenderer::Portrait;
  renderer.tapToLogical(.25f,.25f,x,y); assert(x==512 && y==304);
  renderer.tapToLogical(-1,2,x,y); assert(x==0 && y==0);
}
"""
with tempfile.TemporaryDirectory(prefix="frontlight-drag-") as directory:
    cpp = Path(directory) / "test.cpp"
    cpp.write_text(code)
    binary = Path(directory) / "test"
    sdk = root / "freeink-sdk"
    subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-I" + str(sdk / "libs/ui/FreeInkUI/include"),
                    str(cpp), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("Frontlight drag remains stable during rendering; four orientations and unscaled mapping passed")
