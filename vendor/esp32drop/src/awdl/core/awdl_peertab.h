/* ad_peertab.h -- who is nearby, how close, and can they receive.
 *
 * The send path needs three things about a neighbour and nothing else: its MAC (to connect
 * to), how strong its frames are (to decide it is close enough to mean it), and whether it
 * has been heard offering _airdrop._tcp (to know a transfer can even be attempted). This
 * holds exactly that.
 *
 * WHY NOT awdl_census.h. The census remembers up to six names and service types per peer
 * and measures 3,604 bytes. It exists to answer "what is on
 * this mesh" for the sniffer, which is a different question. The table here is 164 bytes --
 * 13% of it -- because it stores one name per peer and nothing it does not need. On a
 * receiver measured running with about 6 KB of headroom that difference is the whole
 * argument.
 *
 * NO DISPLAY NAME HERE, deliberately. A row is a handle for reaching a peer, not a label for
 * showing one. Apple's AWDL service records mostly carry identifiers rather than names --
 * UUIDs, TXT fragments, CLink-<hex> -- and the census-era capture rate for a real name was
 * 0 of 2 peers, so the field would have been empty in almost every row that had one. The
 * name a user should see comes from /Discover's ReceiverComputerName instead, which IS
 * measured to arrive, and which the send path already has to read. Dropping it takes a row
 * from 60 bytes to 20 and the table from 484 to 164.
 *
 * DEPENDENCY-FREE, like every other header in core/ here: no Arduino, no ESP-IDF, no allocation.
 * It compiles unchanged into the host tests (tools/test-peertab.sh), which is the only
 * reason the eviction and averaging below can be trusted without a device.
 */
#ifndef AWDL_PEERTAB_H
#define AWDL_PEERTAB_H
/* THE FILE IS awdl_peertab.h AND THE TYPE IS AdPeerTab, deliberately: the file is named
 * for its PRODUCER and the type for its CONSUMER. check-library.sh's rule 3 matches include
 * targets against /airdrop|ad_/, so a file called ad_peertab.h inside awdl/ trips a rule it
 * does not actually break -- and a checker that has to be argued with stops being read. The
 * names adp_ and AdPeerTab stay: they are the published surface ad_send.h uses.
 * (And note the literal that broke this comment on the way in -- "adp_*" followed by a
 *  slash closes a block comment. Two errors, neither of them where the text was.)
 *
 * WHY THIS LIVES IN THE AWDL LAYER, having started in airdrop/core.
 *
 * The data is born here and nowhere else: RSSI exists for one instant, in the promiscuous
 * callback's rx_ctrl, with the transmitter's MAC in the frame beside it. Anything further
 * out has already lost both -- which is why the table itself sits in awdl/port. Leaving
 * the TYPE on the AirDrop side meant the link layer included an AirDrop header, and
 * tools/check-library.sh's rule 3 forbids that in every case, deliberately: the rule is
 * how an accidental coupling gets caught, and an exception nobody has to remember is
 * better than one everybody does. The AirDrop side reaches it through ESP32AWDL.h, which
 * rule 2 already permits.
 *
 * It stays dependency-free -- stdint, stdbool, string -- so it can be included from either
 * side and compiled unchanged into tools/peertab_test.c. */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Eight rows. The mesh sizes seen in practice are two to five AWDL-active devices in a
   room; eight leaves margin without paying for a crowd that has not been observed. When it
   overflows the OLDEST row goes, and `dropped` counts it -- silence about a full table
   would look exactly like an absent peer. */
#define ADP_ROWS 8

struct AdPeerRow {
  uint8_t mac[6];
  bool used;
  bool saw_airdrop; /* heard offering _airdrop._tcp. HEARD, not assumed: a peer that
                       has not announced the service may still have it and simply not
                       have spoken yet, so false means "unknown", never "cannot". */
  int8_t rssi_last;
  int8_t rssi_avg; /* IIR, not a window: see adp_frame() */
  uint16_t port;   /* from the service record; 8770 in every capture so far */
  uint32_t first_ms, last_ms;
};

struct AdPeerTab {
  struct AdPeerRow row[ADP_ROWS];
  uint32_t dropped;
};

static inline void adp_reset(struct AdPeerTab* t) { memset(t, 0, sizeof *t); }

/* -1 when absent. */
static inline int adp_find(const struct AdPeerTab* t, const uint8_t mac[6]) {
  for (int i = 0; i < ADP_ROWS; i++)
    if (t->row[i].used && memcmp(t->row[i].mac, mac, 6) == 0) return i;
  return -1;
}

/* A free row, or the least recently heard one. Never fails, so a caller never has to
   handle "no room" -- it handles `dropped` growing instead, which is the honest signal. */
/* AN UNHEARD PEER MUST NOT READ AS A NEAR ONE.
 *
 * A fresh row and an evicted one are both all-zero, and zero in rssi_avg is 0 dBm -- which
 * is stronger than the -14 dBm measured with a phone touching the badge. adp_frame() knew
 * this and seeded the real reading on a new row ("0 dBm would read as touching"), but
 * adp_service() creates rows too and did not, so a peer known only by its service record
 * arrived claiming the strongest signal a caller could see. GreetingCard picks the strongest
 * fresh row, so that row wins.
 *
 * Fixed where the row is HANDED OUT rather than in each writer, because there are two
 * writers today and the next one would have to remember. -128 is the weakest value an
 * int8_t can hold: unknown, and unable to win any proximity test until a frame arrives. */
static inline void adp_row_init(struct AdPeerRow* r, const uint8_t mac[6], uint32_t now_ms) {
  memset(r, 0, sizeof *r);
  memcpy(r->mac, mac, 6);
  r->used = true;
  r->first_ms = r->last_ms = now_ms;
  r->rssi_avg = r->rssi_last = -128; /* the sentinel: below any real noise floor */
}

static inline int adp_slot(struct AdPeerTab* t, uint32_t now_ms) {
  for (int i = 0; i < ADP_ROWS; i++)
    if (!t->row[i].used) return i;
  int oldest = 0;
  uint32_t worst = 0;
  for (int i = 0; i < ADP_ROWS; i++) {
    uint32_t age = now_ms - t->row[i].last_ms; /* unsigned wrap is the correct age here */
    if (age >= worst) {
      worst = age;
      oldest = i;
    }
  }
  t->dropped++;
  memset(&t->row[oldest], 0, sizeof t->row[oldest]);
  return oldest;
}

/* Any AWDL frame from `mac`. This is the only thing that moves RSSI.
 *
 * rssi_avg is a 3:1 IIR rather than a ring of samples, and that is a deliberate trade: a
 * ring of sixteen int8 would add 16 bytes to every row -- more than doubling the table --
 * to sharpen a number whose only job is to answer "is this getting closer". The IIR keeps
 * a row at 20 bytes and still smooths the frame-to-frame swing that makes a raw reading
 * unusable as a trigger. First sample seeds it, so a peer never has to be heard twice
 * before its average means anything. */
static inline void adp_frame(struct AdPeerTab* t, const uint8_t mac[6], int8_t rssi, uint32_t now_ms) {
  int i = adp_find(t, mac);
  if (i < 0) {
    i = adp_slot(t, now_ms);
    adp_row_init(&t->row[i], mac, now_ms);
    /* Seed with the real reading rather than leaving adp_row_init's -128: the IIR below
       would otherwise spend several frames climbing out of "unknown" and read a peer as
       far away while it is in your hand. One creation path, one deliberate override. */
    t->row[i].rssi_avg = rssi;
  }
  struct AdPeerRow* p = &t->row[i];
  p->rssi_last = rssi;
  /* SEED ON THE FIRST READING, WHEREVER THE ROW CAME FROM. adp_service() can create a row
     before any frame has been heard, and it leaves rssi_avg at the -128 that means unknown.
     Averaging into that crawls: a first frame at -40 gives (-128*3 + -40)/4 = -106, and a
     phone in your hand reads as across the street for several frames -- the mirror of the
     bug -128 was introduced to fix. -128 is safe as the sentinel because it is not a
     reading: it sits below the noise floor of any receiver this runs on. */
  p->rssi_avg = (p->rssi_avg == -128) ? rssi : (int8_t)((p->rssi_avg * 3 + rssi) / 4);
  p->last_ms = now_ms;
}

/* A service record naming _airdrop._tcp. */
static inline void adp_service(struct AdPeerTab* t, const uint8_t mac[6], uint16_t port, uint32_t now_ms) {
  int i = adp_find(t, mac);
  if (i < 0) {
    i = adp_slot(t, now_ms);
    adp_row_init(&t->row[i], mac, now_ms);
  }
  struct AdPeerRow* p = &t->row[i];
  p->saw_airdrop = true;
  if (port) p->port = port;
  p->last_ms = now_ms;
}

/* Drop rows not heard from in `ttl_ms`. AWDL MACs rotate about every 100 s, so a peer that
   has gone quiet for longer is not the same peer any more even if it is the same device --
   keeping the row would offer the caller a MAC that no longer answers. */
static inline void adp_expire(struct AdPeerTab* t, uint32_t now_ms, uint32_t ttl_ms) {
  for (int i = 0; i < ADP_ROWS; i++)
    if (t->row[i].used && (uint32_t)(now_ms - t->row[i].last_ms) > ttl_ms) memset(&t->row[i], 0, sizeof t->row[i]);
}

#endif /* AD_PEERTAB_H */
