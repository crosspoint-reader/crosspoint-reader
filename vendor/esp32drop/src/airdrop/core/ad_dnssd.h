/* ad_dnssd.h -- mDNS / DNS-SD codec.
 *
 * First piece: the Known-Answer decision. RFC 6762 section 7.1 says a responder MUST NOT
 * answer a question whose answer is already in the query's Known-Answer list, provided
 * that known answer still has at least half the responder's TTL. We have never honoured
 * it, and it is not a nicety here -- a reply is 1,107 bytes built and injected from the
 * frame task, measured at 3,400-7,379 us against a 319 us baseline, up to 45% of one AW.
 *
 * Measured over 101 real captured queries: 23 of them (23%) carry every record we would
 * send, so under 7.1 we should have stayed silent and instead sent the lot. A further 62
 * (61%) ask about nothing of ours at all and are answered only because the firmware's
 * filter is a raw "_airdrop" substring test -- that is a SEPARATE defect with its own
 * build, and this file deliberately does not act on it: when nothing of ours is asked,
 * the answer here is "not suppressed", leaving today's behaviour exactly as it is.
 *
 * WHY THIS IS DANGEROUS CODE, and how it is guarded. Every failure mode of a suppression
 * bug points the same way: the badge quietly stops answering and quietly stops being
 * discoverable. This project has had that failure twice and both times a human noticed
 * before any instrument did. So the rule here is deliberately conservative -- suppress
 * only when EVERY record we would send is provably already held, never on a partial
 * match, never on a parse we could not complete, and never for a question type we
 * cannot reason about. Anything unrecognised returns false and we reply as before.
 *
 * Dependency-free integer C: it compiles unchanged into the firmware and into
 * tools/test-dnssd.sh, where the fixtures are real queries captured off the air.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The port sharingd dials, and the port advertised in the SRV record and in the AWDL
   action frame. Not configurable: AirDrop senders do not read it from anywhere we could
   change, so a setter would only be a way to become undiscoverable. */
static const uint16_t AIRDROP_PORT = 8770;

/* Wire types we reason about. */
#define AD_DNS_T_PTR 12
#define AD_DNS_T_TXT 16
#define AD_DNS_T_AAAA 28
#define AD_DNS_T_SRV 33
#define AD_DNS_T_ANY 255

/* The TTL our PTR records carry (build_mdns_msg writes 0x1194). 7.1's condition is that
   the known answer's remaining TTL is at least half of ours. */
#define AD_DNSSD_PTR_TTL 4500

/* A name may not chase more than this many compression pointers. A malformed message
   can point a name at itself; without a cap that is an infinite loop on the frame task. */
#define AD_DNS_MAX_JUMPS 16

static int ad_ci(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* Advance past the name at `p`, returning the offset of the byte after it, or -1.
   Does NOT follow pointers -- a compressed name ends at its pointer, which is exactly
   what a caller stepping through a message wants. */
static int ad_dns_name_skip(const uint8_t* m, int mlen, int p) {
  while (p >= 0 && p < mlen) {
    uint8_t l = m[p];
    if (l == 0) return p + 1;
    if ((l & 0xc0) == 0xc0) return (p + 2 <= mlen) ? p + 2 : -1;
    if (l & 0xc0) return -1; /* reserved label type */
    p += 1 + l;
  }
  return -1;
}

/* Compare the wire name at `p` against "<first>.<rest>", following pointers.
   `first` may be NULL to compare against `rest` alone. Case-insensitive, as DNS
   requires. Returns false on any malformation rather than guessing. */
static bool ad_dns_name_eq2(const uint8_t* m, int mlen, int p, const char* first, const char* rest) {
  const char* want = first ? first : rest;
  bool on_first = (first != NULL);
  int jumps = 0;
  for (;;) {
    if (p < 0 || p >= mlen) return false;
    uint8_t l = m[p];
    if (l == 0) return (*want == 0) && !on_first; /* both must end together */
    if ((l & 0xc0) == 0xc0) {
      if (p + 2 > mlen || ++jumps > AD_DNS_MAX_JUMPS) return false;
      int t = ((int)(l & 0x3f) << 8) | m[p + 1];
      if (t >= p) return false; /* only backward jumps: no loops */
      p = t;
      continue;
    }
    if (l & 0xc0) return false;
    if (p + 1 + l > mlen) return false;
    /* match this label against the head of `want` */
    for (int i = 0; i < l; i++) {
      if (want[i] == 0 || want[i] == '.') return false;
      if (ad_ci(m[p + 1 + i]) != ad_ci((unsigned char)want[i])) return false;
    }
    want += l;
    if (on_first) {
      if (*want != 0) return false; /* `first` is exactly one label */
      want = rest;
      on_first = false;
    } else {
      if (*want == '.')
        want++;
      else if (*want != 0)
        return false;
    }
    p += 1 + l;
  }
}
static bool ad_dns_name_eq(const uint8_t* m, int mlen, int p, const char* dotted) {
  return ad_dns_name_eq2(m, mlen, p, NULL, dotted);
}

/* Can this name be resolved at all? Same guards as the comparison above.
   The distinction matters: ad_dns_name_eq2 returns false both for "this is a different
   name" and for "this name is malformed", and a caller deciding whether a query concerns
   it must treat those two oppositely. Without this, a query whose names do not parse
   looks exactly like a query about somebody else. */
static bool ad_dns_name_ok(const uint8_t* m, int mlen, int p) {
  int jumps = 0;
  for (;;) {
    if (p < 0 || p >= mlen) return false;
    uint8_t l = m[p];
    if (l == 0) return true;
    if ((l & 0xc0) == 0xc0) {
      if (p + 2 > mlen || ++jumps > AD_DNS_MAX_JUMPS) return false;
      int t = ((int)(l & 0x3f) << 8) | m[p + 1];
      if (t >= p) return false;
      p = t;
      continue;
    }
    if (l & 0xc0) return false;
    if (p + 1 + l > mlen) return false;
    p += 1 + l;
  }
}

/* ------------------------------------------------------------------------- */

/* What we advertise. Both strings are exactly the ones the responder puts on the wire:
   `instance` is the 12-hex service instance, `device_name` the DNM the two pairing
   services use. If these ever disagree with the encoder, suppression decides against a
   record we do not actually send -- so they must come from the same place. */
struct AdDnssdOwn {
  const char* instance;
  const char* device_name;
};

/* The four service names we answer a PTR browse for, paired with which of the two
   identity strings forms the first label of our answer. NULL means the answer's rdata
   is a bare service name rather than "<label>.<service>". */
struct AdDnssdPtr {
  const char* service;
  int label;
}; /* label: 0=instance 1=dnm 2=none */
static const struct AdDnssdPtr AD_DNSSD_PTRS[] = {
    {"_airdrop._tcp.local", 0},
    {"_applicationServicePairing._tcp.local", 1},
    {"_appSvcPrePair._tcp.local", 1},
    {"_services._dns-sd._udp.local", 2}, /* answer rdata is _airdrop._tcp.local */
};
#define AD_DNSSD_NPTR (int)(sizeof(AD_DNSSD_PTRS) / sizeof(AD_DNSSD_PTRS[0]))

/* True when RFC 6762 7.1 forbids answering this query: every record we would send is
   already in its Known-Answer list with at least half our TTL left.
   FALSE in every other case, including "we hold nothing this query asks for" and
   including any message we cannot fully parse. Never suppress on doubt. */
static bool ad_dnssd_known_answer_suppresses(const uint8_t* m, int mlen, const struct AdDnssdOwn* own) {
  if (mlen < 12 || !own || !own->instance || !own->device_name) return false;
  if (m[2] & 0x80) return false; /* a response, not a query */
  int qd = (m[4] << 8) | m[5];
  int an = (m[6] << 8) | m[7];
  if (qd <= 0 || an <= 0) return false; /* no questions, or nothing known */

  /* Pass 1: record where the questions are, and skip to the answer section. */
  int q_off[8];
  uint16_t q_type[8];
  int nq = 0;
  int p = 12;
  for (int i = 0; i < qd; i++) {
    int name = p;
    p = ad_dns_name_skip(m, mlen, p);
    if (p < 0 || p + 4 > mlen) return false;
    if (nq < 8) {
      q_off[nq] = name;
      q_type[nq] = (uint16_t)((m[p] << 8) | m[p + 1]);
      nq++;
    }
    p += 4;
  }
  if (nq != qd) return false; /* more questions than we track */
  const int an_start = p;

  int answerable = 0, suppressed = 0;
  for (int i = 0; i < nq; i++) {
    uint16_t t = q_type[i];

    /* Questions about our own instance -- SRV, TXT, AAAA -- are answered by records
       that a browse's Known-Answer list (which carries PTRs) cannot contain. Treat any
       such question as an unconditional reason to reply. */
    if (t == AD_DNS_T_SRV || t == AD_DNS_T_TXT || t == AD_DNS_T_AAAA || t == AD_DNS_T_ANY) {
      if (ad_dns_name_eq2(m, mlen, q_off[i], own->instance, "_airdrop._tcp.local") ||
          ad_dns_name_eq2(m, mlen, q_off[i], own->instance, "local"))
        return false;
    }
    if (t != AD_DNS_T_PTR && t != AD_DNS_T_ANY) continue;

    for (int k = 0; k < AD_DNSSD_NPTR; k++) {
      if (!ad_dns_name_eq(m, mlen, q_off[i], AD_DNSSD_PTRS[k].service)) continue;
      answerable++;
      /* Is OUR answer to this question already in the Known-Answer list? */
      const char* lab = (AD_DNSSD_PTRS[k].label == 0)   ? own->instance
                        : (AD_DNSSD_PTRS[k].label == 1) ? own->device_name
                                                        : NULL;
      const char* rest = (AD_DNSSD_PTRS[k].label == 2) ? "_airdrop._tcp.local" : AD_DNSSD_PTRS[k].service;
      int a = an_start;
      for (int j = 0; j < an; j++) {
        int aname = a;
        a = ad_dns_name_skip(m, mlen, a);
        if (a < 0 || a + 10 > mlen) return false; /* malformed: do not suppress */
        uint16_t at = (uint16_t)((m[a] << 8) | m[a + 1]);
        uint32_t ttl =
            ((uint32_t)m[a + 4] << 24) | ((uint32_t)m[a + 5] << 16) | ((uint32_t)m[a + 6] << 8) | (uint32_t)m[a + 7];
        uint16_t rdlen = (uint16_t)((m[a + 8] << 8) | m[a + 9]);
        int rdata = a + 10;
        if (rdata + rdlen > mlen) return false;
        a = rdata + rdlen;
        if (at != AD_DNS_T_PTR) continue;
        if (!ad_dns_name_eq(m, mlen, aname, AD_DNSSD_PTRS[k].service)) continue;
        /* 7.1: only suppress while the known answer still has half our TTL. */
        if (ttl * 2u < (uint32_t)AD_DNSSD_PTR_TTL) continue;
        if (ad_dns_name_eq2(m, mlen, rdata, lab, rest)) {
          suppressed++;
          break;
        }
      }
      break; /* one service per question */
    }
  }
  return answerable > 0 && suppressed == answerable;
}

/* ------------------------------------------------------------------------- */
/* Is this query asking for anything we hold?
 *
 * The firmware's filter is a raw substring test -- `contains_airdrop(dns) ||
 * mem_contains(dns, instance)` -- so a query in which some OTHER device resolves its own
 * `<hex>._airdrop._tcp.local` matches, and we answer it with our full 1,107-byte
 * response. Measured over 101 captured queries: 62 of them (61%) ask for nothing of ours
 * and are answered anyway. That is the largest single source of the reply traffic whose
 * cost was measured at 3,400-7,379us of frame-path time per send.
 *
 * Same discipline as the suppression decision above, and for the same reason: getting
 * this wrong stops us answering a query we should answer, and the badge goes quietly
 * undiscoverable. So every uncertain path returns TRUE and we reply exactly as the old
 * filter did -- a message too short to hold a header, a name that will not parse, a
 * question count we cannot walk. Only a query we fully parsed, and in which not one
 * question names a record we hold, returns false.
 */
static bool ad_dnssd_asks_us(const uint8_t* m, int mlen, const struct AdDnssdOwn* own) {
  if (mlen < 12 || !own || !own->instance || !own->device_name) return true;
  if (m[2] & 0x80) return true; /* a response, not a query */
  int qd = (m[4] << 8) | m[5];
  if (qd <= 0) return true;
  int p = 12;
  for (int i = 0; i < qd; i++) {
    int name = p;
    p = ad_dns_name_skip(m, mlen, p);
    if (p < 0 || p + 4 > mlen) return true; /* malformed: answer as before */
    uint16_t t = (uint16_t)((m[p] << 8) | m[p + 1]);
    p += 4;

    /* A name we cannot resolve is not a name we can decide about. Falling through would
       make it indistinguishable from a question about somebody else, which is the unsafe
       direction: we would go silent on a peer's malformed packet. */
    if (!ad_dns_name_ok(m, mlen, name)) return true;

    /* A browse for one of the services we advertise. */
    if (t == AD_DNS_T_PTR || t == AD_DNS_T_ANY)
      for (int k = 0; k < AD_DNSSD_NPTR; k++)
        if (ad_dns_name_eq(m, mlen, name, AD_DNSSD_PTRS[k].service)) return true;

    /* Our own names, at any type. A question for a type we lack is still ours to
       answer: the response carries NSEC records saying so, and without them iOS
       re-queries forever (which is why they are in the message at all). */
    if (ad_dns_name_eq2(m, mlen, name, own->instance, "_airdrop._tcp.local")) return true;
    if (ad_dns_name_eq2(m, mlen, name, own->instance, "local")) return true;
    if (ad_dns_name_eq2(m, mlen, name, own->device_name, "_applicationServicePairing._tcp.local")) return true;
    if (ad_dns_name_eq2(m, mlen, name, own->device_name, "_appSvcPrePair._tcp.local")) return true;
  }
  return false;
}
