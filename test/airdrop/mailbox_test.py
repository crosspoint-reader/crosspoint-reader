from pathlib import Path
import subprocess, tempfile
r=Path(__file__).resolve().parents[2]
s=(r/'vendor/esp32drop/src/airdrop/port/ad_port_esp32.cpp').read_text()
mail=s[s.index('enum class StreamSlot'):s.index('// ---- /Upload receive + decode')]
poll=s[s.index('  auto pending = StreamSlot::Pending;',s.index('void ad_poll(void)')):s.index('\n}',s.index('void ad_poll(void)'))]
code='''#include <atomic>
#include <thread>
#include <future>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdint>
using namespace std::chrono_literals;
enum class AdStreamEvent { Begin, Data, Finish, Abort };
using ad_stream_cb_t=bool(*)(void*,AdStreamEvent,const uint8_t*,uint32_t);
static std::atomic<bool> g_stop{false};
static std::atomic<uint32_t> clockMs{0};
uint32_t millis(){return clockMs.load();}
#define pdMS_TO_TICKS(x) (x)
void vTaskDelay(int){std::this_thread::sleep_for(1ms);}
'''+mail+'\nvoid service(){\n'+poll+'}\n'+'''
std::atomic<bool> entered{false},releaseCallback{false};
bool result=true;
bool cb(void*,AdStreamEvent,const uint8_t* p,uint32_t n){
 entered=true;
 while(!releaseCallback.load())std::this_thread::sleep_for(1ms);
 assert(n==1 && *p==42);return result;
}
int main(){
 for(int mode=0;mode<6;++mode){
  g_stop=false;clockMs=0;entered=false;releaseCallback=false;result=mode!=1;
  ad_on_stream(cb,nullptr);
  uint8_t data=42;
  auto producer=std::async(std::launch::async,[&]{return stream_deliver(AdStreamEvent::Data,&data,1);});
  while(g_stream_slot.load()!=StreamSlot::Pending)std::this_thread::sleep_for(1ms);
  if(mode==2||mode==3){
   if(mode==2)g_stop=true;else clockMs=10001;
   assert(producer.wait_for(1s)==std::future_status::ready);
   assert(!producer.get() && g_stream_slot.load()==StreamSlot::Idle);
   service();assert(!entered.load());continue;
  }
  auto consumer=std::async(std::launch::async,service);
  while(!entered.load())std::this_thread::sleep_for(1ms);
  if(mode==4)g_stop=true;
  if(mode==5)clockMs=10001;
  assert(producer.wait_for(20ms)==std::future_status::timeout);
  releaseCallback=true;consumer.get();
  assert(producer.get()==(mode==0));
  assert(g_stream_slot.load()==StreamSlot::Idle);
 }
 puts("actual stream mailbox: success, refusal, pending/processing Stop and timeout passed");
}
'''
with tempfile.TemporaryDirectory(prefix='airdrop-test-') as td:
 source=Path(td)/'test.cpp'
 binary=Path(td)/'test'
 Path(str(source)).write_text(code)
 subprocess.run(['c++','-std=c++17','-pthread','-fsanitize=address,undefined','-g',str(source),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
