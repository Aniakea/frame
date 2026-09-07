#ifndef FRAME_POC_FRAME_COMPLETION_HH
#define FRAME_POC_FRAME_COMPLETION_HH

#include <atomic>
#include <cstdint>

#include "frame_poc/frame_abi.h"
#include "frame_poc/frame_strand.hh"

namespace frame::poc {

// §4.1.4 / LIFE-005: every async operation resolves through one atomic
// terminal-state CAS. The winner (underlying completion vs cancel) decides
// the delivered error; the loser performs no delivery (a late result only
// releases underlying resources).
enum class completion_state : uint8_t { pending, completed, cancelled };

// Pre-allocated completion slot with pool semantics: the owner reserves
// BEFORE starting the underlying operation; a failed reserve means nothing
// was started and the caller keeps ownership. The delivery node is embedded
// (no allocation on the completion path, §4.1.2).
class completion_node {
  public:
    completion_node() = default;

    completion_node(const completion_node&) = delete;
    completion_node& operator=(const completion_node&) = delete;

    // Owner-thread only. Returns false if the node is already reserved
    // (in flight). On true the node is PENDING and bound to
    // (target, generation, op).
    bool reserve(strand& target, uint64_t generation, frame_completion_operation_t op) {
        if (in_flight_.exchange(true, std::memory_order_acq_rel)) {
            return false; // already used: service returns QUEUE_FULL, nothing started
        }
        target_ = &target;
        generation_ = generation;
        op_ = op;
        delivery_ = strand_op_node{};
        delivery_.kind = op_kind::completion;
        delivery_.pooled = false;
        delivery_.generation = generation;
        delivery_.completion = op;
        state_.store(completion_state::pending, std::memory_order_release);
        return true;
    }

    // Owner-thread only, after the previous reservation's delivery has been
    // observed (pool recycle). Never called on an in-flight node.
    void recycle() {
        state_.store(completion_state::pending, std::memory_order_relaxed);
        in_flight_.store(false, std::memory_order_release);
    }

    // Underlying-operation completion. Winner CAS (PENDING -> COMPLETED)
    // delivers `err` on the target strand and returns true; a late result
    // after CANCELLED releases nothing here and returns false.
    bool complete(frame_err_t err) {
        completion_state expected = completion_state::pending;
        if (!state_.compare_exchange_strong(expected, completion_state::completed,
                                            std::memory_order_acq_rel)) {
            return false;
        }
        delivery_.completion_err = err; // COMPLETED first wins keeps the original result
        target_->post_completion(delivery_);
        return true;
    }

    // Cancellation. Winner CAS (PENDING -> CANCELLED) delivers
    // FRAME_ERR_CANCELLED on the target strand and returns true.
    bool cancel() {
        completion_state expected = completion_state::pending;
        if (!state_.compare_exchange_strong(expected, completion_state::cancelled,
                                            std::memory_order_acq_rel)) {
            return false;
        }
        delivery_.completion_err = FRAME_ERR_CANCELLED;
        target_->post_completion(delivery_);
        return true;
    }

    completion_state state() const noexcept { return state_.load(std::memory_order_acquire); }

  private:
    std::atomic<completion_state> state_{completion_state::pending};
    std::atomic<bool> in_flight_{false};
    strand* target_ = nullptr;
    uint64_t generation_ = 0;
    frame_completion_operation_t op_{};
    strand_op_node delivery_{}; // embedded delivery node (§4.1.2)
};

} // namespace frame::poc

#endif
