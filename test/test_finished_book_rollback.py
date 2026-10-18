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
code = '\n#include <cassert>\n#include <functional>\n#include <iterator>\n#include <map>\n#include <set>\n#include <string>\n#define LOG_ERR(...) ((void)0)\n#define LOG_INF(...) ((void)0)\nstruct Store {\n std::set<std::string> paths;\n bool exists(const char* p){return paths.contains(p);}\n bool rename(const char* from,const char* to){if(!paths.contains(from)||paths.contains(to))return false;paths.erase(from);paths.insert(to);return true;}\n} Storage;\nstruct ClippingStore {static inline std::map<std::string,std::string> history;\n static bool moveBook(const std::string& from,const std::string& to){if(!Storage.rename(from.c_str(),to.c_str()))return false;\n history[to]=history[from];history.erase(from);return true;}};\nstruct Recent {void updatePath(const std::string&,const std::string&,const std::string&,const std::string&){}} RECENT_BOOKS;\nstruct State {std::string openEpubPath;void saveToFile(){}} APP_STATE;\n'
code += method('void moveFinishedBookToReadFolder(') + "\n"
code += 'int main(){ const std::string from="/Books/book.epub",to="/Read/book.epub";\n Storage.paths={from,from+".key",from+".rights",to+".rights"};ClippingStore::history[from]="saved note";\n moveFinishedBookToReadFolder(from,to,"");\n assert(Storage.exists(from.c_str())&&!Storage.exists(to.c_str()));\n assert(ClippingStore::history.at(from)=="saved note"&&!ClippingStore::history.contains(to));\n}\n'
with tempfile.TemporaryDirectory(prefix="reader-fix-test-") as directory:
    cpp,binary=Path(directory)/"check.cpp",Path(directory)/"check"
    cpp.write_text(code)
    subprocess.run(["c++","-std=c++20",str(cpp),"-o",str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
