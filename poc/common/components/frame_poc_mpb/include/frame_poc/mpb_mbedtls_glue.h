#ifndef FRAME_POC_MPB_MBEDTLS_GLUE_H
#define FRAME_POC_MPB_MBEDTLS_GLUE_H

#include <stddef.h>
#include <stdint.h>

#include "frame_poc/frame_abi.h"
#include "frame_poc/mpb_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Target-side crypto injection for mpb_parse() on top of the mbedtls PSA
 * crypto API shipped with ESP-IDF (mbedtls 4.x). Compiled only inside the
 * ESP-IDF build; host tests inject their own callbacks instead. The host
 * parser source itself never includes this header. */

typedef struct {
    uint32_t key_id;
    uint8_t uncompressed_point[65]; /* 0x04 || X || Y, secp256r1 */
} mpb_trusted_key_t;

/* Fill out_crypto with PSA-backed sha256_fn and ecdsa_verify_fn bound to the
 * given trusted keys. psa_crypto_init() must have succeeded before the first
 * mpb_parse() call; this maker returns FRAME_ERR_INVALID_ARGUMENT for null
 * arguments and FRAME_ERR_SIGNATURE_INVALID when a key cannot be imported. */
frame_err_t mpb_mbedtls_crypto_make(const mpb_trusted_key_t* trusted_keys, size_t trusted_key_count,
                                    mpb_crypto_t* out_crypto);

#ifdef __cplusplus
}
#endif

#endif
