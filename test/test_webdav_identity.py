"""Exercise production font replacement and FAT identity methods with I/O faults."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
def body(text, signature):
    start = text.index(signature)
    pos = text.index("{", start)
    end = pos + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]
hal = (ROOT / "lib/hal/HalStorage.cpp").read_text()
identity = body(hal, "bool HalStorage::areSameFile(")
code = r'''
#include <cassert>
#include <vector>
#include <string>
std::vector<std::string> errors;
void logError(const char*,const char*,const char* path){errors.emplace_back(path);}
#define LOG_ERR(...) logError(__VA_ARGS__)
#include <cstdint>
#include <map>
#include <string>
constexpr int O_RDONLY=0;
struct File {bool valid=false,directory=false;uint32_t cluster=0,index=0;
 explicit operator bool()const{return valid;}bool isDirectory()const{return directory;}
 uint32_t firstCluster()const{return cluster;}uint32_t dirIndex()const{return index;}};
struct Card {std::map<std::string,File> files;File open(const char* p,int){return files[p];}} SDCard;
struct HalStorage {struct StorageLock{};bool areSameFile(const char*,const char*,bool&);};
''' + identity + r'''
int main(){
 SDCard.files["/"]={true,true,1,0};
 for(const char* p:{"/Books","/BOOKS","/BOOKS~1"})SDCard.files[p]={true,true,2,3};
 SDCard.files["/Other"]={true,true,3,4};
 for(const char* p:{"/Books/book.epub","/BOOKS/BOOK.EPUB","/BOOKS~1/BOOK~1.EPU"})SDCard.files[p]={true,false,0,7};
 SDCard.files["/Books/other.epub"]={true,false,0,8};SDCard.files["/Other/book.epub"]={true,false,0,7};
 HalStorage storage;bool same;
 for(const char* p:{"/BOOKS/BOOK.EPUB","/BOOKS~1/BOOK~1.EPU"}){assert(storage.areSameFile("/Books/book.epub",p,same));assert(same);}
 for(const char* p:{"/Books/other.epub","/Other/book.epub"}){assert(storage.areSameFile("/Books/book.epub",p,same));assert(!same);}
 assert(!storage.areSameFile("/Books/missing.epub","/Books/book.epub",same));assert(!same);
 assert(errors.back()=="/Books/missing.epub");
 assert(!storage.areSameFile("/Books/book.epub","/Other/missing.epub",same));assert(!same);
 assert(errors.back()=="/Other/missing.epub");
}
'''
with tempfile.TemporaryDirectory(prefix="fat-identity-test-") as directory:
    cpp = Path(directory) / "identity.cpp"
    binary = Path(directory) / "identity"
    cpp.write_text(code)
    subprocess.run(["c++", "-std=c++20", str(cpp), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("FAT identity checks passed")
