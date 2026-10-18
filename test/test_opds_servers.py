"""Check production OPDS JSON streaming with an oversized first record."""
from pathlib import Path
import json
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "src/network/CrossPointWebServer.cpp").read_text()
start = source.index("void CrossPointWebServer::handleGetOpdsServers() const")
end = source.index("{", start) + 1
depth = 1
while depth:
    depth += (source[end] == "{") - (source[end] == "}")
    end += 1
method = source[start:end]
code = r'''
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#define LOG_DBG(...) ((void)0)
constexpr int CONTENT_LENGTH_UNKNOWN=-1;
void yield(){}
void resetTaskWatchdogIfSubscribed(){}
struct Print {virtual ~Print()=default;virtual size_t write(uint8_t)=0;
 size_t write(const uint8_t* data,size_t size){for(size_t i=0;i<size;++i)write(data[i]);return size;}};
struct WebServer {std::string output;void setContentLength(int){}void send(int,const char*,const char*){}
 void sendContent(const char* data,size_t size){output.append(data,size);}
 void sendContent(const char* data){output+=data;}};
struct Slot {std::string* value=nullptr;
 void operator=(const std::string& text){if(value)*value=text;}
 void operator=(size_t){}void operator=(bool){}
 template<class T>T to(){return {};}};
struct JsonObject {Slot operator[](const char*){return {};}};
struct JsonArray {template<class T>T add(){return {};}};
struct JsonDocument {std::string name;void clear(){name.clear();}
 Slot operator[](const char* key){return {std::strcmp(key,"name")==0?&name:nullptr};}};
size_t serializeJson(const JsonDocument& doc,Print& output){
 const std::string record="{\"name\":\""+doc.name+"\"}";
 return output.write(reinterpret_cast<const uint8_t*>(record.data()),record.size());}
size_t serializeJson(const JsonDocument& doc,char* output,size_t capacity){
 const std::string record="{\"name\":\""+doc.name+"\"}";const size_t count=std::min(capacity,record.size());
 std::memcpy(output,record.data(),count);if(count<capacity)output[count]='\0';return count;}
struct Header {std::string name,value;};
struct Server {std::string name,url,username,password;std::vector<Header> customHeaders;};
struct Store {std::vector<Server> servers;const auto& getServers()const{return servers;}} OPDS_STORE;
struct CrossPointWebServer {WebServer* server;void handleGetOpdsServers()const;};
''' + method + r'''
int main(){
 for(unsigned count:{0u,1u,2u}){
  OPDS_STORE.servers.clear();
  if(count>0)OPDS_STORE.servers.push_back({std::string(1100,'x')});
  if(count>1)OPDS_STORE.servers.push_back({"second"});
  WebServer output;CrossPointWebServer api{&output};api.handleGetOpdsServers();
  std::cout<<output.output<<'\n';
 }
}
'''
with tempfile.TemporaryDirectory(prefix="opds-servers-test-") as directory:
    cpp = Path(directory) / "stream.cpp"
    binary = Path(directory) / "stream"
    cpp.write_text(code)
    subprocess.run(["c++", "-std=c++20", str(cpp), "-o", str(binary)], check=True)
    output = subprocess.check_output([str(binary)], text=True).splitlines()
assert [json.loads(line) for line in output] == [
    [], [{"name": "x" * 1100}], [{"name": "x" * 1100}, {"name": "second"}]
]
print("OPDS empty, oversized-first, and following-record JSON checks passed")
