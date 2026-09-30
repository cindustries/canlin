/* CanLin configuration for the vendored Mbed TLS 3.6 (third_party/mbedtls, read-only).
 * Selected with -DMBEDTLS_CONFIG_FILE="canlin_mbedtls_config.h" (mk/tls.mk); it replaces
 * include/mbedtls/mbedtls_config.h completely. HTTP transport spec §5.
 *
 * One configuration for every build: host tests compile exactly the code TARGET=i686
 * runs. Deliberately absent: MBEDTLS_AESNI_C, MBEDTLS_PADLOCK_C, MBEDTLS_AESCE_C,
 * MBEDTLS_HAVE_SSE2 and MBEDTLS_HAVE_ASM (no inline assembly at all, so the bignum and
 * constant-time code is the same portable C on the host and on a P2).
 *
 * TLS 1.2 + 1.3 client; ECDHE over X25519, secp256r1, secp384r1; AES-GCM and
 * ChaCha20-Poly1305; RSA (PKCS#1 v1.5 and PSS) and ECDSA certificates.
 *
 * MBEDTLS_HAVE_TIME_DATE is off on purpose: certificate validity dates are checked by
 * src/net/tls.c against the transport's configured wall clock, so a wrong system date
 * is reported as `tls-clock` and tests can use a fake wall clock (spec §5).
 *
 * MBEDTLS_SSL_SRV_C is on only because the loopback test server (t/net) runs on this
 * same build; the runtime never configures a server endpoint. See the canlin#41 report
 * (follow-up: drop it from runtime images once a runtime binary exists).
 */
#ifndef CANLIN_MBEDTLS_CONFIG_H
#define CANLIN_MBEDTLS_CONFIG_H

/* ---- system ---- */
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_FS_IO               /* trust bundle file */
#define MBEDTLS_DEPRECATED_REMOVED
#define MBEDTLS_ENTROPY_C           /* platform entropy: getrandom() / /dev/urandom */
/* Mbed TLS calls getrandom() only when built against glibc; on bionic and musl it reads
 * this file, and its default "/dev/random" blocks on kernels before 5.6 whenever the
 * entropy estimate is low (seen on the Android 4.4 tablet: kernel 3.4, TLS stalled).
 * The DRBG is seeded on the owner thread, so the fallback must never block. */
#define MBEDTLS_PLATFORM_DEV_RANDOM "/dev/urandom"
#define MBEDTLS_CTR_DRBG_C

/* ---- PSA (TLS 1.3 key schedule) ---- */
#define MBEDTLS_PSA_CRYPTO_C

/* ---- bignum, public key ---- */
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_OID_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED

/* ---- symmetric, hashes ---- */
#define MBEDTLS_CIPHER_C
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
#define MBEDTLS_CHACHA20_C
#define MBEDTLS_POLY1305_C
#define MBEDTLS_CHACHAPOLY_C
#define MBEDTLS_MD_C
#define MBEDTLS_HKDF_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C

/* ---- X.509 ---- */
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
/* RSA-PSS: required for rsa_pss_rsae_* in the default signature algorithm list, i.e.
 * for any TLS 1.3 server with an RSA certificate; also accepts PSS-signed certificates. */
#define MBEDTLS_X509_RSASSA_PSS_SUPPORT

/* ---- TLS ---- */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_SRV_C /* loopback test server only, see above */
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_PROTO_TLS1_3
#define MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE
#define MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE /* required by TLS 1.3 in 3.6 */
#define MBEDTLS_SSL_IN_CONTENT_LEN  16384
#define MBEDTLS_SSL_OUT_CONTENT_LEN 4096

#endif
