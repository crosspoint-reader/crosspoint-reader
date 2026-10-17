"""
PlatformIO pre-build hook: inject WitchReader's trimmed wolfSSL configuration.

The wolfSSL Arduino library ships a user_settings.h tuned for a broad default.
For this firmware we want a lean CLIENT-ONLY TLS 1.3/1.2 profile to save flash
(the app partition is ~95% full) and stack. Rather than edit the vendored file
in place (it lives under .pio/libdeps and is re-fetched on clean builds), we
append our overrides under a unique marker, idempotently.

Runs as a PlatformIO pre: hook and standalone (`python scripts/patch_wolfssl.py`).
No-op when the wolfSSL libdep is absent (flag-off / non-network envs), so it is
safe to register globally.
"""

import glob
import os

try:
    Import("env")  # noqa: F821 -- provided by PlatformIO when run as a build hook
    PROJECT_DIR = env["PROJECT_DIR"]  # noqa: F821
except (NameError, Exception):  # not under PlatformIO -> standalone invocation
    PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

MARKER = "/* ---- CrossPoint wolfSSL client-only overrides ---- */"
END_MARKER = "/* ---- end CrossPoint wolfSSL overrides ---- */"

# Lean client-only profile. Enables TLS 1.3 + 1.2, ECDHE (X25519/P-256), the AEAD
# suites our hosts use (AES-GCM + ChaCha20-Poly1305), RSA-PSS verify, SNI, HKDF.
# Disables the server side, static DH/FFDHE, legacy ciphers, filesystem, OCSP/CRL.
OVERRIDES = """

{marker}
#ifndef CROSSPOINT_WOLFSSL_TUNED
#define CROSSPOINT_WOLFSSL_TUNED

/* Optimize buffer sizing for a small embedded client (wolfSSL asks for this). */
#ifndef WOLFSSL_CLIENT_EXAMPLE
#define WOLFSSL_CLIENT_EXAMPLE
#endif

/* TLS versions: 1.3 preferred, 1.2 fallback; drop everything older. */
#undef  WOLFSSL_TLS13
#define WOLFSSL_TLS13
#undef  NO_OLD_TLS
#define NO_OLD_TLS            /* removes TLS 1.0/1.1 + SSLv3 */

/* Client only. */
#ifndef NO_WOLFSSL_SERVER
#define NO_WOLFSSL_SERVER
#endif

/* Key exchange: ECDHE only. Drop static/ephemeral finite-field DH entirely. */
#ifndef NO_DH
#define NO_DH
#endif
#undef  HAVE_FFDHE_2048
#undef  HAVE_FFDHE_3072
#undef  HAVE_FFDHE_4096

/* Curves + KDF needed for TLS 1.3 ECDHE. */
#ifndef HAVE_ECC
#define HAVE_ECC
#endif
#ifndef HAVE_CURVE25519
#define HAVE_CURVE25519
#endif
#ifndef HAVE_SUPPORTED_CURVES
#define HAVE_SUPPORTED_CURVES
#endif
#ifndef HAVE_TLS_EXTENSIONS
#define HAVE_TLS_EXTENSIONS
#endif
#ifndef HAVE_HKDF
#define HAVE_HKDF
#endif
#ifndef HAVE_SNI
#define HAVE_SNI
#endif

/* Hashes: SHA-256 always; SHA-384/512 are REQUIRED by TLS 1.3 (AES-256-GCM-SHA384
 * suite + P-384 key schedule) even when the leaf cert is SHA-256. Omitting them
 * makes the 1.3 handshake fail with HASH_TYPE_E (-232). */
#ifndef WOLFSSL_SHA384
#define WOLFSSL_SHA384
#endif
#ifndef WOLFSSL_SHA512
#define WOLFSSL_SHA512
#endif

/* AEAD ciphers our hosts negotiate. */
#ifndef HAVE_AESGCM
#define HAVE_AESGCM
#endif
#ifndef HAVE_CHACHA
#define HAVE_CHACHA
#endif
#ifndef HAVE_POLY1305
#define HAVE_POLY1305
#endif

/* RSA verify for RSA-chained certs (USERTrust RSA); PSS for TLS 1.3. */
#ifndef WC_RSA_PSS
#define WC_RSA_PSS
#endif
/* RSA public-key operations only: the client verifies RSA (and RSA-PSS) signatures but never signs
 * or decrypts with an RSA key. Compiles out the RSA private paths, in wolfCrypt and in SP math
 * (sp_RsaPrivate_*). The only RSA signer left in a client build is client-certificate
 * authentication (internal.c RsaSign, for CertificateVerify), so that goes too: the reader never
 * presents a client certificate. A server that asks for one still gets the empty Certificate
 * message (tls13.c SendTls13Certificate stays), exactly as before. */
#ifndef WOLFSSL_RSA_PUBLIC_ONLY
#define WOLFSSL_RSA_PUBLIC_ONLY
#endif
#ifndef WOLFSSL_NO_CLIENT_AUTH
#define WOLFSSL_NO_CLIENT_AUTH
#endif

/* Math backend: use SP (single-precision) math instead of TFM fast-math so the
 * SP ECC fast path (P-256/P-384) + WOLFSSL_SP_RISCV32 assembly kick in. Our
 * hosts are all ECDHE/ECDSA, so accelerating ECC is the biggest handshake win.
 * SP-ECC auto-enables only when USE_FAST_MATH is OFF, so undef it here. */
#undef  USE_FAST_MATH
#define WOLFSSL_SP_MATH_ALL      /* SP math for all key sizes (RSA verify too) */
#define WOLFSSL_HAVE_SP_ECC
#define WOLFSSL_SP_384           /* P-384 (Sectigo/GitHub ECDSA chains use it) */
/* P-256 is on by default with SP-ECC; keep RSA via SP too for USERTrust RSA. */
#define WOLFSSL_HAVE_SP_RSA
/* RSA-4096 through SP as well. Without it a 4096-bit public key falls through to the generic
 * sp_int exponentiation, and on the device that failed: ISRG Root X1 (RSA-4096) signing the
 * cross-signed Root YR that raw/objects/release-assets.githubusercontent.com send since 2026
 * failed ConfirmSignature (-155) at depth 2, while OpenSSL accepts the same chain. Why is not
 * established: wolfSSL reports an allocation failure inside the signature math as -155 too
 * (see the verify log in SecureClient.cpp), so it may have been the generic path's larger heap
 * use on the X3 rather than the math. Either way this define is what made the chain pass on the
 * device; dropping it to save its ~3.8 KB risks bringing the failure back. */
#define WOLFSSL_SP_4096

/* Bignum ceiling: sized for RSA-4096 chains (no FFDHE), half of upstream's 16384. */
#undef  FP_MAX_BITS
#define FP_MAX_BITS 8192
/* SP integer max bits (SP-math analogue of FP_MAX_BITS): cover RSA-4096. */
#ifndef SP_INT_BITS
#define SP_INT_BITS 4096
#endif

/* Trim unused primitives / features to reclaim flash. */
#ifndef NO_DSA
#define NO_DSA
#endif
#ifndef NO_RC4
#define NO_RC4
#endif
#ifndef NO_MD4
#define NO_MD4
#endif
#ifndef NO_DES3
#define NO_DES3
#endif
#ifndef NO_PSK
#define NO_PSK
#endif
#ifndef NO_PWDBASED
#define NO_PWDBASED
#endif
#ifndef NO_WOLFSSL_MEMORY
/* keep wolfSSL memory hooks so heap stats work */
#endif

/* No filesystem on device: certs come from a buffer, not files. */
#ifndef NO_FILESYSTEM
#define NO_FILESYSTEM
#endif

/* No session cache. The reader never resumes a TLS session: every connection runs a full
 * handshake and verifies the whole chain, so the cache (and its client index) would only hold
 * copies of sessions nothing reads back. The library's own user_settings.h asks for
 * MICRO_SESSION_CACHE further down; NO_SESSION_CACHE overrides any cache size (ssl_sess.c). */
#undef  SMALL_SESSION_CACHE
#ifndef NO_SESSION_CACHE
#define NO_SESSION_CACHE
#endif

/* RFC 6066 max_fragment_length, so SecureClient can ask servers for 2 KB records instead of 16 KB
 * ones (each of which needs a ~17 KB contiguous receive buffer). The request in SecureClient is
 * ported from Free-Ink/freeink-sdk b72416b (Justin Mitchell); this define is ours. */
#ifndef HAVE_MAX_FRAGMENT
#define HAVE_MAX_FRAGMENT
#endif

/* Instrumentation only, off unless -DCROSSPOINT_TLS_VERIFY_TIMING is in build_flags.
 * wolfSSL issues the verify callback ONLY on a verification error by default, so a
 * successful handshake reports nothing and per-certificate cost cannot be measured. These two
 * make it fire for every certificate in the chain (intermediates first, leaf last), which is
 * what SecureClient's callback times. Semantics are unchanged for us: the callback returns
 * `preverify` untouched on success, which is exactly what wolfSSL does with no callback. */
#ifdef CROSSPOINT_TLS_VERIFY_TIMING
#ifndef WOLFSSL_ALWAYS_VERIFY_CB
#define WOLFSSL_ALWAYS_VERIFY_CB
#endif
#ifndef WOLFSSL_VERIFY_CB_ALL_CERTS
#define WOLFSSL_VERIFY_CB_ALL_CERTS
#endif
#endif

/* Some cross-signed chains (GTS R4 via GlobalSign, USERTrust) verify more
 * reliably when alternate chain building is allowed. */
#ifndef WOLFSSL_ALT_CERT_CHAINS
#define WOLFSSL_ALT_CERT_CHAINS
#endif

/* Seed the DRBG from the hardware RNG. The library's user_settings.h undefines WOLFSSL_ESPIDF,
 * so wc_GenerateSeed() would take its ARDUINO branch and read libc random(), which nothing here
 * seeds: every boot, and with the reboot after each Wi-Fi session every TLS session, would start
 * from the same DRBG state, making the ECDHE keys predictable. wc_GenerateSeed() checks
 * CUSTOM_RAND_GENERATE_SEED before any platform branch. esp_fill_random() is true random while
 * the radio is on, which it always is when TLS runs.
 * Ported from crosspoint-reader PR #3897 (Rory Chatterton, @rorychatterton). */
#include <esp_random.h>
static inline int crosspoint_wolfssl_seed(unsigned char* output, unsigned int sz) {{
  esp_fill_random(output, sz);
  return 0;
}}
#undef  CUSTOM_RAND_GENERATE_SEED
#define CUSTOM_RAND_GENERATE_SEED crosspoint_wolfssl_seed

#endif /* CROSSPOINT_WOLFSSL_TUNED */
{end_marker}
"""

TAIL_MARKER = "/* ---- CrossPoint wolfSSL tail overrides ---- */"
TAIL_END_MARKER = "/* ---- end CrossPoint wolfSSL tail overrides ---- */"

# Appended, not prepended: these undo defines the library's user_settings.h makes
# unconditionally further down, which the prepended block above cannot reach.
TAIL_OVERRIDES = """
{marker}
/* No wolfSSL trace. The library defines DEBUG_WOLFSSL, which compiles the string of every
 * WOLFSSL_ENTER/WOLFSSL_LEAVE/WOLFSSL_MSG call into flash (~1000 of them), yet nothing turns the
 * trace on: SecureClient never calls wolfSSL_Debugging_ON(). Errors still reach our logs through
 * wolfSSL_get_error(), which does not depend on it. Build with -DCROSSPOINT_WOLFSSL_DEBUG to keep
 * the trace (and call wolfSSL_Debugging_ON() to see it).
 * Adapted from CrossInk (uxjulia/CrossInk) commit 9e27d1dd (Julia Nguyen), MIT. */
#ifndef CROSSPOINT_WOLFSSL_DEBUG
#undef  DEBUG_WOLFSSL
#endif
{end_marker}
"""


def strip_block(text, start, end):
    # Remove a prior injection delimited by start/end markers, wherever it sits.
    if start in text and end in text:
        text = text.split(start, 1)[0] + text.split(end, 1)[1]
    return text


def patch(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        text = f.read()
    # Strip any prior injection (idempotent).
    text = strip_block(text, MARKER, END_MARKER)
    text = strip_block(text, TAIL_MARKER, TAIL_END_MARKER)
    # Prepend so our defines (esp. WOLFSSL_CLIENT_EXAMPLE) are seen BEFORE the
    # library's own #if checks near the top of user_settings.h, avoiding a
    # spurious "define WOLFSSL_CLIENT_EXAMPLE" #warning.
    block = OVERRIDES.format(marker=MARKER, end_marker=END_MARKER).lstrip() + "\n"
    tail_block = TAIL_OVERRIDES.format(marker=TAIL_MARKER, end_marker=TAIL_END_MARKER).lstrip()
    text = block + text.strip() + "\n\n" + tail_block
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    print("Patched wolfSSL user_settings: {}".format(os.path.relpath(path, PROJECT_DIR)))


def main():
    patterns = [
        os.path.join(PROJECT_DIR, ".pio", "libdeps", "*", "Arduino-wolfSSL", "src", "user_settings.h"),
        os.path.join(PROJECT_DIR, ".pio", "libdeps", "*", "wolfssl", "src", "user_settings.h"),
    ]
    found = False
    for pat in patterns:
        for path in glob.glob(pat):
            patch(path)
            found = True
    if not found:
        # Normal for flag-off / non-network envs; not an error.
        print("patch_wolfssl: no wolfSSL user_settings.h found (skipping)")


main()
