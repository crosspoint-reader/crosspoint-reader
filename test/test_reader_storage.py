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
progress = source.split("  bool loadedProgress = false;", 1)[1].split("  loadLinkStack();", 1)[0]
code = r'''
#include <array>
#include <cassert>
#include <cstring>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <set>
#include <map>
#include <vector>
#define LOG_DBG(...) ((void)0)
#define LOG_ERR(...) ((void)0)
struct RenderLock { static inline bool held=false; RenderLock(){assert(!held);held=true;} ~RenderLock(){held=false;} };
struct Renderer { bool storeBwBuffer(){assert(RenderLock::held);return true;} void restoreBwBuffer(bool){assert(RenderLock::held);} void displayBuffer(int){assert(RenderLock::held);} };
struct ReaderToolbarUi { struct Nav {int top=0;void reset(int=0){assert(RenderLock::held);}} n;
 explicit ReaderToolbarUi(Renderer&) {assert(RenderLock::held);} ~ReaderToolbarUi(){assert(RenderLock::held);}
 void begin(){assert(RenderLock::held);} Nav& nav(){assert(RenderLock::held);return n;} };
struct Input {void resetHomeButtonInput(){} bool hasTouch(){return true;}};
struct Popup {void dismiss(){assert(RenderLock::held);}};
struct Section {int currentPage=2,pageCount=5;~Section(){assert(RenderLock::held);}};
struct BoardConfig {static bool isEegoA4(){return false;}};
struct HalDisplay {static constexpr int FAST_REFRESH=1;};
struct Settings {void saveToFile(){}} SETTINGS;
struct FontSystem {void ensureLoaded(Renderer&){assert(RenderLock::held);}} sdFontSystem;
struct Epub {std::string getCachePath(){return "cache";}int getSpineIndexForTextReference(){return 3;}};
struct HalFile {int read(void* p,size_t n);};
struct Store {
 std::vector<uint8_t> bytes;std::set<std::string> paths;
 bool openFileForRead(const char*,const std::string&,HalFile&){return !bytes.empty();}
 bool exists(const char* p){return paths.contains(p);}
 bool rename(const char* from,const char* to){if(!paths.contains(from)||paths.contains(to))return false;paths.erase(from);paths.insert(to);return true;}
} Storage;
struct ClippingStore {static inline std::map<std::string,std::string> history;
 static bool moveBook(const std::string& from,const std::string& to){if(!Storage.rename(from.c_str(),to.c_str()))return false;
 history[to]=history[from];history.erase(from);return true;}};
struct Recent {void updatePath(const std::string&,const std::string&,const std::string&,const std::string&){}} RECENT_BOOKS;
struct State {std::string openEpubPath;void saveToFile(){}} APP_STATE;
#define LOG_INF(...) ((void)0)
int HalFile::read(void* p,size_t n){n=std::min(n,Storage.bytes.size());std::memcpy(p,Storage.bytes.data(),n);return n;}
struct EpubReaderActivity {
 enum class Overlay {None,Toolbar,Contents,Text,More}; Overlay overlay=Overlay::None;
 Renderer renderer;Input mappedInput;Popup overlayPopup;
 std::unique_ptr<ReaderToolbarUi> toolbarUi;std::unique_ptr<Section> section;std::shared_ptr<Epub> epub=std::make_shared<Epub>();
 bool pageBufferStale=false,overlayPageStored=false,panelCursorShown=false,panelHoldJumped=false;
 int focusedTool=0,panelIndex=0,pagesUntilFullRefresh=0,currentSpineIndex=0,nextPageNumber=0,cachedSpineIndex=0,cachedChapterTotalPageCount=0;
 std::optional<uint32_t> cachedVisibleTextOffset;
 int currentTocIndex(){return 1;}void buildMoreActions(){}void settleOverlayRefresh(){assert(RenderLock::held);}
 void renderOverlay(){assert(RenderLock::held);}void pushOverlayRefresh(){assert(RenderLock::held);}void requestUpdate(){}
 bool xteinkClassPanel(){return false;}void discardOverlayPage(){assert(RenderLock::held);overlayPageStored=false;}
 void rememberCurrentContentOffset(){assert(RenderLock::held);}
 void openOverlay(Overlay);void closeOverlayToPage();void applyReaderTextSettings();void loadProgress();
};
'''
for signature in ["void EpubReaderActivity::openOverlay(", "void EpubReaderActivity::closeOverlayToPage()", "void EpubReaderActivity::applyReaderTextSettings()"]:
    code += method(signature) + "\n"
code += "void EpubReaderActivity::loadProgress(){bool loadedProgress=false;" + progress + "}\n"
code += method("void moveFinishedBookToReadFolder(") + "\n"
code += r'''
int main(){
 for(auto target:{EpubReaderActivity::Overlay::Toolbar,EpubReaderActivity::Overlay::Contents,EpubReaderActivity::Overlay::Text,EpubReaderActivity::Overlay::More}){
  EpubReaderActivity activity;activity.section=std::make_unique<Section>();
  activity.openOverlay(target);activity.openOverlay(EpubReaderActivity::Overlay::Toolbar);
  activity.closeOverlayToPage();assert(!activity.toolbarUi);
  activity.applyReaderTextSettings();assert(!activity.section);
 }
 for(size_t bytes:{4,6,10}) {
  EpubReaderActivity activity;Storage.bytes.assign(bytes,0);Storage.bytes[2]=2;
  activity.loadProgress();assert(activity.currentSpineIndex==0&&activity.nextPageNumber==2);
 }
 EpubReaderActivity activity;Storage.bytes.clear();activity.loadProgress();assert(activity.currentSpineIndex==3);
 const std::string from="/Books/book.epub",to="/Read/book.epub";
 Storage.paths={from,from+".key",from+".rights",to+".rights"};ClippingStore::history[from]="saved note";
 moveFinishedBookToReadFolder(from,to,"");
 assert(Storage.exists(from.c_str())&&!Storage.exists(to.c_str()));
 assert(ClippingStore::history.at(from)=="saved note"&&!ClippingStore::history.contains(to));
}
'''
with tempfile.TemporaryDirectory(prefix="reader-storage-test-") as directory:
    cpp = Path(directory) / "check.cpp"
    binary = Path(directory) / "check"
    cpp.write_text(code)
    subprocess.run(["c++", "-std=c++20", str(cpp), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("Reader lifetime and saved-spine-zero checks passed")
