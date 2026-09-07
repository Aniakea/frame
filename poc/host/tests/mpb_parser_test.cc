#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "frame_poc/mpb_parser.h"
#include "mpb_corpus/corpus_manifest.inc"
#include "sha256_ref.h"

namespace {

std::vector<uint8_t> read_file(const std::string& path) {
    std::FILE* handle = std::fopen(path.c_str(), "rb");
    if (handle == nullptr) {
        ADD_FAILURE() << "cannot open " << path;
        return {};
    }
    std::fseek(handle, 0, SEEK_END);
    long size = std::ftell(handle);
    std::fseek(handle, 0, SEEK_SET);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    if (size > 0 && std::fread(data.data(), 1, data.size(), handle) != data.size()) {
        ADD_FAILURE() << "short read on " << path;
        data.clear();
    }
    std::fclose(handle);
    return data;
}

std::string to_hex(const uint8_t* data, size_t length) {
    static const char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(length * 2);
    for (size_t i = 0; i < length; ++i) {
        out.push_back(kDigits[data[i] >> 4]);
        out.push_back(kDigits[data[i] & 0xF]);
    }
    return out;
}

int from_hex(const std::string& hex, uint8_t* out, size_t capacity) {
    if (hex.size() / 2 > capacity || hex.size() % 2 != 0) {
        return -1;
    }
    for (size_t i = 0; i < hex.size() / 2; ++i) {
        out[i] = static_cast<uint8_t>(std::stoul(hex.substr(i * 2, 2), nullptr, 16));
    }
    return static_cast<int>(hex.size() / 2);
}

/* Known-answer crypto double: sha256 is the local reference implementation,
 * ecdsa verification accepts exactly the builder-produced triple recorded in
 * the corpus (see tests/mpb_corpus/README.md for the strategy). */
struct KatCrypto {
    uint8_t expected_digest[32];
    uint8_t expected_signature[64];
    uint32_t trusted_key_id;
    int sha_calls = 0;
    int verify_calls = 0;
};

int ct_equal(const uint8_t* a, const uint8_t* b, size_t n) {
    uint8_t diff = 0;
    for (size_t i = 0; i < n; ++i) {
        diff = static_cast<uint8_t>(diff | (a[i] ^ b[i]));
    }
    return diff == 0;
}

void kat_sha256(void* ctx, const uint8_t* data, size_t length, uint8_t out[32]) {
    auto* kat = static_cast<KatCrypto*>(ctx);
    ++kat->sha_calls;
    sha256_ref(data, length, out);
}

int kat_verify(void* ctx, uint32_t key_id, const uint8_t digest[32], const uint8_t signature[64]) {
    auto* kat = static_cast<KatCrypto*>(ctx);
    ++kat->verify_calls;
    if (key_id != kat->trusted_key_id) {
        return 0;
    }
    return ct_equal(digest, kat->expected_digest, 32) &&
           ct_equal(signature, kat->expected_signature, 64);
}

mpb_policy_t default_policy() {
    return mpb_policy_t{
        .max_package_bytes = 512u * 1024u,
        .epoch_floor = MPB_CORPUS_EPOCH_FLOOR,
        .abi_major = 1,
        .abi_minor = 0,
        .device_features = 0,
    };
}

void kat_crypto_for(const mpb_corpus_entry_t& entry, mpb_crypto_t* crypto, KatCrypto* kat) {
    /* Every fixture records the legitimate (digest, signature) pair; see
     * tests/mpb_corpus/README.md for the known-answer strategy. */
    ASSERT_EQ(from_hex(entry.kat_digest_hex, kat->expected_digest, 32), 32);
    ASSERT_EQ(from_hex(entry.kat_signature_hex, kat->expected_signature, 64), 64);
    kat->trusted_key_id = MPB_CORPUS_KEY_ID_A;
    crypto->ctx = kat;
    crypto->sha256_fn = &kat_sha256;
    crypto->ecdsa_verify_fn = &kat_verify;
}

const mpb_corpus_entry_t* find_entry(const char* name) {
    for (const auto& entry : k_mpb_corpus) {
        if (std::string(entry.file) == name) {
            return &entry;
        }
    }
    ADD_FAILURE() << "corpus entry " << name << " missing";
    return nullptr;
}

std::vector<uint8_t> load_entry(const mpb_corpus_entry_t& entry) {
    std::vector<uint8_t> data = read_file(std::string(MPB_CORPUS_DIR) + "/" + entry.file);
    uint8_t actual[32];
    sha256_ref(data.data(), data.size(), actual);
    EXPECT_EQ(to_hex(actual, 32), std::string(entry.file_sha256_hex))
        << "corpus file " << entry.file << " does not match its recorded sha256";
    return data;
}

frame_err_t parse_with(const mpb_corpus_entry_t& entry, const std::vector<uint8_t>& data,
                       const mpb_policy_t& policy, mpb_view_t* view, KatCrypto* kat) {
    mpb_crypto_t crypto{};
    kat_crypto_for(entry, &crypto, kat);
    return mpb_parse(data.data(), data.size(), &policy, &crypto, view);
}

TEST(MpbParser, EveryFixtureMatchesItsExpectedErrorCode) {
    for (const auto& entry : k_mpb_corpus) {
        SCOPED_TRACE(entry.file);
        std::vector<uint8_t> data = load_entry(entry);
        ASSERT_FALSE(data.empty());
        mpb_view_t view{};
        KatCrypto kat{};
        frame_err_t err = parse_with(entry, data, default_policy(), &view, &kat);
        EXPECT_EQ(err, entry.expected_error);
        if (entry.expected_error != FRAME_OK) {
            EXPECT_EQ(view.staging, nullptr) << "view must stay zeroed on failure";
        }
    }
}

TEST(MpbParser, CodePositiveVerifiesAndExposesView) {
    const mpb_corpus_entry_t* entry = find_entry("pos_code.mpb");
    ASSERT_NE(entry, nullptr);
    std::vector<uint8_t> data = load_entry(*entry);
    mpb_view_t view{};
    KatCrypto kat{};
    ASSERT_EQ(parse_with(*entry, data, default_policy(), &view, &kat), FRAME_OK);

    EXPECT_EQ(view.package_kind, MPB_PACKAGE_CODE);
    EXPECT_EQ(view.payload_count, 1);
    EXPECT_EQ(view.security_epoch, 1u);
    EXPECT_EQ(view.key_id, MPB_CORPUS_KEY_ID_A);
    EXPECT_EQ(view.target_id, 0x33505345u);
    EXPECT_EQ(view.core_abi_major, 1);
    EXPECT_EQ(view.core_abi_minor, 0);
    EXPECT_EQ(view.required_features, 0u);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(view.name), view.name_length),
              std::string(entry->name));
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(view.version), view.version_length),
              std::string(entry->version));
    EXPECT_EQ(view.max_memory_bytes, 262144u);
    EXPECT_EQ(view.iram_required_bytes, 4660u);

    ASSERT_EQ(view.payload_count, 1);
    const uint8_t* elf = view.staging + view.payloads[0].offset;
    EXPECT_EQ(view.payloads[0].length,
              data.size() - view.payloads[0].offset - MPB_SIGNATURE_RECORD_SIZE);
    EXPECT_EQ(elf[0], 0x7F);
    EXPECT_EQ(elf[1], 'E');
    EXPECT_EQ(elf[2], 'L');
    EXPECT_EQ(elf[3], 'F');
    uint8_t span_digest[32];
    sha256_ref(elf, view.payloads[0].length, span_digest);
    EXPECT_EQ(to_hex(span_digest, 32), std::string(entry->payload_sha256_hex[0]));

    EXPECT_EQ(kat.sha_calls, 2); /* signing digest + one payload hash */
    EXPECT_EQ(kat.verify_calls, 1);
}

TEST(MpbParser, ResourceOnlyPositiveVerifiesWithWindowPadding) {
    const mpb_corpus_entry_t* entry = find_entry("pos_resource_only.mpb");
    ASSERT_NE(entry, nullptr);
    std::vector<uint8_t> data = load_entry(*entry);
    mpb_view_t view{};
    KatCrypto kat{};
    ASSERT_EQ(parse_with(*entry, data, default_policy(), &view, &kat), FRAME_OK);

    EXPECT_EQ(view.package_kind, MPB_PACKAGE_RESOURCE_ONLY);
    ASSERT_EQ(view.payload_count, 2);
    EXPECT_EQ(view.iram_required_bytes, 0u);
    for (int i = 0; i < view.payload_count; ++i) {
        SCOPED_TRACE(i);
        uint8_t span_digest[32];
        sha256_ref(view.staging + view.payloads[i].offset, view.payloads[i].length, span_digest);
        EXPECT_EQ(to_hex(span_digest, 32), std::string(entry->payload_sha256_hex[i]));
    }
    EXPECT_EQ(kat.sha_calls, 3); /* signing digest + two payload hashes */
}

TEST(MpbParser, ManifestFieldAccessorRoundTrip) {
    const mpb_corpus_entry_t* entry = find_entry("pos_code.mpb");
    ASSERT_NE(entry, nullptr);
    std::vector<uint8_t> data = load_entry(*entry);
    mpb_view_t view{};
    KatCrypto kat{};
    ASSERT_EQ(parse_with(*entry, data, default_policy(), &view, &kat), FRAME_OK);

    mpb_field_t field{};
    EXPECT_EQ(mpb_manifest_field(&view, MPB_TLV_NAME, &field), FRAME_OK);
    EXPECT_EQ(field.flags, MPB_TLV_FLAG_REQUIRED);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(field.value), field.value_length),
              "poc-a-baseline");
    EXPECT_EQ(mpb_manifest_field(&view, MPB_TLV_REQUIRES, &field), FRAME_OK);
    EXPECT_EQ(field.flags, 0);
    /* requires encoding: count u8 | len u16 | bytes */
    ASSERT_GE(field.value_length, 3u);
    EXPECT_EQ(field.value[0], 1);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(field.value + 3), field.value_length - 3),
              "frame/core>=1.0.0");
    EXPECT_EQ(mpb_manifest_field(&view, MPB_TLV_STATE_SCHEMA_ID, &field), FRAME_ERR_NOT_FOUND);
    EXPECT_EQ(mpb_manifest_field(&view, 99, &field), FRAME_ERR_NOT_FOUND);
    EXPECT_EQ(mpb_manifest_field(&view, MPB_TLV_NAME, nullptr), FRAME_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(mpb_manifest_field(nullptr, MPB_TLV_NAME, &field), FRAME_ERR_INVALID_ARGUMENT);
}

TEST(MpbParser, NullArgumentsAreRejected) {
    const mpb_corpus_entry_t* entry = find_entry("pos_code.mpb");
    ASSERT_NE(entry, nullptr);
    std::vector<uint8_t> data = load_entry(*entry);
    mpb_view_t view{};
    KatCrypto kat{};
    mpb_crypto_t crypto{};
    kat_crypto_for(*entry, &crypto, &kat);
    mpb_policy_t policy = default_policy();

    EXPECT_EQ(mpb_parse(nullptr, data.size(), &policy, &crypto, &view), FRAME_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(mpb_parse(data.data(), data.size(), nullptr, &crypto, &view),
              FRAME_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(mpb_parse(data.data(), data.size(), &policy, nullptr, &view),
              FRAME_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(mpb_parse(data.data(), data.size(), &policy, &crypto, nullptr),
              FRAME_ERR_INVALID_ARGUMENT);
    crypto.ecdsa_verify_fn = nullptr;
    EXPECT_EQ(mpb_parse(data.data(), data.size(), &policy, &crypto, &view),
              FRAME_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(view.staging, nullptr);
}

TEST(MpbParser, CorpusMagic) {
    const mpb_corpus_entry_t* entry = find_entry("neg_magic.mpb");
    ASSERT_NE(entry, nullptr);
    std::vector<uint8_t> data = load_entry(*entry);
    mpb_view_t view{};
    KatCrypto kat{};
    EXPECT_EQ(parse_with(*entry, data, default_policy(), &view, &kat), FRAME_ERR_PACKAGE_INVALID);
    EXPECT_EQ(kat.verify_calls, 0); /* structure is checked before the signature */
}

TEST(MpbParser, CorpusTotalSize) {
    const mpb_corpus_entry_t* entry = find_entry("neg_total_size.mpb");
    ASSERT_NE(entry, nullptr);
    std::vector<uint8_t> data = load_entry(*entry);
    mpb_view_t view{};
    KatCrypto kat{};
    EXPECT_EQ(parse_with(*entry, data, default_policy(), &view, &kat), FRAME_ERR_PACKAGE_INVALID);
}

TEST(MpbParser, CorpusOffsetOutOfBounds) {
    const mpb_corpus_entry_t* entry = find_entry("neg_offset_oob.mpb");
    ASSERT_NE(entry, nullptr);
    std::vector<uint8_t> data = load_entry(*entry);
    mpb_view_t view{};
    KatCrypto kat{};
    EXPECT_EQ(parse_with(*entry, data, default_policy(), &view, &kat), FRAME_ERR_PACKAGE_INVALID);
    EXPECT_EQ(kat.verify_calls, 0);
}

TEST(MpbParser, CorpusRegionOverlap) {
    const mpb_corpus_entry_t* entry = find_entry("neg_overlap.mpb");
    ASSERT_NE(entry, nullptr);
    std::vector<uint8_t> data = load_entry(*entry);
    mpb_view_t view{};
    KatCrypto kat{};
    EXPECT_EQ(parse_with(*entry, data, default_policy(), &view, &kat), FRAME_ERR_PACKAGE_INVALID);
}

TEST(MpbParser, CorpusDuplicateRequiredTlv) {
    const mpb_corpus_entry_t* entry = find_entry("neg_dup_required.mpb");
    ASSERT_NE(entry, nullptr);
    std::vector<uint8_t> data = load_entry(*entry);
    mpb_view_t view{};
    KatCrypto kat{};
    EXPECT_EQ(parse_with(*entry, data, default_policy(), &view, &kat), FRAME_ERR_PACKAGE_INVALID);
}

TEST(MpbParser, CorpusUnknownRequiredTlv) {
    const mpb_corpus_entry_t* entry = find_entry("neg_unknown_required_tlv.mpb");
    ASSERT_NE(entry, nullptr);
    std::vector<uint8_t> data = load_entry(*entry);
    mpb_view_t view{};
    KatCrypto kat{};
    EXPECT_EQ(parse_with(*entry, data, default_policy(), &view, &kat), FRAME_ERR_PACKAGE_INVALID);
}

TEST(MpbParser, CorpusMixedElfInResourceOnly) {
    const mpb_corpus_entry_t* entry = find_entry("neg_mixed_elf_in_res.mpb");
    ASSERT_NE(entry, nullptr);
    std::vector<uint8_t> data = load_entry(*entry);
    mpb_view_t view{};
    KatCrypto kat{};
    EXPECT_EQ(parse_with(*entry, data, default_policy(), &view, &kat), FRAME_ERR_PACKAGE_INVALID);
}

TEST(MpbParser, CorpusMixedResourceInCode) {
    const mpb_corpus_entry_t* entry = find_entry("neg_mixed_res_in_elf.mpb");
    ASSERT_NE(entry, nullptr);
    std::vector<uint8_t> data = load_entry(*entry);
    mpb_view_t view{};
    KatCrypto kat{};
    EXPECT_EQ(parse_with(*entry, data, default_policy(), &view, &kat), FRAME_ERR_PACKAGE_INVALID);
}

TEST(MpbParser, CorpusPayloadHash) {
    const mpb_corpus_entry_t* entry = find_entry("neg_payload_hash.mpb");
    ASSERT_NE(entry, nullptr);
    std::vector<uint8_t> data = load_entry(*entry);
    mpb_view_t view{};
    KatCrypto kat{};
    EXPECT_EQ(parse_with(*entry, data, default_policy(), &view, &kat), FRAME_ERR_HASH_MISMATCH);
    EXPECT_EQ(kat.verify_calls, 1); /* signature must have passed first */
}

TEST(MpbParser, CorpusSignatureTamper) {
    const mpb_corpus_entry_t* entry = find_entry("neg_signature.mpb");
    ASSERT_NE(entry, nullptr);
    std::vector<uint8_t> data = load_entry(*entry);
    mpb_view_t view{};
    KatCrypto kat{};
    EXPECT_EQ(parse_with(*entry, data, default_policy(), &view, &kat), FRAME_ERR_SIGNATURE_INVALID);
    EXPECT_EQ(kat.sha_calls, 1); /* digest computed, payload hashes never reached */
}

TEST(MpbParser, CorpusWrongKeyId) {
    const mpb_corpus_entry_t* entry = find_entry("neg_key_id.mpb");
    ASSERT_NE(entry, nullptr);
    std::vector<uint8_t> data = load_entry(*entry);
    mpb_view_t view{};
    KatCrypto kat{};
    EXPECT_EQ(parse_with(*entry, data, default_policy(), &view, &kat), FRAME_ERR_SIGNATURE_INVALID);
}

TEST(MpbParser, CorpusTargetMismatch) {
    const mpb_corpus_entry_t* entry = find_entry("neg_target.mpb");
    ASSERT_NE(entry, nullptr);
    std::vector<uint8_t> data = load_entry(*entry);
    mpb_view_t view{};
    KatCrypto kat{};
    EXPECT_EQ(parse_with(*entry, data, default_policy(), &view, &kat), FRAME_ERR_TARGET_MISMATCH);
    EXPECT_EQ(kat.sha_calls, 2); /* signature and payload hash both passed */
}

TEST(MpbParser, CorpusAbiMismatch) {
    const mpb_corpus_entry_t* entry = find_entry("neg_abi.mpb");
    ASSERT_NE(entry, nullptr);
    std::vector<uint8_t> data = load_entry(*entry);
    mpb_view_t view{};
    KatCrypto kat{};
    EXPECT_EQ(parse_with(*entry, data, default_policy(), &view, &kat), FRAME_ERR_ABI_MISMATCH);
}

TEST(MpbParser, CorpusFeaturesExceedDevice) {
    const mpb_corpus_entry_t* entry = find_entry("neg_features.mpb");
    ASSERT_NE(entry, nullptr);
    std::vector<uint8_t> data = load_entry(*entry);
    mpb_view_t view{};
    KatCrypto kat{};
    EXPECT_EQ(parse_with(*entry, data, default_policy(), &view, &kat), FRAME_ERR_ABI_MISMATCH);
}

TEST(MpbParser, CorpusEpochRollback) {
    const mpb_corpus_entry_t* entry = find_entry("neg_epoch.mpb");
    ASSERT_NE(entry, nullptr);
    std::vector<uint8_t> data = load_entry(*entry);
    mpb_view_t view{};
    KatCrypto kat{};
    EXPECT_EQ(parse_with(*entry, data, default_policy(), &view, &kat), FRAME_ERR_EPOCH_ROLLBACK);
}

TEST(MpbParser, DoubleFaultReportsSignatureBeforeHash) {
    const mpb_corpus_entry_t* entry = find_entry("neg_double_fault.mpb");
    ASSERT_NE(entry, nullptr);
    std::vector<uint8_t> data = load_entry(*entry);
    mpb_view_t view{};
    KatCrypto kat{};
    EXPECT_EQ(parse_with(*entry, data, default_policy(), &view, &kat), FRAME_ERR_SIGNATURE_INVALID);
    EXPECT_EQ(kat.sha_calls, 1); /* stopped after the signing digest */
}

TEST(MpbParser, MaxPackageBytesPolicyRejectsOversize) {
    const mpb_corpus_entry_t* entry = find_entry("pos_code.mpb");
    ASSERT_NE(entry, nullptr);
    std::vector<uint8_t> data = load_entry(*entry);
    mpb_policy_t policy = default_policy();
    policy.max_package_bytes = static_cast<uint32_t>(data.size() - 1);
    mpb_view_t view{};
    KatCrypto kat{};
    EXPECT_EQ(parse_with(*entry, data, policy, &view, &kat), FRAME_ERR_PACKAGE_INVALID);
}

TEST(MpbParser, EpochFloorEqualStillVerifies) {
    const mpb_corpus_entry_t* entry = find_entry("pos_code.mpb");
    ASSERT_NE(entry, nullptr);
    std::vector<uint8_t> data = load_entry(*entry);
    mpb_policy_t policy = default_policy();
    policy.epoch_floor = 1;
    mpb_view_t view{};
    KatCrypto kat{};
    EXPECT_EQ(parse_with(*entry, data, policy, &view, &kat), FRAME_OK);
    policy.epoch_floor = 2;
    view = mpb_view_t{};
    EXPECT_EQ(parse_with(*entry, data, policy, &view, &kat), FRAME_ERR_EPOCH_ROLLBACK);
}

TEST(MpbParser, SignatureRecordFieldsAreStructural) {
    const mpb_corpus_entry_t* entry = find_entry("pos_code.mpb");
    ASSERT_NE(entry, nullptr);
    std::vector<uint8_t> data = load_entry(*entry);
    const uint32_t sig_offset = static_cast<uint32_t>(
        data[60] | (data[61] << 8) | (data[62] << 16) | ((uint32_t)data[63] << 24));
    std::vector<uint8_t> tampered = data;
    tampered[sig_offset] ^= 0x01; /* algorithm != 1 */
    mpb_view_t view{};
    mpb_crypto_t crypto{};
    KatCrypto kat{};
    kat_crypto_for(*entry, &crypto, &kat);
    mpb_policy_t policy = default_policy();
    EXPECT_EQ(mpb_parse(tampered.data(), tampered.size(), &policy, &crypto, &view),
              FRAME_ERR_PACKAGE_INVALID);
    EXPECT_EQ(kat.verify_calls, 0);
}

TEST(MpbParser, TamperedElfPayloadByteRejectedAsHashMismatch) {
    const mpb_corpus_entry_t* entry = find_entry("pos_code.mpb");
    ASSERT_NE(entry, nullptr);
    std::vector<uint8_t> data = load_entry(*entry);
    mpb_view_t view{};
    KatCrypto kat{};
    ASSERT_EQ(parse_with(*entry, data, default_policy(), &view, &kat), FRAME_OK);
    std::vector<uint8_t> tampered = data;
    tampered[view.payloads[0].offset + view.payloads[0].length / 2] ^= 0x80;
    view = mpb_view_t{};
    EXPECT_EQ(parse_with(*entry, tampered, default_policy(), &view, &kat), FRAME_ERR_HASH_MISMATCH);
}

TEST(MpbParser, TruncationFuzzAlwaysFailsClosed) {
    for (const char* name : {"pos_code.mpb", "pos_resource_only.mpb"}) {
        SCOPED_TRACE(name);
        const mpb_corpus_entry_t* entry = find_entry(name);
        ASSERT_NE(entry, nullptr);
        std::vector<uint8_t> data = load_entry(*entry);
        mpb_policy_t policy = default_policy();
        for (size_t cut = 0; cut < data.size(); ++cut) {
            mpb_view_t view{};
            KatCrypto kat{};
            std::vector<uint8_t> truncated(data.begin(), data.begin() + cut);
            mpb_crypto_t crypto{};
            kat_crypto_for(*entry, &crypto, &kat);
            frame_err_t err =
                mpb_parse(truncated.data(), truncated.size(), &policy, &crypto, &view);
            EXPECT_NE(err, FRAME_OK) << "truncation to " << cut << " accepted";
            EXPECT_EQ(view.staging, nullptr);
        }
    }
}

TEST(MpbParser, RandomTruncationsFailClosed) {
    const mpb_corpus_entry_t* entry = find_entry("pos_code.mpb");
    ASSERT_NE(entry, nullptr);
    std::vector<uint8_t> data = load_entry(*entry);
    uint32_t state = 0x12345678u;
    auto next = [&state](uint32_t bound) {
        state = state * 1664525u + 1013904223u;
        return state % bound;
    };
    mpb_policy_t policy = default_policy();
    for (int i = 0; i < 1000; ++i) {
        size_t cut = next(static_cast<uint32_t>(data.size()));
        mpb_view_t view{};
        mpb_crypto_t crypto{};
        KatCrypto kat{};
        kat_crypto_for(*entry, &crypto, &kat);
        std::vector<uint8_t> truncated(data.begin(), data.begin() + cut);
        frame_err_t err = mpb_parse(truncated.data(), truncated.size(), &policy, &crypto, &view);
        ASSERT_NE(err, FRAME_OK) << "random truncation " << cut << " accepted";
        ASSERT_EQ(view.staging, nullptr);
    }
}

TEST(MpbParser, ByteFlipFuzzNeverCrashes) {
    for (const char* name : {"pos_code.mpb", "pos_resource_only.mpb"}) {
        SCOPED_TRACE(name);
        const mpb_corpus_entry_t* entry = find_entry(name);
        ASSERT_NE(entry, nullptr);
        std::vector<uint8_t> data = load_entry(*entry);
        uint32_t state = 0xC0FFEEu;
        auto next = [&state](uint32_t bound) {
            state = state * 1664525u + 1013904223u;
            return state % bound;
        };
        mpb_policy_t policy = default_policy();
        for (int i = 0; i < 1000; ++i) {
            size_t offset = next(static_cast<uint32_t>(data.size()));
            uint8_t bit = static_cast<uint8_t>(1u << (next(8)));
            std::vector<uint8_t> flipped = data;
            flipped[offset] ^= bit;
            mpb_view_t view{};
            mpb_crypto_t crypto{};
            KatCrypto kat{};
            kat_crypto_for(*entry, &crypto, &kat);
            frame_err_t err = mpb_parse(flipped.data(), flipped.size(), &policy, &crypto, &view);
            if (err == FRAME_OK) {
                /* accepted only when the flip hit unauthenticated padding */
                ASSERT_EQ(view.package_kind, entry->package_kind);
                ASSERT_EQ(view.staging, flipped.data());
                ASSERT_LT(view.payloads[0].offset, flipped.size());
            } else {
                ASSERT_EQ(view.staging, nullptr);
            }
        }
    }
}

} // namespace
