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
code = '\n#include <algorithm>\n#include <cassert>\n#include <cstdint>\n#include <cstring>\n#include <memory>\n#include <optional>\n#include <string>\n#include <vector>\n#define LOG_DBG(...) ((void)0)\nstruct Epub {std::string getCachePath(){return "cache";}int getSpineIndexForTextReference(){return 3;}};\nstruct HalFile {int read(void*,size_t);};\nstruct Store {\n std::vector<uint8_t> bytes;\n bool openFileForRead(const char*,const std::string&,HalFile&){return !bytes.empty();}\n} Storage;\nint HalFile::read(void* p,size_t n){n=std::min(n,Storage.bytes.size());std::memcpy(p,Storage.bytes.data(),n);return n;}\nstruct EpubReaderActivity {\n std::shared_ptr<Epub> epub=std::make_shared<Epub>();\n int currentSpineIndex=0,nextPageNumber=0,cachedSpineIndex=0,cachedChapterTotalPageCount=0;\n std::optional<uint32_t> cachedVisibleTextOffset;\n void loadProgress();\n};\n'
progress = source.split("  bool loadedProgress = false;",1)[1].split("  loadLinkStack();",1)[0]
code += "void EpubReaderActivity::loadProgress(){bool loadedProgress=false;" + progress + "}\n"
code += 'int main(){ for(size_t bytes:{4,6,10}) {\n  EpubReaderActivity activity;Storage.bytes.assign(bytes,0);Storage.bytes[2]=2;\n  activity.loadProgress();assert(activity.currentSpineIndex==0&&activity.nextPageNumber==2);\n }\n EpubReaderActivity activity;Storage.bytes.clear();activity.loadProgress();assert(activity.currentSpineIndex==3);\n\n}\n'
with tempfile.TemporaryDirectory(prefix="reader-fix-test-") as directory:
    cpp,binary=Path(directory)/"check.cpp",Path(directory)/"check"
    cpp.write_text(code)
    subprocess.run(["c++","-std=c++20",str(cpp),"-o",str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
