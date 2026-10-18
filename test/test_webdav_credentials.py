"""Exercise production WebDAV guards with the shared in-memory filesystem."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT/'src/network/WebDAVHandler.cpp').read_text()

def method(signature):
    start=source.index(signature)
    end=source.index('{',start)+1
    depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}')
        end+=1
    return source[start:end]

code=r'''
#include <HalStorage.h>
#include <ProtectedPaths.h>
#include <cassert>
#include <cctype>
#define LOG_DBG(...) ((void)0)
class String:public std::string {
 public:
 using std::string::string;
 String(const std::string& s):std::string(s){}
 operator const char*()const{return c_str();}
 char charAt(size_t n)const{return at(n);}
 int indexOf(char c,size_t n=0)const{auto p=find(c,n);return p==npos?-1:static_cast<int>(p);}
 int lastIndexOf(char c)const{auto p=rfind(c);return p==npos?-1:static_cast<int>(p);}
 String substring(size_t a,size_t b=npos)const{return substr(a,b==npos?npos:b-a);}
 bool startsWith(const char* s)const{return starts_with(s);}
 bool endsWith(const char* s)const{return ends_with(s);}
 bool equals(const char* s)const{return *this==s;}
 void trim(){while(!empty()&&std::isspace(static_cast<unsigned char>(back())))pop_back();
  while(!empty()&&std::isspace(static_cast<unsigned char>(front())))erase(0,1);}
};
struct WebServer {
 String path="/";int status=0;std::vector<std::string> entries;
 void send(int s,const char*,const char*){status=s;}
 void sendContent(const char*){}void setContentLength(int){}
};
constexpr int CONTENT_LENGTH_UNKNOWN=-1,RAW_START=0,RAW_WRITE=1,RAW_END=2,RAW_ABORTED=3;
struct HTTPRaw{int status;uint8_t* buf=nullptr;size_t currentSize=0,totalSize=0;};
void yield(){}void resetTaskWatchdogIfSubscribed(){}
const char* HIDDEN_ITEMS[]={"System Volume Information"};
const char* FIXED_DATE="date";
struct WebDAVHandler {
 String _putPath;HalFile _putFile;bool _putOk=false,_putExisted=false;
 String getRequestPath(WebServer& s)const{return s.path;}
 int getDepth(WebServer&)const{return 1;}
 void sendPropEntry(WebServer& s,const String& p,bool,size_t,const String&)const{s.entries.push_back(p);}
 bool isProtectedPath(const String&)const;void handlePropfind(WebServer&);void raw(WebServer&,const String&,HTTPRaw&);
};
'''
code+='\n'.join(method(s) for s in ['bool WebDAVHandler::isProtectedPath(', 'void WebDAVHandler::handlePropfind(', 'void WebDAVHandler::raw('])
code+=r'''
int main(){
 fake::reset();Storage.mkdir("/");Storage.mkdir("/.crosspoint");
 fake::add("/.crosspoint/wifi.json","credential");fake::add("/.crosspoint/wifi.json.davtmp","sentinel");
 fake::add("/book.epub");fake::add("/CRO~ABCD");
 WebDAVHandler dav;
 for(const char* p:{"/.crosspoint/wifi.json","/ .crosspoint/wifi.json","/CROSSP~1/WIFI~1.JSO","/CRO~ABCD/WIF~ABCD.JSO","/System Volume Information/file"}){
  WebServer server;server.path=p;dav.handlePropfind(server);assert(server.status==403&&server.entries.empty());
  assert(dav.isProtectedPath(p));
 }
 WebServer listing;dav.handlePropfind(listing);assert(listing.status==207);
 assert((listing.entries==std::vector<std::string>{"/","/book.epub"}));
 for(const char* p:{"/.crosspoint/wifi.json","/.crosspoint/opds.json","/CRO~ABCD/WIF~ABCD.JSO"}){
  fake::add(p,"credential");const std::string temp=std::string(p)+".davtmp";fake::add(temp,"sentinel");
  const auto original=fake::files.at(p)->bytes, backup=fake::files.at(temp)->bytes;
  WebServer server;server.path=p;
  for(int phase:{RAW_START,RAW_WRITE,RAW_END,RAW_ABORTED}){HTTPRaw raw{phase};dav.raw(server,"",raw);}
  assert(!dav._putOk&&fake::files.at(p)->bytes==original&&fake::files.at(temp)->bytes==backup);
 }
 WebServer server;server.path="/ordinary.epub";HTTPRaw start{RAW_START};dav.raw(server,"",start);
 assert(dav._putOk&&Storage.exists("/ordinary.epub.davtmp"));
 HTTPRaw abort{RAW_ABORTED};dav.raw(server,"",abort);assert(!Storage.exists("/ordinary.epub.davtmp"));
}
'''
with tempfile.TemporaryDirectory(prefix='webdav-credentials-') as directory:
    cpp,exe=Path(directory)/'test.cpp',Path(directory)/'test'
    cpp.write_text(code)
    subprocess.run(['c++','-std=c++20','-I'+str(ROOT/'test/library_builder/stubs'),'-I'+str(ROOT/'src/network'),str(cpp),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('WebDAV listings and upload callbacks protect credential stores and aliases')
