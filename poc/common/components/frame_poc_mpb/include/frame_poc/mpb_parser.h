#ifndef FRAME_POC_MPB_PARSER_H
#define FRAME_POC_MPB_PARSER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "frame_poc/frame_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Strict single-type MPB container parser (poc-a task T3).
 *
 * The container layout, the frozen offsets and the verification order are
 * defined by tools/frame_tools/manifest_builder.py (appendix A of
 * _doc/project.md section 9, v4.2 single-type semantics of PLUG-001). That
 * python module is the reference oracle; this parser must agree with it.
 *
 * Layout contract (little-endian, field-by-field reads, no unaligned casts):
 *   fixed_header  92B  magic u32 0x4D504246 ("FBPM") @0 | format_major u16 @4
 *                      format_minor u16 @6 | header_size u32 @8 | total_size
 *                      u32 @12 | flags u32 @16 (bit0 resource-only) |
 *                      target_id u32 @20 | core_abi_major/minor u16 @24/@26 |
 *                      required/optional features u64 @28/@36 |
 *                      manifest_offset u32 @44 | manifest_length u32 @48 |
 *                      payload_table_offset u32 @52 | payload_count u16 @56
 *                      (1..16) | payload_entry_size u16 @58 (=60) |
 *                      signature_offset u32 @60 | signature_length u32 @64
 *                      (=72) | key_id u32 @68 | security_epoch u32 @72 |
 *                      reserved[4] u32 @76 (ignored on read)
 *   manifest      TLV stream: field_id u16 | flags u16 | length u32 | value.
 *                      ids 1..35 are canonical; unknown REQUIRED ids reject,
 *                      unknown OPTIONAL ids are skipped; duplicated known
 *                      ids reject; name(1) and version(2) must be present.
 *   payload_table count * 60B entries: type u16 @0 (ELF=1/RESOURCE=2) |
 *                      flags u16 @2 (REQUIRED=0x1, COMPRESSED=0x2 forbidden
 *                      for ELF) | offset u32 @4 | length u32 @8 |
 *                      unpacked u32 @12 (== length) | alignment u32 @16
 *                      (power of two, 1..4096) | sha256[32] @20 |
 *                      reserved u32[2] @52 (ignored on read)
 *   payloads      live in [table_end, signature_offset); alignment padding
 *                      inside the window is unauthenticated by design and is
 *                      never read by this parser
 *   signature     72B  algorithm u16 (=1) @0 | record_version u16 (=1) @2 |
 *                      key_id u32 @4 (must equal header key_id) | IEEE P1363
 *                      r||s 64B @8. Digest = SHA-256(header || manifest ||
 *                      payload_table) over the contiguous prefix
 *                      [0, table_end) of the staging buffer.
 *
 * Verification order is hardcoded and must not be bypassed (SEC-004):
 *   1. structure/bounds/type checks        -> FRAME_ERR_PACKAGE_INVALID
 *   2. signature (ECDSA P-256 over digest) -> FRAME_ERR_SIGNATURE_INVALID
 *   3. per-payload SHA-256                 -> FRAME_ERR_HASH_MISMATCH
 *   4. epoch/target/ABI/features policy    -> FRAME_ERR_EPOCH_ROLLBACK,
 *      FRAME_ERR_TARGET_MISMATCH, FRAME_ERR_ABI_MISMATCH
 *
 * Hostile-input rules: malformed input is the subject. Every integer is read
 * byte by byte, every region computation is carried out in uint64_t before it
 * is narrowed, reserved fields are ignored, and the parser never touches
 * payload-window padding bytes. The parser performs no allocation and keeps
 * no global state; it never writes to the staging buffer (PLUG-008 immutable
 * staging: the caller owns the buffer and must keep it alive as long as the
 * returned view is used, because view fields point into it).
 *
 * Semantics note: this parser validates container structure, TLV shape,
 * signature, hashes and device policy. Deep manifest value semantics (utf-8
 * validity, strict semver syntax, quota cross-field rules) stay in the python
 * oracle: manifest bytes are inside the signed region, so post-signature
 * tampering is already rejected, and the builder refuses to emit containers
 * that violate the semantic rules.
 */

#define MPB_MAGIC 0x4D504246u          /* bytes 46 42 50 4D little-endian */
#define MPB_TARGET_ESP32S3 0x33505345u /* "ESP3" */
#define MPB_HEADER_SIZE 92u
#define MPB_PAYLOAD_ENTRY_SIZE 60u
#define MPB_SIGNATURE_RECORD_SIZE 72u
#define MPB_TLV_HEADER_SIZE 8u
#define MPB_MAX_PAYLOAD_COUNT 16u
#define MPB_MAX_NAME_BYTES 64u
#define MPB_MAX_VERSION_BYTES 32u
#define MPB_MAX_REQUIRES_ENTRIES 32u
#define MPB_MAX_REQUIRE_BYTES 96u
#define MPB_MAX_ALIGNMENT 4096u

#define MPB_FLAG_RESOURCE_ONLY 0x00000001u

#define MPB_PAYLOAD_TYPE_ELF 1u
#define MPB_PAYLOAD_TYPE_RESOURCE 2u

#define MPB_PAYLOAD_FLAG_REQUIRED 0x0001u
#define MPB_PAYLOAD_FLAG_COMPRESSED 0x0002u

#define MPB_SIG_ALGORITHM_ECDSA_P256_SHA256 1u
#define MPB_SIG_RECORD_VERSION 1u

#define MPB_TLV_FLAG_REQUIRED 0x0001u

typedef enum {
    MPB_PACKAGE_CODE = 1,          /* exactly one ELF payload, flags bit0 clear */
    MPB_PACKAGE_RESOURCE_ONLY = 2, /* 1..16 resource payloads, flags bit0 set */
} mpb_package_kind_t;

/* Canonical manifest TLV field ids (1..35); see manifest_builder._FIELD_DESCS. */
enum {
    MPB_TLV_NAME = 1,
    MPB_TLV_VERSION = 2,
    MPB_TLV_REQUIRES = 3,
    MPB_TLV_MAX_MEMORY_BYTES = 4,
    MPB_TLV_IRAM_REQUIRED_BYTES = 5,
    MPB_TLV_CORE_AFFINITY = 6,
    MPB_TLV_STRAND_QUEUE_CAPACITY = 7,
    MPB_TLV_MAX_OUTSTANDING_OPERATIONS = 8,
    MPB_TLV_MAX_TIMERS = 9,
    MPB_TLV_MAX_MANAGED_TASKS = 10,
    MPB_TLV_MANAGED_TASK_PRIORITY = 11,
    MPB_TLV_MANAGED_TASK_STACK_SIZE = 12,
    MPB_TLV_HEARTBEAT_INTERVAL_MS = 13,
    MPB_TLV_EVENT_RATE_LIMIT = 14,
    MPB_TLV_MAX_EVENT_SUBSCRIPTIONS = 15,
    MPB_TLV_MAX_EVENT_MESSAGE_BYTES = 16,
    MPB_TLV_MAX_EVENT_BUFFER_BYTES = 17,
    MPB_TLV_MAX_OUTSTANDING_RPC = 18,
    MPB_TLV_MAX_DISPLAY_COMMANDS = 19,
    MPB_TLV_MAX_DISPLAY_BYTES = 20,
    MPB_TLV_MAX_STORAGE_REQUESTS = 21,
    MPB_TLV_MAX_STORAGE_IO_BYTES = 22,
    MPB_TLV_MAX_STORAGE_COPY_BYTES = 23,
    MPB_TLV_MAX_TCP_CONNECTIONS = 24,
    MPB_TLV_MAX_UDP_SOCKETS = 25,
    MPB_TLV_MAX_NETWORK_REQUESTS = 26,
    MPB_TLV_MAX_NETWORK_BUFFER_BYTES = 27,
    MPB_TLV_MAX_NETWORK_IO_BYTES = 28,
    MPB_TLV_MAX_LOG_RECORDS_PER_SECOND = 29,
    MPB_TLV_MAX_LOG_BUFFER_BYTES = 30,
    MPB_TLV_MAX_RESOURCE_LEASES = 31,
    MPB_TLV_MAX_UPDATE_BACKLOG_EVENTS = 32,
    MPB_TLV_MAX_UPDATE_BACKLOG_BYTES = 33,
    MPB_TLV_STATE_SCHEMA_ID = 34,
    MPB_TLV_STATE_SCHEMA_VERSION = 35,
};

/* Injected crypto backend (SEC-002/SEC-004). The same parser source compiles
 * on host (tests inject a reference SHA-256 plus a known-answer ECDSA double)
 * and on target (mpb_mbedtls_glue.c injects PSA crypto / mbedtls). */
typedef struct mpb_crypto {
    void* ctx; /* caller-owned context passed to every callback */
    /* One-shot SHA-256 over data[0..length). Always writes digest_out. */
    void (*sha256_fn)(void* ctx, const uint8_t* data, size_t length, uint8_t digest_out[32]);
    /* ECDSA P-256 verification of the IEEE P1363 r||s signature over the
     * 32 byte digest, restricted to key_id. Returns 1 when the signature is
     * valid for a trusted key with that id, 0 otherwise (unknown key_id
     * counts as invalid). */
    int (*ecdsa_verify_fn)(void* ctx, uint32_t key_id, const uint8_t digest[32],
                           const uint8_t signature_p1363[64]);
} mpb_crypto_t;

/* Device policy applied in the final verification stage. The epoch floor is
 * the compile-time virtual floor of the PoC (no eFuse counters are burned). */
typedef struct mpb_policy {
    uint32_t max_package_bytes; /* FRAME hard maximum, e.g. 512*1024 for PoC */
    uint32_t epoch_floor;
    uint16_t abi_major;
    uint16_t abi_minor;
    uint64_t device_features;
} mpb_policy_t;

/* Verified byte span inside the caller's staging buffer. */
typedef struct {
    size_t offset;
    size_t length;
} mpb_span_t;

/* Result view. All pointers refer to the immutable staging buffer passed to
 * mpb_parse(); they stay valid exactly as long as that buffer stays alive
 * and unmodified. No payload bytes are copied. */
typedef struct {
    const uint8_t* staging; /* staging base the spans and pointers refer to */
    mpb_package_kind_t package_kind;
    uint16_t payload_count;
    mpb_span_t payloads[MPB_MAX_PAYLOAD_COUNT];
    /* fixed header fields the loader/harness consume */
    uint32_t security_epoch;
    uint32_t key_id;
    uint32_t target_id;
    uint16_t core_abi_major;
    uint16_t core_abi_minor;
    uint64_t required_features;
    uint64_t optional_features;
    /* manifest essentials (zero when the package does not declare them) */
    const uint8_t* name; /* not NUL terminated; use name_length */
    size_t name_length;
    const uint8_t* version;
    size_t version_length;
    uint32_t max_memory_bytes;
    uint32_t iram_required_bytes;
    /* manifest region for mpb_manifest_field() */
    uint32_t manifest_offset;
    uint32_t manifest_length;
    uint64_t seen_fields_mask; /* bit (id-1) set for every decoded TLV id */
} mpb_view_t;

/* Raw manifest TLV accessor for verified views: returns FRAME_OK and fills
 * out with the field's flags and value span (pointing into staging), or
 * FRAME_ERR_NOT_FOUND when the package does not carry the field. */
typedef struct {
    uint16_t field_id;
    uint16_t flags;
    const uint8_t* value; /* points into the staging buffer */
    uint32_t value_length;
} mpb_field_t;

/* Parse and fully verify an MPB container held in an immutable staging
 * buffer. Returns FRAME_OK and fills out_view, or a FRAME_ERR_* code with
 * out_view zeroed. Fails closed: any malformed byte, unknown construct,
 * wrong key, bad signature, hash mismatch or policy violation rejects the
 * whole container before any payload byte is executed or re-parsed. */
frame_err_t mpb_parse(const uint8_t* staging, size_t size, const mpb_policy_t* policy,
                      const mpb_crypto_t* crypto, mpb_view_t* out_view);

frame_err_t mpb_manifest_field(const mpb_view_t* view, uint16_t field_id, mpb_field_t* out);

#ifdef __cplusplus
}
#endif

#endif
