#ifndef FRAME_POC_FRAME_HANDLE_HH
#define FRAME_POC_FRAME_HANDLE_HH

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "frame_poc/frame_abi.h"

namespace frame::poc {

struct handle_owner {
    plugin_instance_handle_t instance;
    uint64_t generation;
};

enum class handle_type : uint16_t {
    invalid = 0,
    buffer = 1,
    timer,
    managed_task,
    resource,
    storage_request,
    network_request,
};

template <typename Value, size_t Capacity> class handle_table {
    static_assert(Capacity > 0);
    static_assert(Capacity <= std::numeric_limits<uint32_t>::max());

  public:
    frame_err_t acquire(handle_owner owner, handle_type type, Value value, uint64_t* out_handle) {
        if (out_handle == nullptr || owner.instance == 0 || owner.generation == 0) {
            return FRAME_ERR_INVALID_ARGUMENT;
        }

        for (size_t slot = 0; slot < entries_.size(); ++slot) {
            entry& candidate = entries_[slot];
            if (candidate.live) {
                continue;
            }
            if (candidate.retired) {
                continue;
            }

            candidate.live = true;
            candidate.owner = owner;
            candidate.type = type;
            candidate.value = value;
            *out_handle = encode(slot, candidate.epoch);
            return FRAME_OK;
        }
        return FRAME_ERR_CAPACITY;
    }

    frame_err_t get(uint64_t handle, handle_owner owner, handle_type type, Value** out_value) {
        if (out_value == nullptr) {
            return FRAME_ERR_INVALID_PTR;
        }

        entry* candidate = find(handle);
        if (candidate == nullptr || candidate->owner.instance != owner.instance ||
            candidate->owner.generation != owner.generation || candidate->type != type) {
            return FRAME_ERR_INVALID_PTR;
        }

        *out_value = &candidate->value;
        return FRAME_OK;
    }

    frame_err_t release(uint64_t handle, handle_owner owner, handle_type type) {
        entry* candidate = find(handle);
        if (candidate == nullptr || candidate->owner.instance != owner.instance ||
            candidate->owner.generation != owner.generation || candidate->type != type) {
            return FRAME_ERR_INVALID_PTR;
        }

        candidate->live = false;
        candidate->owner = {};
        if (candidate->epoch == std::numeric_limits<uint32_t>::max()) {
            candidate->retired = true;
        } else {
            ++candidate->epoch;
        }
        return FRAME_OK;
    }

  private:
    struct entry {
        handle_owner owner{};
        handle_type type{handle_type::invalid};
        Value value{};
        uint32_t epoch{1};
        bool live{false};
        bool retired{false};
    };

    static uint64_t encode(size_t slot, uint32_t epoch) {
        return (static_cast<uint64_t>(epoch) << 32) | (static_cast<uint64_t>(slot) + 1);
    }

    entry* find(uint64_t handle) {
        const uint32_t encoded_slot = static_cast<uint32_t>(handle);
        const uint32_t epoch = static_cast<uint32_t>(handle >> 32);
        if (encoded_slot == 0 || epoch == 0) {
            return nullptr;
        }

        const size_t slot = encoded_slot - 1;
        if (slot >= entries_.size()) {
            return nullptr;
        }

        entry& candidate = entries_[slot];
        return candidate.live && candidate.epoch == epoch ? &candidate : nullptr;
    }

    std::array<entry, Capacity> entries_{};
};

} // namespace frame::poc

#endif
