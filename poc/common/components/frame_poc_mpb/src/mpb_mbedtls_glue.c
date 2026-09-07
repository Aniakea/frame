#include "frame_poc/mpb_mbedtls_glue.h"

#include "psa/crypto.h"

typedef struct {
    const mpb_trusted_key_t* keys;
    size_t key_count;
} mpb_glue_ctx_t;

static mpb_glue_ctx_t mpb_glue_ctx;

/* PSA ECDSA uses the fixed-length r||s (IEEE P1363) signature format and the
 * uncompressed-point public key format, which matches the container contract
 * (manifest_builder SIGNATURE_RECORD / P1363 r||s) without conversions. */

static void mpb_glue_sha256(void* ctx, const uint8_t* data, size_t length, uint8_t digest_out[32]) {
    (void)ctx;
    size_t produced = 0;
    (void)psa_hash_compute(PSA_ALG_SHA_256, data, length, digest_out, 32, &produced);
}

static const mpb_trusted_key_t* mpb_glue_find_key(uint32_t key_id) {
    for (size_t i = 0; i < mpb_glue_ctx.key_count; ++i) {
        if (mpb_glue_ctx.keys[i].key_id == key_id) {
            return &mpb_glue_ctx.keys[i];
        }
    }
    return NULL;
}

static int mpb_glue_verify(void* ctx, uint32_t key_id, const uint8_t digest[32],
                           const uint8_t signature_p1363[64]) {
    (void)ctx;
    const mpb_trusted_key_t* key = mpb_glue_find_key(key_id);
    if (key == NULL) {
        return 0;
    }
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_VERIFY_HASH);
    psa_set_key_algorithm(&attributes, PSA_ALG_ECDSA_ANY);
    psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attributes, 256);
    psa_key_id_t imported = 0;
    psa_status_t status = psa_import_key(&attributes, key->uncompressed_point,
                                         sizeof(key->uncompressed_point), &imported);
    if (status != PSA_SUCCESS) {
        return 0;
    }
    status = psa_verify_hash(imported, PSA_ALG_ECDSA_ANY, digest, 32, signature_p1363, 64);
    (void)psa_destroy_key(imported);
    return status == PSA_SUCCESS;
}

frame_err_t mpb_mbedtls_crypto_make(const mpb_trusted_key_t* trusted_keys, size_t trusted_key_count,
                                    mpb_crypto_t* out_crypto) {
    if (trusted_keys == NULL || trusted_key_count == 0 || out_crypto == NULL) {
        return FRAME_ERR_INVALID_ARGUMENT;
    }
    for (size_t i = 0; i < trusted_key_count; ++i) {
        if (trusted_keys[i].uncompressed_point[0] != 0x04) {
            return FRAME_ERR_INVALID_ARGUMENT;
        }
    }
    if (psa_crypto_init() != PSA_SUCCESS) {
        return FRAME_ERR_SIGNATURE_INVALID;
    }
    mpb_glue_ctx.keys = trusted_keys;
    mpb_glue_ctx.key_count = trusted_key_count;
    out_crypto->ctx = &mpb_glue_ctx;
    out_crypto->sha256_fn = &mpb_glue_sha256;
    out_crypto->ecdsa_verify_fn = &mpb_glue_verify;
    return FRAME_OK;
}
