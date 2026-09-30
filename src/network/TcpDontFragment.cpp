#include "TcpDontFragment.h"

#include <Logging.h>
#include <WiFi.h>
#include <esp_netif_net_stack.h>
#include <lwip/inet_chksum.h>
#include <lwip/ip.h>
#include <lwip/netif.h>
#include <lwip/prot/ip4.h>
#include <lwip/tcpip.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {
netif_output_fn s_originalOutput = nullptr;

// Runs in the lwIP tcpip task with p->payload pointing at the finished IPv4 header. Header fields are
// read and written with memcpy: the payload pointer carries no alignment guarantee for wider types.
err_t outputWithDontFragment(struct netif* netif, struct pbuf* p, const ip4_addr_t* ipaddr) {
  if (p->len >= IP_HLEN) {
    auto* hdr = static_cast<uint8_t*>(p->payload);
    const uint8_t versionAndLength = hdr[offsetof(struct ip_hdr, _v_hl)];
    const uint8_t protocol = hdr[offsetof(struct ip_hdr, _proto)];
    const uint16_t headerLength = static_cast<uint16_t>((versionAndLength & 0x0f) * 4);
    uint16_t offsetBe = 0;
    memcpy(&offsetBe, hdr + offsetof(struct ip_hdr, _offset), sizeof(offsetBe));
    const uint16_t offset = lwip_ntohs(offsetBe);

    // Whole TCP packets only: lwIP has already split anything larger than the MTU into fragments.
    if ((versionAndLength >> 4) == 4 && protocol == IP_PROTO_TCP && headerLength >= IP_HLEN && p->len >= headerLength &&
        (offset & (IP_MF | IP_OFFMASK)) == 0 && (offset & IP_DF) == 0) {
      offsetBe = lwip_htons(static_cast<uint16_t>(offset | IP_DF));
      memcpy(hdr + offsetof(struct ip_hdr, _offset), &offsetBe, sizeof(offsetBe));
      uint16_t checksum = 0;
      memcpy(hdr + offsetof(struct ip_hdr, _chksum), &checksum, sizeof(checksum));
      checksum = inet_chksum(hdr, headerLength);  // already in network byte order
      memcpy(hdr + offsetof(struct ip_hdr, _chksum), &checksum, sizeof(checksum));
    }
  }
  return s_originalOutput(netif, p, ipaddr);
}

// Runs in the lwIP tcpip task, which is also where esp_netif adds and removes the station netif: the
// swap cannot race an output in progress, and resolving the netif here (not in the caller) means a
// station torn down between the request and this call is simply not found rather than dereferenced.
void installHook(void* /*unused*/) {
  esp_netif_t* espNetif = WiFi.STA.netif();
  auto* netif = espNetif ? static_cast<struct netif*>(esp_netif_get_netif_impl(espNetif)) : nullptr;
  if (!netif || !netif->output) {
    LOG_ERR("WIFI", "STA netif unavailable, TCP Don't-Fragment not applied");
    return;
  }
  if (netif->output == outputWithDontFragment) return;
  s_originalOutput = netif->output;
  netif->output = outputWithDontFragment;
  LOG_INF("WIFI", "TCP Don't-Fragment enabled on STA netif");
}
}  // namespace

void applyTcpDontFragment() {
  if (tcpip_callback(installHook, nullptr) != ERR_OK) {
    LOG_ERR("WIFI", "tcpip_callback failed, TCP Don't-Fragment not applied");
  }
}
