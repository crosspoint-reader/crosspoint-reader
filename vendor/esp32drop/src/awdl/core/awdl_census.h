// Neighbour census: who else is on this AWDL mesh, how strong, how stable, and
// -- the part that survives MAC rotation -- what they call themselves.
//
// Why this exists. The stability of our transmit window is decided by peers we do
// not control: a neighbour that re-bases its TSF, or whose phase goes noisy, churns
// the gap-based master election and (measured) drops us out of the Mac's neighbour
// table for over a minute. To reason about that we must be able to say "the noisy
// one is THAT device", not "the noisy one is a2b3c4d5e6f7" -- especially since Apple
// rotates AWDL MACs, so a MAC is a handle with an expiry date.
//
// Identity that outlives the MAC comes from mDNS. AWDL data frames carry mDNS in
// clear, and Apple devices announce services on it. NOTE, measured from real
// captures in this repo: the `_airdrop._tcp` instance label is an OPAQUE 12-hex-digit
// id (e.g. "fdba584fc267"), NOT a device name -- so AirDrop records alone cannot
// name anybody. The device name lives elsewhere: in A/AAAA hostnames
// ("Miros-MacBook-Pro.local") and in instance labels of other services
// (_companion-link._tcp, _rdlink._tcp, _airplay._tcp ...). That is why the census
// looks at ALL mDNS, not just the AirDrop-relevant subset the receiver filters to.
//
// Dependency-free integer C over plain buffers, like awdl_http.h / awdl_serve.h, so
// the same code compiles into the sniffer firmware and into a host test that replays
// real captured mDNS bytes (tools/test-census.sh).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "awdl_lru.h"

#define CEN_PEERS 12     // distinct source MACs tracked
#define CEN_NAMES 6      // names + service types remembered per peer
#define CEN_NAME_LEN 40  // a truncated name still identifies a device
#define CEN_LABEL_MIN 2  // shorter label runs are noise, not names
// Beyond this, a row's remembered facts describe the past, not the mesh right now.
// 10s is comfortably longer than any peer's announcement interval measured here
// (healthy peers announce every 90-500ms) and shorter than the idle periods that
// make a row misleading.
#define CEN_FRESH_MS 10000

struct CenName {
  char s[CEN_NAME_LEN];
  uint8_t kind;  // 1 = host (<name>.local), 2 = service instance, 3 = service TYPE
};

struct CenPeer {
  bool used;
  uint8_t mac[6];
  // Radio. RSSI is the proximity handle: it changes when a device MOVES, which is
  // an identification lever that needs no cooperation from the device (no Wi-Fi
  // toggling), and it keeps working after a MAC rotation.
  int32_t rssi_sum;
  uint32_t rssi_n;
  int8_t rssi_last;
  int8_t rssi_min, rssi_max;
  uint32_t frames;  // every frame we heard from this MAC
  uint32_t mdns;    // mDNS frames specifically
  uint32_t first_ms, last_ms;
  struct CenName name[CEN_NAMES];
  uint8_t n_names;
  // Which master THIS peer says it is synced to. The mesh's shape is not visible
  // from a list of transmitters -- what matters is who follows whom, because that is
  // what the election is trying to agree with. It also answers a question the
  // receiver could not: a master row keyed on OUR address appears every 30-60s, and
  // only the advertiser can tell us who is putting it there.
  uint8_t adv_master[6];
  bool adv_valid;
  uint32_t adv_n;
  // How many times this peer named the WATCHED address as its master. The last
  // advertised master alone cannot answer that: cen_advert overwrites it on every
  // sync frame, so a peer that names the badge in one frame and its real root in the
  // next looks innocent in a 5s snapshot -- which is exactly what happened, with the
  // receiver reporting self-rows while the sniffer showed nobody advertising it.
  uint32_t adv_watch_n;
};

struct Census {
  struct CenPeer peer[CEN_PEERS];
  uint32_t dropped;  // frames from a 13th+ MAC, so the table size is auditable
};

static inline void cen_init(struct Census* c) { memset(c, 0, sizeof(*c)); }

static struct CenPeer* cen_row(struct Census* c, const uint8_t* mac, uint32_t now_ms) {
  int free_i = -1, stalest = 0;
  uint32_t oldest = 0xffffffff;
  for (int i = 0; i < CEN_PEERS; i++) {
    if (c->peer[i].used) {
      if (memcmp(c->peer[i].mac, mac, 6) == 0) return &c->peer[i];
      if (awdl_lru_staler(c->peer[i].last_ms, oldest)) {
        oldest = c->peer[i].last_ms;
        stalest = i;
      }
    } else if (free_i < 0)
      free_i = i;
  }
  int idx = (free_i >= 0) ? free_i : stalest;
  if (free_i < 0) c->dropped++;  // we evicted somebody: say so
  struct CenPeer* p = &c->peer[idx];
  memset(p, 0, sizeof(*p));
  p->used = true;
  memcpy(p->mac, mac, 6);
  p->first_ms = now_ms;
  p->rssi_min = 127;
  p->rssi_max = -127;
  return p;
}

// Record that `src` advertises `master` as its sync master.
static inline void cen_advert(struct Census* c, const uint8_t* src, const uint8_t* master, const uint8_t* watch,
                              uint32_t now_ms) {
  struct CenPeer* p = cen_row(c, src, now_ms);
  memcpy(p->adv_master, master, 6);
  p->adv_valid = true;
  p->adv_n++;
  if (watch && memcmp(master, watch, 6) == 0) p->adv_watch_n++;
}

// Every received frame: keeps the RSSI distribution and liveness per MAC.
static inline void cen_frame(struct Census* c, const uint8_t* mac, int8_t rssi, uint32_t now_ms) {
  struct CenPeer* p = cen_row(c, mac, now_ms);
  p->frames++;
  p->rssi_last = rssi;
  p->rssi_sum += rssi;
  p->rssi_n++;
  if (rssi < p->rssi_min) p->rssi_min = rssi;
  if (rssi > p->rssi_max) p->rssi_max = rssi;
  p->last_ms = now_ms;
}

static bool cen_have_name(struct CenPeer* p, const char* s) {
  for (int i = 0; i < p->n_names; i++)
    if (strcmp(p->name[i].s, s) == 0) return true;
  return false;
}

static void cen_add_name(struct CenPeer* p, const char* s, uint8_t kind) {
  if (!s[0] || cen_have_name(p, s)) return;
  if (p->n_names < CEN_NAMES) {
    strncpy(p->name[p->n_names].s, s, CEN_NAME_LEN - 1);
    p->name[p->n_names].s[CEN_NAME_LEN - 1] = 0;
    p->name[p->n_names].kind = kind;
    p->n_names++;
  }
}

// Is this label an opaque id rather than a name? The AirDrop instance label is 12
// hex digits (measured), and a random hex blob identifies a SESSION, not a device --
// remembering it would make two sightings of the same Mac look like two devices.
static bool cen_all_hex(const char* s, int n) {
  if (n < 8) return false;
  for (int i = 0; i < n; i++) {
    char ch = s[i];
    bool hex = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
    if (!hex) return false;
  }
  return true;
}

// Opaque identifiers that are NOT device names, learned from the first real capture
// this census produced:
//   "22b839e1-686e-437b-b1db-83d554ef9a83"  a UUID -- per-service, not per-device
//   "sn=com"                                a TXT key=value pair, not a name at all
//   "CLink-bde27f44a935"                    a companion-link id with a hex tail
// Keeping any of these would invent a new "device" every time one rotates.
static bool cen_opaque(const char* s, int n) {
  int dash = 0, eq = 0, hexrun = 0, maxhex = 0;
  for (int i = 0; i < n; i++) {
    char ch = s[i];
    if (ch == '=') eq++;
    if (ch == '-') {
      dash++;
      if (hexrun > maxhex) maxhex = hexrun;
      hexrun = 0;
      continue;
    }
    bool hex = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
    if (hex)
      hexrun++;
    else {
      if (hexrun > maxhex) maxhex = hexrun;
      hexrun = 0;
    }
  }
  if (hexrun > maxhex) maxhex = hexrun;
  if (eq) return true;                    // TXT fragment
  if (dash >= 3 && n >= 32) return true;  // UUID shape
  if (maxhex >= 10) return true;          // a long hex run is an id, not a name
  return false;
}

// Walk a DNS name starting at `off`, appending labels into `out` dotted. Returns the
// offset just past the name, or -1 if it is malformed / compressed away.
//
// Compression pointers are NOT followed. A pointer means "the rest of this name was
// already spelled out elsewhere in this packet", and the linear scan below visits
// that spelling on its own, so following pointers would only add a way to loop
// forever on a hostile packet.
static int cen_walk_name(const uint8_t* d, int dl, int off, char* out, int outsz, int* n_labels) {
  int o = 0;
  *n_labels = 0;
  while (off < dl) {
    uint8_t l = d[off];
    if (l == 0) {
      off++;
      break;
    }
    if ((l & 0xc0) == 0xc0) {
      off += 2;
      break;
    }  // pointer: stop, keep what we have
    if (l > 63 || off + 1 + l > dl) return -1;
    if (o && o < outsz - 1) out[o++] = '.';
    for (int i = 0; i < l; i++) {
      uint8_t ch = d[off + 1 + i];
      if (ch < 32 || ch > 126) return -1;  // not a text label
      if (o < outsz - 1) out[o++] = (char)ch;
    }
    (*n_labels)++;
    off += 1 + l;
    if (*n_labels > 8) break;  // absurd depth: stop
  }
  out[o < outsz ? o : outsz - 1] = 0;
  return off;
}

// Harvest device-identifying names from one mDNS payload.
//
// Deliberately a linear scan for label runs rather than a section-by-section parser:
// the goal is identification, not correctness of DNS semantics, and a scan sees names
// in questions, answers, authority and additional records alike without needing to
// track record counts, types, or the rdata layouts they imply.
//
// What is kept:
//   kind 1 (host)    "<something>.local" with exactly two labels -> the device name
//   kind 2 (service) the instance label of "<instance>._service._tcp.local"
// What is dropped: opaque hex instance ids (AirDrop), bare service types with no
// instance, and anything that is not printable text.
static void cen_mdns(struct Census* c, const uint8_t* mac, const uint8_t* dns, int dl, uint32_t now_ms) {
  struct CenPeer* p = cen_row(c, mac, now_ms);
  p->mdns++;
  p->last_ms = now_ms;
  if (dl < 12) return;

  // Names may be harvested from RESPONSES only. In a query the names are what the
  // sender is ASKING ABOUT -- somebody else's. The first real capture showed exactly
  // this failure: our own badge's "M5 Badge" service turned up attached to a
  // neighbour, because that neighbour had queried for it. A census that mixes up
  // "who sent this" with "who this is about" cannot identify anything.
  if (!(dns[2] & 0x80)) return;

  for (int off = 12; off < dl;) {
    uint8_t l = dns[off];
    if (l == 0 || l > 63 || (l & 0xc0) || off + 1 + l > dl) {
      off++;
      continue;
    }
    char nm[128];
    int nlab = 0;
    int end = cen_walk_name(dns, dl, off, nm, sizeof(nm), &nlab);
    if (end < 0 || nlab < 2) {
      off++;
      continue;
    }

    // Must be a .local name to be about a device on this link.
    int len = (int)strlen(nm);
    bool is_local = (len > 6 && strcmp(nm + len - 6, ".local") == 0);
    if (!is_local) {
      off++;
      continue;
    }

    // A name whose FIRST label starts with '_' is a service type, never a device
    // name: the linear scan lands on the tail of every service name it walks past,
    // so without this "_tcp.local" arrives here looking exactly like a two-label
    // hostname. (The host test caught this.)
    //
    // The service TYPE is still worth keeping though -- it says what KIND of device
    // this is even when no name is offered, which is the situation for the peers we
    // most want to identify. _companion-link is any Apple device, _airplay/_raop
    // means a speaker or TV, _touch-able an Apple TV, _homekit an accessory.
    if (nm[0] == '_') {
      const char* d2 = strchr(nm, '.');
      int tl = d2 ? (int)(d2 - nm) : len;
      if (tl >= 3 && tl < CEN_NAME_LEN) {
        char ty[CEN_NAME_LEN];
        memcpy(ty, nm, tl);
        ty[tl] = 0;
        cen_add_name(p, ty, 3);
      }
      off++;
      continue;
    }

    // First label, and whether any label is a service type ("_xxx").
    const char* dot = strchr(nm, '.');
    int fl = dot ? (int)(dot - nm) : len;
    bool service = (strstr(nm, "._tcp.") != 0) || (strstr(nm, "._udp.") != 0);

    if (!service && nlab == 2) {
      if (fl >= CEN_LABEL_MIN && !cen_all_hex(nm, fl) && !cen_opaque(nm, fl)) {
        char host[CEN_NAME_LEN];
        int k = fl < CEN_NAME_LEN - 1 ? fl : CEN_NAME_LEN - 1;
        memcpy(host, nm, k);
        host[k] = 0;
        cen_add_name(p, host, 1);  // "<device>.local" -> device name
      }
    } else if (service && nm[0] != '_') {
      if (fl >= CEN_LABEL_MIN && !cen_all_hex(nm, fl) && !cen_opaque(nm, fl)) {
        char inst[CEN_NAME_LEN];
        int k = fl < CEN_NAME_LEN - 1 ? fl : CEN_NAME_LEN - 1;
        memcpy(inst, nm, k);
        inst[k] = 0;
        cen_add_name(p, inst, 2);  // "<instance>._svc._tcp.local"
      }
    }
    off = end > off ? end : off + 1;
  }
}

// One line per peer, meant to be greppable and merged host-side with the receiver's
// own ESRC/MSTR stability numbers (tools/neighbors.py).
//   CEN <mac> rssi=<last>/<avg>[<min>..<max>] frames=<n> mdns=<n> age=<ms> names=<a|b>
static int cen_format(struct CenPeer* p, uint32_t now_ms, char* out, int outsz) {
  int avg = p->rssi_n ? (int)(p->rssi_sum / (int32_t)p->rssi_n) : 0;
  int n = snprintf(
      out, outsz, "CEN %02x%02x%02x%02x%02x%02x rssi=%d/%d[%d..%d] frames=%lu mdns=%lu age=%lu names=", p->mac[0],
      p->mac[1], p->mac[2], p->mac[3], p->mac[4], p->mac[5], (int)p->rssi_last, avg, (int)p->rssi_min, (int)p->rssi_max,
      (unsigned long)p->frames, (unsigned long)p->mdns, (unsigned long)(now_ms - p->last_ms));
  // Qualify the topology by freshness. A peer's advertised master is remembered
  // until it is overwritten, so a row untouched for minutes still carries whatever
  // it last said -- and read as if it were current it produces a confident, wrong
  // picture of the mesh. (It did: a stale row was read as "the badge is in a
  // different mesh from the Mac" when in truth the Mac had gone dormant five
  // minutes earlier.) Anything older than CEN_FRESH_MS says so instead.
  if (p->adv_valid) {
    if (now_ms - p->last_ms <= CEN_FRESH_MS)
      n += snprintf(out + n, outsz - n, "master=%02x%02x%02x%02x%02x%02x ", p->adv_master[0], p->adv_master[1],
                    p->adv_master[2], p->adv_master[3], p->adv_master[4], p->adv_master[5]);
    else
      n += snprintf(out + n, outsz - n, "master=stale ");
  }
  if (p->adv_watch_n) n += snprintf(out + n, outsz - n, "NAMED-WATCHED=%lu ", (unsigned long)p->adv_watch_n);
  if (!p->n_names) n += snprintf(out + n, outsz - n, "-");
  for (int i = 0; i < p->n_names && n < outsz - 1; i++)
    n += snprintf(out + n, outsz - n, "%s%s%s", i ? "|" : "",
                  p->name[i].kind == 1 ? "" : (p->name[i].kind == 2 ? "@" : ""), p->name[i].s);
  return n;
}
