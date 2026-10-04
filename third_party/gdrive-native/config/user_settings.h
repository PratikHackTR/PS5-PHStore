#ifndef GDRIVE_PROBE_WOLFSSL_SETTINGS_H
#define GDRIVE_PROBE_WOLFSSL_SETTINGS_H
#define WOLFSSL_PTHREADS
#define WOLFSSL_TLS_READ_AHEAD
#define WOLFSSL_SP_MATH_ALL
#define WOLFSSL_SP_SMALL
#define WOLFSSL_TLS13
#define HAVE_TLS_EXTENSIONS
#define HAVE_SUPPORTED_CURVES
#define HAVE_SNI
#define HAVE_ECC
#define ECC_TIMING_RESISTANT
#define TFM_TIMING_RESISTANT
#define HAVE_AESGCM
#define HAVE_HKDF
#define HAVE_CHACHA
#define HAVE_POLY1305
#define WC_RSA_PSS
#define WOLFSSL_SHA384
#define WOLFSSL_SHA512
#define OPENSSL_EXTRA
#define OPENSSL_ALL
#define HAVE_EX_DATA
#define NO_DSA
#define NO_DH
#define NO_PSK
#define NO_RC4
#define NO_DES3
#define NO_OLD_TLS
#define NO_WOLFSSL_SERVER
#define WOLFSSL_AESNI
#define WOLFSSL_X86_64_BUILD
#define NO_AVX2_SUPPORT
#define NO_VAES_SUPPORT
#define NO_AVX512_SUPPORT
#define PH_CRYPTO_SUITE

#define NO_DEV_RANDOM
#ifndef __ASSEMBLER__
int probe_entropy(unsigned char *output, unsigned int size);
#endif
#define CUSTOM_RAND_GENERATE_SEED probe_entropy
#define HAVE_HASHDRBG
#endif

#define WC_RSA_BLINDING

#define WOLFSSL_ALT_CERT_CHAINS
