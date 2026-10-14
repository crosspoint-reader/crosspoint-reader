"""Inject failures into the production section-cache commit method."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "lib/Epub/Epub/Section.cpp").read_text()
def method(signature):
    start = source.index(signature)
    pos = source.index("{", start)
    end = pos + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]
code = r'''
#include <HalStorage.h>
#include <Serialization.h>
#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#define LOG_ERR(...) ((void)0)
constexpr uint8_t SECTION_FILE_VERSION=55,SECTION_FILE_PARTIAL_VERSION = 0xFE - (55 - 28);
constexpr uint32_t HEADER_SIZE=64;
struct Parser {std::vector<std::pair<std::string,uint16_t>> anchors{{"anchor",0}};
 const auto& getAnchors()const{return anchors;}};
struct Lut {uint32_t fileOffset;uint16_t paragraphIndex,listItemIndex;uint32_t visibleTextOffset;};
struct Context {std::unique_ptr<Parser> parser=std::make_unique<Parser>();std::vector<Lut> lut{{64,0,0,0}};};
struct Section {
 HalFile file;std::string filePath="/cache/0.bin";uint16_t builtPageCount_=1;
 std::unique_ptr<Context> build_=std::make_unique<Context>();
 std::string binTmpPath()const{return filePath+".part";}
 bool commitBuildFile(uint8_t,uint32_t,uint32_t);
};
''' + method("bool Section::commitBuildFile(") + r'''
void setup(Section& section){
 fake::reset();Storage.mkdir("/cache");HalFile original;
 assert(Storage.openFileForWrite("TEST",section.filePath,original));const uint8_t bytes[]={1,2,3};
 assert(original.write(bytes,3)==3);original.close();
 assert(Storage.openFileForWrite("TEST",section.binTmpPath(),section.file));
 section.file.node->bytes.resize(74);assert(section.file.seekSet(74));fake::writesByPath.clear();
}
int main(){
 for(uint8_t version:{SECTION_FILE_VERSION,SECTION_FILE_PARTIAL_VERSION}){
  Section successful;setup(successful);assert(successful.commitBuildFile(version,10,100));
  const unsigned writes=fake::writesByPath[successful.binTmpPath()];
  assert(fake::files.at(successful.filePath)->bytes.front()==version);
  for(unsigned failure=0;failure<writes+2;++failure){
   Section section;setup(section);const auto old=fake::files.at(section.filePath)->bytes;
   if(failure<writes)fake::failWrite=failure;
   else if(failure==writes)fake::failClosePath=section.binTmpPath();
   else fake::failRename=1;
   assert(!section.commitBuildFile(version,10,100));
   assert(fake::files.at(section.filePath)->bytes==old);
   assert(!Storage.exists(section.binTmpPath().c_str()));
  }
 }
}
'''
with tempfile.TemporaryDirectory(prefix="section-storage-test-") as directory:
    cpp = Path(directory) / "check.cpp"
    binary = Path(directory) / "check"
    cpp.write_text(code)
    subprocess.run(["c++", "-std=c++20", "-I" + str(ROOT / "test/library_builder/stubs"),
                    "-I" + str(ROOT / "lib/Serialization"), str(cpp), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("Completed/partial section write, close and replacement-failure checks passed")
