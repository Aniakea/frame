#ifndef MPB_TEST_SHA256_REF_H
#define MPB_TEST_SHA256_REF_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Reference FIPS 180-4 SHA-256 for host tests only. Correctness is
 * self-checked against the known-answer digests that the corpus generator
 * (python oracle) records for every positive fixture. */
void sha256_ref(const uint8_t* data, size_t length, uint8_t digest_out[32]);

#ifdef __cplusplus
}
#endif

#endif
