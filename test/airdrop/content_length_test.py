# Compile the firmware's actual Content-Length loop with its HTTP reader.
# The progress stub models a bounded consuming decoder; parser/codec tests are separate.
from pathlib import Path
import subprocess, tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'vendor/esp32drop/src/airdrop/port/ad_port_esp32.cpp').read_text()
a=s.index('    HttpSink sk{};',s.index('} else if (clen > 0) {',s.index('static void rx_upload')))
b=s.index('\n  }\n\n  /* STREAMED BODIES',a)
loop=s[a:b]
code=r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "airdrop/core/ad_http.h"
static constexpr uint32_t g_rx_max=30u*1024u*1024u;
static bool g_stream_cb=true;
static std::atomic<bool> g_stop{false};
static constexpr int RXF_CAP=2;
struct Pump{uint32_t in_total=0;int fail=0;};
static bool reject=false;
bool rx_sink_grow(HttpSink*,uint32_t){assert(false);return false;}
void rx_sink_progress(HttpSink* sk){auto p=static_cast<Pump*>(sk->ctx);p->in_total+=sk->len;sk->len=0;p->fail=reject;}
struct Source{uint32_t left;int cut=-1;};
int readSource(HttpRdr* r,unsigned char* out,int max){auto s=static_cast<Source*>(r->ctx);if(s->cut==0){s->cut=-1;return 0;}auto n=std::min<uint32_t>(s->left,max);if(s->cut>0)n=std::min<uint32_t>(n,s->cut);if(!n)return -1;memset(out,42,n);s->left-=n;if(s->cut>0)s->cut-=n;return n;}
void check(uint32_t advertised,uint32_t actual,int cut,bool refuse,bool stop,bool expected){
 std::vector<uint8_t> memory(320*1024);
 uint8_t* gz=memory.data();uint32_t cap=memory.size(),len=0,body_total=0;
 bool overflow=false,clean_end=false;Pump pump;int clen=advertised;
 Source src{actual,cut};HttpRdr reader;http_rdr_init(&reader,&src,readSource);auto r=&reader;
 reject=refuse;g_stop=stop;
''' +loop+r'''
 assert(clean_end&&!overflow&&!r->poisoned ? expected : !expected);
 if(expected)assert(body_total==actual);
 assert(cap==320*1024);
 if(advertised > g_rx_max+1048576){assert(pump.fail==RXF_CAP);assert(body_total==0);}
}
int main(){
 check(g_rx_max,g_rx_max,-1,false,false,true);
 check(4096,4096,-1,false,false,true);
 check(4097,4097,-1,false,false,true);
 check(4097,3000,-1,false,false,false);
 check(8192,8192,1000,false,false,false);
 check(g_rx_max,g_rx_max,-1,true,false,false);
 check(g_rx_max,g_rx_max,-1,false,true,false);
 check(g_rx_max+1048577,g_rx_max+1048577,-1,false,false,false);
 puts("actual Content-Length loop: 30 MiB bounded consumption, tails, truncation, poisoned read, refusal, Stop and cap passed");
}
'''
with tempfile.TemporaryDirectory(prefix='airdrop-test-') as td:
 source=Path(td)/'test.cpp'
 binary=Path(td)/'test'
 Path(str(source)).write_text(code)
 subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined','-g','-I'+str(root/'vendor/esp32drop/src'),str(source),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
