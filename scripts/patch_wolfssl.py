from pathlib import Path

Import("env")


PROJECT_DIR = Path(env.subst("$PROJECT_DIR"))
MARKER = "/* CrossPoint wolfSSL compatibility overrides */"
OVERRIDES = f"""

{MARKER}
#undef NO_DH
#ifndef HAVE_FFDHE_2048
#define HAVE_FFDHE_2048
#endif
/* MEMFIX-PORT: 8192 handles up to RSA-4096 keys (the public-CA maximum,
   ISRG Root X1 included) with half the per-bignum heap of 16384: with
   WOLFSSL_SMALL_STACK each fast-math temp is FP_MAX_BITS/8 * 2 bytes on the
   heap, and TLS cert verification allocates dozens at once. */
#undef FP_MAX_BITS
#define FP_MAX_BITS 8192
/* Verified-TLS support for public CA chains (GitHub OTA):
   - WOLFSSL_ALT_CERT_CHAINS: a served cross-sign intermediate whose issuer is
     not itself in the trust store must not fail the chain when the peer cert
     already validates to a directly-trusted anchor (internal.c documents this
     exact case). Without it, real-hardware verification fails ASN_NO_SIGNER_E
     on today's Sectigo/ISRG cross-signed chains regardless of bundle content.
   - SHA-384/512: Sectigo's ECDSA-P384 intermediates sign with SHA-384; ISRG
     cross-signs use SHA-512-capable paths. platformio.ini's
     -DWOLFSSL_OPTIONS_H satisfies options.h's include guard before the file is
     read, so the defaults that would normally enable these are silently
     skipped -- they must be set here.
   - WOLFSSL_SP_384: single-precision math for P-384, needed to verify the
     ECDSA-P384 signatures on chip without the generic fallback's heap cost. */
#ifndef WOLFSSL_ALT_CERT_CHAINS
#define WOLFSSL_ALT_CERT_CHAINS
#endif
#ifndef WOLFSSL_SHA384
#define WOLFSSL_SHA384
#endif
#ifndef WOLFSSL_SHA512
#define WOLFSSL_SHA512
#endif
#ifndef WOLFSSL_SP_384
#define WOLFSSL_SP_384
#endif
"""



def patch_user_settings(path: Path) -> None:
    text = path.read_text()
    if MARKER in text:
        text = text.split(MARKER, 1)[0].rstrip()
    path.write_text(text + OVERRIDES + "\n")
    print(f"Patched wolfSSL settings: {path.relative_to(PROJECT_DIR)}")


for settings in PROJECT_DIR.glob(".pio/libdeps/*/Arduino-wolfSSL/src/user_settings.h"):
    patch_user_settings(settings)
