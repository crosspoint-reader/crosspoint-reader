"""Compile reader lifetime/resume methods against host storage and lock contracts."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "src/activities/reader/EpubReaderActivity.cpp").read_text()
def method(signature):
    start = source.index(signature)
    body = source.index("{", start)
    depth = 1
    end = body + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]
code = '\n#include <array>\n#include <cassert>\n#include <cstring>\n#include <cstdint>\n#include <memory>\n#include <optional>\n#include <string>\n#include <set>\n#include <map>\n#include <vector>\n#define LOG_DBG(...) ((void)0)\n#define LOG_ERR(...) ((void)0)\nstruct RenderLock { static inline bool held=false; RenderLock(){assert(!held);held=true;} ~RenderLock(){held=false;} };\nstruct Renderer { bool storeBwBuffer(){assert(RenderLock::held);return true;} void restoreBwBuffer(bool){assert(RenderLock::held);} void displayBuffer(int){assert(RenderLock::held);} };\nstruct Input;\nstruct ReaderToolbarUi { struct Nav {int top=0;void reset(int=0){assert(RenderLock::held);}} n;\n explicit ReaderToolbarUi(Renderer&) {assert(RenderLock::held);} ~ReaderToolbarUi(){assert(RenderLock::held);}\n int route(Input&){assert(RenderLock::held);return 42;}\n void begin(){assert(RenderLock::held);} Nav& nav(){assert(RenderLock::held);return n;} };\nstruct Input {void resetHomeButtonInput(){} bool hasTouch(){return true;}};\nstruct Popup {void dismiss(){assert(RenderLock::held);}};\nstruct Section {int currentPage=2,pageCount=5;~Section(){assert(RenderLock::held);}};\nstruct BoardConfig {static bool isEegoA4(){return false;}};\nstruct HalDisplay {static constexpr int FAST_REFRESH=1;};\nstruct Settings {void saveToFile(){}} SETTINGS;\nstruct FontSystem {void ensureLoaded(Renderer&){assert(RenderLock::held);}} sdFontSystem;\nstruct EpubReaderActivity {\n enum class Overlay {None,Toolbar,Contents,Text,More}; Overlay overlay=Overlay::None;\n Renderer renderer;Input mappedInput;Popup overlayPopup;\n std::unique_ptr<ReaderToolbarUi> toolbarUi;std::unique_ptr<Section> section;\n bool pageBufferStale=false,overlayPageStored=false,panelCursorShown=false,panelHoldJumped=false;\n int focusedTool=0,panelIndex=0,pagesUntilFullRefresh=0,currentSpineIndex=0,nextPageNumber=0,cachedSpineIndex=0,cachedChapterTotalPageCount=0;\n std::optional<uint32_t> cachedVisibleTextOffset;\n int currentTocIndex(){return 1;}void buildMoreActions(){}void settleOverlayRefresh(){assert(RenderLock::held);}\n void renderOverlay(){assert(RenderLock::held);}void pushOverlayRefresh(){assert(RenderLock::held);}void requestUpdate(){}\n bool xteinkClassPanel(){return false;}void discardOverlayPage(){assert(RenderLock::held);overlayPageStored=false;}\n void rememberCurrentContentOffset(){assert(RenderLock::held);}\n int routeForTest();void openOverlay(Overlay);void closeOverlayToPage();void applyReaderTextSettings();void loadProgress();\n};\n'
code += method('void EpubReaderActivity::openOverlay(') + "\n"
code += method('void EpubReaderActivity::closeOverlayToPage()') + "\n"
code += method('void EpubReaderActivity::applyReaderTextSettings()') + "\n"
routing=source.split("  const auto routed = [this] {",1)[1].split("  }();",1)[0]
code += "int EpubReaderActivity::routeForTest(){return [this]{" + routing + "}();}\n"
code += 'int main(){\n for(auto target:{EpubReaderActivity::Overlay::Toolbar,EpubReaderActivity::Overlay::Contents,EpubReaderActivity::Overlay::Text,EpubReaderActivity::Overlay::More}){\n  EpubReaderActivity activity;activity.section=std::make_unique<Section>();\n  activity.openOverlay(target);activity.openOverlay(EpubReaderActivity::Overlay::Toolbar);\n  assert(activity.routeForTest()==42);activity.closeOverlayToPage();assert(!activity.toolbarUi);\n  activity.applyReaderTextSettings();assert(!activity.section);\n }\n\n}\n'
with tempfile.TemporaryDirectory(prefix="reader-fix-test-") as directory:
    cpp,binary=Path(directory)/"check.cpp",Path(directory)/"check"
    cpp.write_text(code)
    subprocess.run(["c++","-std=c++20",str(cpp),"-o",str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
