#include "frame_poc/mpb_parser.h"

/* Byte-by-byte little-endian readers: no unaligned casts, no padding reads,
 * no signed shift overflow. All region arithmetic below runs in uint64_t
 * before any narrowing comparison. */

static uint16_t mpb_rd16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}

static uint32_t mpb_rd32(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)((uint32_t)p[1] << 8) | (uint32_t)((uint32_t)p[2] << 16) |
           (uint32_t)((uint32_t)p[3] << 24);
}

static uint64_t mpb_rd64(const uint8_t* p) {
    return (uint64_t)mpb_rd32(p) | ((uint64_t)mpb_rd32(p + 4) << 32);
}

/* Constant-time equality for secrets and digests. */
static int mpb_bytes_equal(const uint8_t* a, const uint8_t* b, size_t length) {
    uint8_t diff = 0;
    for (size_t i = 0; i < length; ++i) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

static void mpb_zero_view(mpb_view_t* view) {
    uint8_t* p = (uint8_t*)view;
    for (size_t i = 0; i < sizeof(*view); ++i) {
        p[i] = 0;
    }
}

/* Canonical manifest field kinds (mirrors manifest_builder._FIELD_DESCS). */
typedef enum {
    MPB_KIND_U32 = 1,
    MPB_KIND_NAME,
    MPB_KIND_SEMVER,
    MPB_KIND_STRLIST,
    MPB_KIND_AFFINITY,
    MPB_KIND_SCHEMA_ID,
} mpb_kind_t;

typedef struct {
    uint16_t id;
    uint8_t kind;
} mpb_field_desc_t;

static const mpb_field_desc_t mpb_field_table[35] = {
    {1, MPB_KIND_NAME}, {2, MPB_KIND_SEMVER},     {3, MPB_KIND_STRLIST}, {4, MPB_KIND_U32},
    {5, MPB_KIND_U32},  {6, MPB_KIND_AFFINITY},   {7, MPB_KIND_U32},     {8, MPB_KIND_U32},
    {9, MPB_KIND_U32},  {10, MPB_KIND_U32},       {11, MPB_KIND_U32},    {12, MPB_KIND_U32},
    {13, MPB_KIND_U32}, {14, MPB_KIND_U32},       {15, MPB_KIND_U32},    {16, MPB_KIND_U32},
    {17, MPB_KIND_U32}, {18, MPB_KIND_U32},       {19, MPB_KIND_U32},    {20, MPB_KIND_U32},
    {21, MPB_KIND_U32}, {22, MPB_KIND_U32},       {23, MPB_KIND_U32},    {24, MPB_KIND_U32},
    {25, MPB_KIND_U32}, {26, MPB_KIND_U32},       {27, MPB_KIND_U32},    {28, MPB_KIND_U32},
    {29, MPB_KIND_U32}, {30, MPB_KIND_U32},       {31, MPB_KIND_U32},    {32, MPB_KIND_U32},
    {33, MPB_KIND_U32}, {34, MPB_KIND_SCHEMA_ID}, {35, MPB_KIND_U32},
};

static const mpb_field_desc_t* mpb_lookup_field(uint16_t field_id) {
    for (size_t i = 0; i < sizeof(mpb_field_table) / sizeof(mpb_field_table[0]); ++i) {
        if (mpb_field_table[i].id == field_id) {
            return &mpb_field_table[i];
        }
    }
    return NULL;
}

/* Structural validation of the requires str-list encoding:
 * count u8 | (entry_length u16 | entry bytes){count}. */
static frame_err_t mpb_check_strlist(const uint8_t* body, uint32_t length) {
    if (length < 1) {
        return FRAME_ERR_PACKAGE_INVALID;
    }
    uint32_t count = body[0];
    if (count > MPB_MAX_REQUIRES_ENTRIES) {
        return FRAME_ERR_PACKAGE_INVALID;
    }
    uint32_t cursor = 1;
    for (uint32_t i = 0; i < count; ++i) {
        if (length - cursor < 2) {
            return FRAME_ERR_PACKAGE_INVALID;
        }
        uint32_t entry_length =
            (uint32_t)body[cursor] | (uint32_t)((uint32_t)body[cursor + 1] << 8);
        cursor += 2;
        if (entry_length < 1 || entry_length > MPB_MAX_REQUIRE_BYTES ||
            entry_length > length - cursor) {
            return FRAME_ERR_PACKAGE_INVALID;
        }
        cursor += entry_length;
    }
    if (cursor != length) {
        return FRAME_ERR_PACKAGE_INVALID;
    }
    return FRAME_OK;
}

/* Walk the manifest TLV region: bounds, unknown/required, duplicates, value
 * shapes, presence of name and version. Captures the spans and quota values
 * the view exposes plus the seen-id mask for mpb_manifest_field(). */
static frame_err_t mpb_validate_manifest(const uint8_t* region, uint32_t length,
                                         uint64_t* seen_mask, mpb_view_t* view) {
    uint64_t mask = 0;
    const uint8_t* name = NULL;
    uint32_t name_length = 0;
    const uint8_t* version = NULL;
    uint32_t version_length = 0;
    uint32_t cursor = 0;

    while (cursor < length) {
        if ((uint64_t)length - cursor < MPB_TLV_HEADER_SIZE) {
            return FRAME_ERR_PACKAGE_INVALID;
        }
        const uint8_t* head = region + cursor;
        uint16_t field_id = mpb_rd16(head);
        uint16_t flags = mpb_rd16(head + 2);
        uint32_t value_length = mpb_rd32(head + 4);
        cursor += MPB_TLV_HEADER_SIZE;
        if ((flags & ~(uint16_t)MPB_TLV_FLAG_REQUIRED) != 0) {
            return FRAME_ERR_PACKAGE_INVALID;
        }
        if (value_length > length - cursor) {
            return FRAME_ERR_PACKAGE_INVALID;
        }
        const uint8_t* body = region + cursor;
        cursor += value_length;

        const mpb_field_desc_t* desc = mpb_lookup_field(field_id);
        bool required = (flags & MPB_TLV_FLAG_REQUIRED) != 0;
        if (desc == NULL) {
            if (required) {
                return FRAME_ERR_PACKAGE_INVALID;
            }
            continue; /* unknown optional TLV: bounds-checked and skipped */
        }
        if ((mask & ((uint64_t)1 << (field_id - 1))) != 0) {
            return FRAME_ERR_PACKAGE_INVALID; /* duplicated known field */
        }
        mask |= (uint64_t)1 << (field_id - 1);

        switch (desc->kind) {
        case MPB_KIND_U32:
            if (value_length != 4) {
                return FRAME_ERR_PACKAGE_INVALID;
            }
            if (field_id == MPB_TLV_MAX_MEMORY_BYTES) {
                view->max_memory_bytes = mpb_rd32(body);
            } else if (field_id == MPB_TLV_IRAM_REQUIRED_BYTES) {
                view->iram_required_bytes = mpb_rd32(body);
            }
            break;
        case MPB_KIND_AFFINITY:
            if (value_length != 1 || body[0] > 2) {
                return FRAME_ERR_PACKAGE_INVALID;
            }
            break;
        case MPB_KIND_SCHEMA_ID:
            if (value_length != 16) {
                return FRAME_ERR_PACKAGE_INVALID;
            }
            break;
        case MPB_KIND_NAME:
            if (value_length < 1 || value_length > MPB_MAX_NAME_BYTES) {
                return FRAME_ERR_PACKAGE_INVALID;
            }
            name = body;
            name_length = value_length;
            break;
        case MPB_KIND_SEMVER:
            if (value_length < 1 || value_length > MPB_MAX_VERSION_BYTES) {
                return FRAME_ERR_PACKAGE_INVALID;
            }
            version = body;
            version_length = value_length;
            break;
        case MPB_KIND_STRLIST: {
            frame_err_t err = mpb_check_strlist(body, value_length);
            if (err != FRAME_OK) {
                return err;
            }
            break;
        }
        default:
            return FRAME_ERR_PACKAGE_INVALID;
        }
    }

    if (name == NULL || version == NULL) {
        return FRAME_ERR_PACKAGE_INVALID; /* name and version are required */
    }
    view->name = name;
    view->name_length = name_length;
    view->version = version;
    view->version_length = version_length;
    *seen_mask = mask;
    return FRAME_OK;
}

frame_err_t mpb_parse(const uint8_t* staging, size_t size, const mpb_policy_t* policy,
                      const mpb_crypto_t* crypto, mpb_view_t* out_view) {
    if (staging == NULL || policy == NULL || crypto == NULL || out_view == NULL) {
        return FRAME_ERR_INVALID_ARGUMENT;
    }
    if (crypto->sha256_fn == NULL || crypto->ecdsa_verify_fn == NULL) {
        return FRAME_ERR_INVALID_ARGUMENT;
    }
    if (policy->max_package_bytes < MPB_HEADER_SIZE) {
        return FRAME_ERR_INVALID_ARGUMENT;
    }
    mpb_zero_view(out_view);

    /* --- stage 1: structure, bounds, types (all PACKAGE_INVALID) --- */

    if ((uint64_t)size < MPB_HEADER_SIZE) {
        return FRAME_ERR_PACKAGE_INVALID;
    }
    if ((uint64_t)size > policy->max_package_bytes) {
        return FRAME_ERR_PACKAGE_INVALID;
    }
    if (mpb_rd32(staging + 0) != MPB_MAGIC) {
        return FRAME_ERR_PACKAGE_INVALID;
    }
    if (mpb_rd16(staging + 4) != 1) { /* format_major */
        return FRAME_ERR_PACKAGE_INVALID;
    }
    if (mpb_rd32(staging + 8) != MPB_HEADER_SIZE) {
        return FRAME_ERR_PACKAGE_INVALID;
    }
    if ((uint64_t)mpb_rd32(staging + 12) != (uint64_t)size) { /* total_size */
        return FRAME_ERR_PACKAGE_INVALID;
    }
    uint32_t flags = mpb_rd32(staging + 16);
    if ((flags & ~(uint32_t)MPB_FLAG_RESOURCE_ONLY) != 0) {
        return FRAME_ERR_PACKAGE_INVALID;
    }
    uint32_t target_id = mpb_rd32(staging + 20);
    uint16_t core_abi_major = mpb_rd16(staging + 24);
    uint16_t core_abi_minor = mpb_rd16(staging + 26);
    uint64_t required_features = mpb_rd64(staging + 28);
    uint64_t optional_features = mpb_rd64(staging + 36);
    uint32_t manifest_offset = mpb_rd32(staging + 44);
    uint32_t manifest_length = mpb_rd32(staging + 48);
    uint32_t table_offset = mpb_rd32(staging + 52);
    uint16_t payload_count = mpb_rd16(staging + 56);
    uint16_t entry_size = mpb_rd16(staging + 58);
    uint32_t signature_offset = mpb_rd32(staging + 60);
    uint32_t signature_length = mpb_rd32(staging + 64);
    uint32_t key_id = mpb_rd32(staging + 68);
    uint32_t security_epoch = mpb_rd32(staging + 72);

    if (payload_count < 1 || payload_count > MPB_MAX_PAYLOAD_COUNT) {
        return FRAME_ERR_PACKAGE_INVALID;
    }
    if (entry_size != MPB_PAYLOAD_ENTRY_SIZE) {
        return FRAME_ERR_PACKAGE_INVALID;
    }
    if (signature_length != MPB_SIGNATURE_RECORD_SIZE) {
        return FRAME_ERR_PACKAGE_INVALID;
    }
    if (manifest_offset != MPB_HEADER_SIZE) {
        return FRAME_ERR_PACKAGE_INVALID;
    }
    if (manifest_length < MPB_TLV_HEADER_SIZE + 1) {
        return FRAME_ERR_PACKAGE_INVALID;
    }
    if ((uint64_t)table_offset != (uint64_t)manifest_offset + manifest_length) {
        return FRAME_ERR_PACKAGE_INVALID;
    }
    uint64_t table_end = (uint64_t)table_offset + (uint64_t)payload_count * entry_size;
    if (table_end > (uint64_t)size) {
        return FRAME_ERR_PACKAGE_INVALID;
    }
    if ((uint64_t)signature_offset < table_end) {
        return FRAME_ERR_PACKAGE_INVALID;
    }
    if ((uint64_t)signature_offset + MPB_SIGNATURE_RECORD_SIZE != (uint64_t)size) {
        return FRAME_ERR_PACKAGE_INVALID; /* no trailing undescribed bytes */
    }

    uint32_t elf_count = 0;
    uint32_t resource_count = 0;
    uint64_t offsets[MPB_MAX_PAYLOAD_COUNT];
    uint64_t lengths[MPB_MAX_PAYLOAD_COUNT];
    for (uint32_t i = 0; i < payload_count; ++i) {
        const uint8_t* entry = staging + table_offset + (uint64_t)i * MPB_PAYLOAD_ENTRY_SIZE;
        uint16_t payload_type = mpb_rd16(entry + 0);
        uint16_t payload_flags = mpb_rd16(entry + 2);
        uint64_t offset = mpb_rd32(entry + 4);
        uint64_t length = mpb_rd32(entry + 8);
        uint64_t unpacked = mpb_rd32(entry + 12);
        uint64_t alignment = mpb_rd32(entry + 16);

        if (payload_type != MPB_PAYLOAD_TYPE_ELF && payload_type != MPB_PAYLOAD_TYPE_RESOURCE) {
            return FRAME_ERR_PACKAGE_INVALID;
        }
        if ((payload_flags &
             ~(uint16_t)(MPB_PAYLOAD_FLAG_REQUIRED | MPB_PAYLOAD_FLAG_COMPRESSED)) != 0) {
            return FRAME_ERR_PACKAGE_INVALID;
        }
        if ((payload_flags & MPB_PAYLOAD_FLAG_REQUIRED) == 0) {
            return FRAME_ERR_PACKAGE_INVALID;
        }
        if (payload_type == MPB_PAYLOAD_TYPE_ELF &&
            (payload_flags & MPB_PAYLOAD_FLAG_COMPRESSED) != 0) {
            return FRAME_ERR_PACKAGE_INVALID;
        }
        if (unpacked != length) {
            return FRAME_ERR_PACKAGE_INVALID;
        }
        if (alignment < 1 || alignment > MPB_MAX_ALIGNMENT || (alignment & (alignment - 1)) != 0) {
            return FRAME_ERR_PACKAGE_INVALID;
        }
        if (offset % alignment != 0) {
            return FRAME_ERR_PACKAGE_INVALID;
        }
        if (offset < table_end || offset + length > (uint64_t)signature_offset) {
            return FRAME_ERR_PACKAGE_INVALID;
        }
        for (uint32_t j = 0; j < i; ++j) {
            if (offset < offsets[j] + lengths[j] && offsets[j] < offset + length) {
                return FRAME_ERR_PACKAGE_INVALID; /* region overlap */
            }
        }
        offsets[i] = offset;
        lengths[i] = length;
        if (payload_type == MPB_PAYLOAD_TYPE_ELF) {
            elf_count += 1;
        } else {
            resource_count += 1;
        }
    }

    bool resource_only_flag = (flags & MPB_FLAG_RESOURCE_ONLY) != 0;
    if (elf_count > 1 || (elf_count > 0 && resource_count > 0)) {
        return FRAME_ERR_PACKAGE_INVALID; /* strict single-type (PLUG-001) */
    }
    if (elf_count == 1 && resource_only_flag) {
        return FRAME_ERR_PACKAGE_INVALID; /* flags disagree with composition */
    }
    if (elf_count == 0 && resource_count > 0 && !resource_only_flag) {
        return FRAME_ERR_PACKAGE_INVALID; /* resource-only must set bit0 */
    }

    uint64_t seen_mask = 0;
    frame_err_t err =
        mpb_validate_manifest(staging + manifest_offset, manifest_length, &seen_mask, out_view);
    if (err != FRAME_OK) {
        mpb_zero_view(out_view);
        return err;
    }

    const uint8_t* record = staging + signature_offset;
    if (mpb_rd16(record + 0) != MPB_SIG_ALGORITHM_ECDSA_P256_SHA256 ||
        mpb_rd16(record + 2) != MPB_SIG_RECORD_VERSION || mpb_rd32(record + 4) != key_id) {
        return FRAME_ERR_PACKAGE_INVALID;
    }
    const uint8_t* signature = record + 8;

    /* --- stage 2: signature over header || manifest || payload_table --- */

    uint8_t digest[32];
    crypto->sha256_fn(crypto->ctx, staging, (size_t)table_end, digest);
    if (crypto->ecdsa_verify_fn(crypto->ctx, key_id, digest, signature) != 1) {
        mpb_zero_view(out_view);
        return FRAME_ERR_SIGNATURE_INVALID;
    }

    /* --- stage 3: per-payload hashes --- */

    for (uint32_t i = 0; i < payload_count; ++i) {
        const uint8_t* entry = staging + table_offset + (uint64_t)i * MPB_PAYLOAD_ENTRY_SIZE;
        uint8_t actual[32];
        crypto->sha256_fn(crypto->ctx, staging + offsets[i], (size_t)lengths[i], actual);
        if (!mpb_bytes_equal(actual, entry + 20, 32)) {
            mpb_zero_view(out_view);
            return FRAME_ERR_HASH_MISMATCH;
        }
    }

    /* --- stage 4: epoch / target / ABI / features policy --- */

    if (security_epoch < policy->epoch_floor) {
        mpb_zero_view(out_view);
        return FRAME_ERR_EPOCH_ROLLBACK;
    }
    if (target_id != MPB_TARGET_ESP32S3) {
        mpb_zero_view(out_view);
        return FRAME_ERR_TARGET_MISMATCH;
    }
    if (core_abi_major != policy->abi_major || core_abi_minor > policy->abi_minor) {
        mpb_zero_view(out_view);
        return FRAME_ERR_ABI_MISMATCH;
    }
    if ((required_features & ~policy->device_features) != 0) {
        mpb_zero_view(out_view);
        return FRAME_ERR_ABI_MISMATCH;
    }

    out_view->staging = staging;
    out_view->package_kind = resource_only_flag ? MPB_PACKAGE_RESOURCE_ONLY : MPB_PACKAGE_CODE;
    out_view->payload_count = payload_count;
    for (uint32_t i = 0; i < payload_count; ++i) {
        out_view->payloads[i].offset = (size_t)offsets[i];
        out_view->payloads[i].length = (size_t)lengths[i];
    }
    out_view->security_epoch = security_epoch;
    out_view->key_id = key_id;
    out_view->target_id = target_id;
    out_view->core_abi_major = core_abi_major;
    out_view->core_abi_minor = core_abi_minor;
    out_view->required_features = required_features;
    out_view->optional_features = optional_features;
    out_view->manifest_offset = manifest_offset;
    out_view->manifest_length = manifest_length;
    out_view->seen_fields_mask = seen_mask;
    return FRAME_OK;
}

frame_err_t mpb_manifest_field(const mpb_view_t* view, uint16_t field_id, mpb_field_t* out) {
    if (view == NULL || out == NULL || view->staging == NULL) {
        return FRAME_ERR_INVALID_ARGUMENT;
    }
    if (field_id < 1 || field_id > 35 ||
        (view->seen_fields_mask & ((uint64_t)1 << (field_id - 1))) == 0) {
        return FRAME_ERR_NOT_FOUND;
    }
    const uint8_t* region = view->staging + view->manifest_offset;
    uint32_t remaining = view->manifest_length;
    while (remaining >= MPB_TLV_HEADER_SIZE) {
        uint16_t id = mpb_rd16(region);
        uint16_t value_flags = mpb_rd16(region + 2);
        uint32_t value_length = mpb_rd32(region + 4);
        region += MPB_TLV_HEADER_SIZE;
        remaining -= MPB_TLV_HEADER_SIZE;
        if (id == field_id) {
            out->field_id = id;
            out->flags = value_flags;
            out->value = region;
            out->value_length = value_length;
            return FRAME_OK;
        }
        /* Region already validated by mpb_parse; guard defensively anyway. */
        if (value_length > remaining) {
            return FRAME_ERR_NOT_FOUND;
        }
        region += value_length;
        remaining -= value_length;
    }
    return FRAME_ERR_NOT_FOUND;
}
