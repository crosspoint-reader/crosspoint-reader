"""Exercise production font replacement and FAT identity methods with I/O faults."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
web = (ROOT / "src/network/CrossPointWebServer.cpp").read_text()
def body(text, signature):
    start = text.index(signature)
    pos = text.index("{", start)
    end = pos + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]
header = body(web, "bool validFontHeader(")
start = web.index("    case UPLOAD_FILE_END: {", web.index("void CrossPointWebServer::handleFontUploadData()"))
end = web.index("    case UPLOAD_FILE_ABORTED:", start)
complete = web[start:end].split("{", 1)[1].rsplit("}", 1)[0].replace("      break;", "      return;")
code = r'''
#include <HalStorage.h>
#include <cassert>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#define LOG_DBG(...) ((void)0)
#define LOG_ERR(...) ((void)0)
struct FontUpload {HalFile file; std::string filePath; bool valid=true,isVector=false;
 size_t bufferPos=0,bytesWritten=0;std::unique_ptr<uint8_t[]> buffer;};
struct FontSystem {bool dirty=false;void markRegistryDirty(){dirty=true;}};
struct CrossPointWebServer {FontUpload fontUpload;FontSystem sdFontSystem;void complete();};
void resetTaskWatchdogIfSubscribed() {}
''' + header + "\nvoid CrossPointWebServer::complete(){" + complete + "}\n"
code += r'''
int main(){
 const std::string dest="/fonts/X/font.cpfont",temp=dest+".part";
 const std::vector<uint8_t> old={67,80,70,79,78,84,0,0,1},fresh={67,80,70,79,78,84,0,0,2};
 for(int failure=0;failure<6;++failure){
  fake::reset();Storage.mkdir("/fonts/X");HalFile original;
  assert(Storage.openFileForWrite("TEST",dest,original));assert(original.write(old.data(),old.size())==old.size());original.close();
  CrossPointWebServer server;auto& upload=server.fontUpload;upload.filePath=temp;
  assert(Storage.openFileForWrite("TEST",temp,upload.file));
  auto bytes=fresh;if(failure==0)bytes[0]='X';assert(upload.file.write(bytes.data(),bytes.size())==bytes.size());
  if(failure==1)upload.valid=false;
  if(failure==2)fake::failClosePath=temp;
  if(failure==3)fake::failRename=1;
  if(failure==4){fake::renameFailures[temp]=1;fake::renameFailures[dest+".bak"]=1;}
  server.complete();
  if(failure==4){assert(!Storage.exists(dest.c_str()));assert(fake::files.at(dest+".bak")->bytes==old);assert(server.sdFontSystem.dirty);}
  else assert(fake::files.at(dest)->bytes==(failure==5?fresh:old));
  assert(upload.valid==(failure==5));assert(!Storage.exists(temp.c_str()));
 }
}
'''
with tempfile.TemporaryDirectory(prefix="transfer-storage-test-") as directory:
    cpp = Path(directory) / "font.cpp"
    binary = Path(directory) / "font"
    cpp.write_text(code)
    subprocess.run(["c++", "-std=c++20", "-I" + str(ROOT / "test/library_builder/stubs"), str(cpp), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

print("Font replacement faults passed")
