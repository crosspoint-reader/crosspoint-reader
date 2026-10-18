"""Check OTA's production clock gate and authenticated download wiring."""
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
source=(ROOT/'src/network/OtaUpdater.cpp').read_text()
start=source.index('bool prepareOtaClock()')
end=source.index('\n}  // namespace',start)
clock=source[start:end]
# Both network calls must opt into the verified, HTTPS-only downloader policy.
assert source.count('{}, OTA_TRUST_ROOTS)') == 2
assert source.index('if (!prepareOtaClock())') < source.index('HttpDownloader::fetchUrl(')
install=source[source.index('OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate('):]
assert install.index('if (!prepareOtaClock())') < install.index('esp_ota_begin(') < install.index('HttpDownloader::fetchUrl(')
http=(ROOT/'src/network/HttpDownloader.cpp').read_text()
assert 'options.requireHttps = rootCA != nullptr;' in http
assert 'http.setCACert(rootCA)' in http
code=r'''
#include <cassert>
#include <cstdint>
#include <ctime>
#define LOG_ERR(...) ((void)0)
int syncCalls=0;bool synced=true;int64_t floorValue=1800000000,nowValue=1800000000;
namespace trustedtime {
 bool syncNow(uint32_t ms){assert(ms==5000);++syncCalls;return synced;}
 int64_t trustedNow(){return floorValue;}
}
time_t fakeTime(time_t*){return nowValue;}
#define time fakeTime
'''+clock+r'''
int main(){
 assert(prepareOtaClock());assert(syncCalls==1);
 synced=false;assert(!prepareOtaClock());synced=true;
 floorValue=0;assert(!prepareOtaClock());floorValue=1800000000;
 nowValue=floorValue-1;assert(!prepareOtaClock());
 nowValue=floorValue+1;assert(prepareOtaClock());
}
'''
with tempfile.TemporaryDirectory(prefix='ota-clock-') as directory:
    cpp,exe=Path(directory)/'clock.cpp',Path(directory)/'clock'
    cpp.write_text(code)
    subprocess.run(['c++','-std=c++20',str(cpp),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('OTA rejects invalid or backward clocks before authenticated fetches and partition writes')
