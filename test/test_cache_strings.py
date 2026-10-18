"""Exercise the production image and anchor readers with writer-generated strings."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
def method(source, signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]

image = (ROOT / "lib/Epub/Epub/blocks/ImageBlock.cpp").read_text()
section = (ROOT / "lib/Epub/Epub/Section.cpp").read_text()
code = r'''

#include <HalStorage.h>
#include <BufferedFile.h>
#include <Serialization.h>
#include <MemoryManager.h>
#include <algorithm>
#include <cassert>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#define LOG_ERR(...) ((void)0)
struct ImageBlock {
 std::string imagePath,srcPath; int16_t width,height;
 ImageBlock(const std::string& path,const std::string& src,int16_t w,int16_t h):imagePath(path),srcPath(src),width(w),height(h){}
 bool serialize(HalFile&);
 static std::unique_ptr<ImageBlock> deserialize(HalFile&);
};
struct TocItem {std::string anchor;int spineIndex=0;};
struct Epub {
 std::vector<TocItem> toc;
 int getTocIndexForSpineIndex(int)const{return 0;}
 int getTocItemsCount()const{return static_cast<int>(toc.size());}
 const TocItem& getTocItem(int i)const{return toc[i];}
};
struct Parser {std::vector<std::pair<std::string,uint16_t>> anchors;const auto& getAnchors()const{return anchors;}};
struct Build {std::unique_ptr<Parser> parser;};
struct Section {
 std::string path;Epub* epub;int spineIndex=0;std::unique_ptr<Build> build_;
 bool openAnchorMap(HalFile& file,uint16_t& count)const{return file.open(path.c_str(),"rb")&&serialization::tryReadPod(file,count);}
 std::optional<uint16_t> findAnchorDuringBuild(const std::string&)const{return std::nullopt;}
 std::optional<uint16_t> getPageForAnchor(const std::string&)const;
 int getTocIndexForPage(int)const;
};
'''
code += method(image, "bool ImageBlock::serialize(")
code += method(image, "std::unique_ptr<ImageBlock> ImageBlock::deserialize(")
code += "\ntemplate <typename F>\n" + method(section, "bool readAnchorString(")
code += method(section, "std::optional<uint16_t> Section::getPageForAnchor(")
code += method(section, "int Section::getTocIndexForPage(")
code += r'''
int main(int argc,char** argv){
 assert(argc==2);const std::string path=argv[1],longText(4097,'A');
 {
  ImageBlock original(longText,longText,20,30);HalFile file;
  assert(file.open(path.c_str(),"wb"));assert(original.serialize(file));file.close();
  assert(file.open(path.c_str(),"rb"));auto cached=ImageBlock::deserialize(file);assert(cached);
  assert(cached->imagePath==longText&&cached->srcPath==longText&&file.position()==file.size());file.close();
 }
 {
  HalFile file;assert(file.open(path.c_str(),"wb"));
  serialization::writePod(file,uint16_t{2});serialization::writeString(file,longText);serialization::writePod(file,uint16_t{2});
  serialization::writeString(file,"target");serialization::writePod(file,uint16_t{3});file.close();
  Epub epub;epub.toc={{longText,0},{"target",0}};Section section;section.path=path;section.epub=&epub;
  assert(section.getPageForAnchor(longText)==2);assert(section.getPageForAnchor("target")==3);
  assert(section.getTocIndexForPage(2)==0);assert(section.getTocIndexForPage(3)==1);
  const size_t before=freeink::MemoryManager::largestBlock;freeink::MemoryManager::largestBlock=64;
  assert(!section.getPageForAnchor(longText));freeink::MemoryManager::largestBlock=before;
 }
}
'''
with tempfile.TemporaryDirectory(prefix="cache-string-test-") as directory:
    cpp = Path(directory) / "check.cpp"
    binary = Path(directory) / "check"
    cpp.write_text(code)
    subprocess.run(["c++", "-std=c++20", "-I" + str(ROOT / "test/chapter_html_slim_parser/stubs"),
                    "-I" + str(ROOT / "lib/Serialization"), "-I" + str(ROOT / "lib/Memory"),
                    str(cpp), "-o", str(binary)], check=True)
    subprocess.run([str(binary), str(Path(directory) / "cache.bin")], check=True)
print("Long image strings and anchor navigation remain readable; oversized allocations fail")
