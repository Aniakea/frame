#ifndef FRAME_POC_FRAME_ABI_H
#define FRAME_POC_FRAME_ABI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int32_t frame_err_t;

enum {
    FRAME_OK = 0,
    FRAME_ERR_INVALID_PTR = -1,
    FRAME_ERR_TIMEOUT = -2,
    FRAME_ERR_QUOTA_EXCEEDED = -3,
    FRAME_ERR_LEASE_FULL = -4,
    FRAME_ERR_PLUGIN_NOT_FOUND = -5,
    FRAME_ERR_DEP_CYCLE = -6,
    FRAME_ERR_ABI_MISMATCH = -7,
    FRAME_ERR_BUSY = -8,
    FRAME_ERR_INVALID_CONTEXT = -9,
    FRAME_ERR_NET_AGAIN = -10,
    FRAME_ERR_WDT_TRIGGERED = -11,
    FRAME_ERR_ROLLBACK_FAILED = -12,
    FRAME_ERR_QUEUE_FULL = -13,
    FRAME_ERR_PLUGIN_STOPPING = -14,
    FRAME_ERR_CANCELLED = -15,
    FRAME_ERR_RESTART_REQUIRED = -16,
    FRAME_ERR_SIGNATURE_INVALID = -17,
    FRAME_ERR_PACKAGE_INVALID = -18,
    FRAME_ERR_HASH_MISMATCH = -19,
    FRAME_ERR_FS_NOT_FOUND = -20,
    FRAME_ERR_FS_TIMEOUT = -21,
    FRAME_ERR_FS_IO = -22,
    FRAME_ERR_FS_NO_TF = -23,
    FRAME_ERR_FS_RECOVERING = -24,
    FRAME_ERR_DURABILITY_DEGRADED = -25,
    FRAME_ERR_CAPACITY = -26,
    FRAME_ERR_NOT_FOUND = -27,
    FRAME_ERR_INVALID_ARGUMENT = -28,
    FRAME_ERR_RATE_LIMITED = -29,
    FRAME_ERR_MEM_FRAGMENTED = -30,
    FRAME_ERR_SEMVER_MISMATCH = -31,
    FRAME_ERR_IRAM_EXHAUSTED = -32,
    FRAME_ERR_STATE_SCHEMA = -33,
    FRAME_ERR_EPOCH_ROLLBACK = -34,
    FRAME_ERR_TARGET_MISMATCH = -35,
};

enum {
    FRAME_ABI_MAJOR = 1,
    FRAME_ABI_MINOR = 0,
};

typedef uint64_t plugin_instance_handle_t;
typedef uint64_t frame_buffer_handle_t;
typedef uint64_t steady_timer_handle_t;
typedef uint64_t managed_task_handle_t;
typedef uint64_t resource_handle_t;
typedef uint64_t frame_request_handle_t;
typedef uint64_t frame_allocation_handle_t;

typedef struct {
    uint32_t struct_size;
    uint16_t abi_major;
    uint16_t abi_minor;
    uint64_t feature_bits;
} frame_abi_header_t;

typedef struct {
    void (*invoke)(void* user);
    void (*destroy)(void* user);
    void* user;
} frame_post_operation_t;

typedef struct {
    void (*complete)(void* user, frame_err_t err);
    void (*destroy)(void* user);
    void* user;
} frame_completion_operation_t;

static inline bool frame_abi_supports(const frame_abi_header_t* offered, uint16_t required_major,
                                      uint16_t required_minor, uint32_t required_size,
                                      uint64_t required_features) {
    return offered != NULL && offered->abi_major == required_major &&
           offered->abi_minor >= required_minor && offered->struct_size >= required_size &&
           (offered->feature_bits & required_features) == required_features;
}

#ifdef __cplusplus
}
#endif

#endif
