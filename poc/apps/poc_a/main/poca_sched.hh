#ifndef POC_A_MAIN_POCA_SCHED_HH
#define POC_A_MAIN_POCA_SCHED_HH

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "frame_poc/frame_abi.h"

// FreeRTOS realization of the host strand/completion scheduling protocol.
//
// Derived from poc/host/include/frame_poc/frame_strand.hh and
// poc/host/include/frame_poc/frame_completion.hh @2a37556,
// protocol-identical; drift checked by the on-target vector suite
// (`poca sched`, poca_sched.cc) which re-runs the host assertions.
//
// Port deltas (all mechanical, none protocol-visible; the T11/T12 notepad
// thread-mapping note is the authority this realizes):
//   * std::mutex (strand, io_core) -> portENTER_CRITICAL(&portMUX_TYPE)
//     cross-core spinlock. The scheduling protocol tasks (producers,
//     workers) are pinned across both cores on this SMP target, so the
//     host mutex's cross-thread exclusion needs a spinlock, not a
//     local-interrupt critical section.
//   * io_core std::thread worker -> one FreeRTOS task pinned to one core,
//     priority 4 per project.md 4.1.1 (host header pins this mapping).
//   * io_core ring + condition variables -> one FreeRTOS bounded queue of
//     strand pointers (capacity = max strands, so a legal SCHEDULED
//     transition never blocks) plus an in-ring shadow bitmap under the
//     io_core mux: the host duplicate detector scans its ring vector;
//     queue internals are not public API, so the shadow provides the same
//     max-once fault detection with identical set/clear points.
//   * std::chrono::steady_clock handler budget -> esp_timer_get_time()
//     microseconds.
//   * std::thread::id -> TaskHandle_t (worker identity assertions).
//   * std::vector node pool -> caller-provided static arena (strand ctor
//     takes the pool pointer; zero dynamic allocation anywhere on the
//     scheduling path, 4.1.2).
// Everything else - the strand FIFO, the IDLE/SCHEDULED CAS-under-lock
// sequence, the ready_pending_ invariant, the anti-lost-wakeup protocol,
// exactly-once invoke/destroy, the completion PENDING/COMPLETED/CANCELLED
// CAS, the closing_-driven terminal transitions - is line-for-line the
// host protocol.

namespace frame::poc {

enum class strand_state : uint8_t { idle, scheduled, running, closing, stopped };

enum class op_kind : uint8_t { post, cleanup, completion };

// Intrusive FIFO node (host shape, verbatim). Pool nodes (kind post) come
// from the caller-provided preallocated arena; completion nodes (kind
// completion) are embedded in caller-owned storage and never touch the pool.
struct strand_op_node {
    strand_op_node* next = nullptr;
    op_kind kind = op_kind::post;
    bool pooled = false;
    uint64_t seq = 0;
    uint64_t generation = 0;
    frame_post_operation_t post{};
    frame_completion_operation_t completion{};
    frame_err_t completion_err = FRAME_OK;
};

class strand;

// One worker task pinned to one fixed core (host io_core, 4.1.1). The
// ready ring is a bounded queue with capacity equal to the maximum number
// of strands, so a legal SCHEDULED transition never blocks the poster.
class io_core {
  public:
    static constexpr std::size_t kMaxStrandsPerCore = 4;
    static constexpr UBaseType_t kWorkerPrio = 4;
    static constexpr uint32_t kWorkerStackBytes = 8192;

    explicit io_core(std::size_t max_strands, BaseType_t core_id);
    ~io_core();

    io_core(const io_core&) = delete;
    io_core& operator=(const io_core&) = delete;

    // Creates and pins the worker task. push_ready may be called before
    // start(); queued strands are drained once started (host semantics).
    void start();
    // Idempotent. Enqueues the stop sentinel, waits for the worker to exit,
    // deletes the task handle and the queue. Drains nothing.
    void stop();

    bool duplicate_detected() const noexcept {
        return duplicate_fault_.load(std::memory_order_relaxed);
    }
    bool queue_fault() const noexcept { return queue_fault_.load(std::memory_order_relaxed); }
    TaskHandle_t worker_handle() const noexcept { return worker_; }
    BaseType_t core() const noexcept { return core_id_; }

  private:
    friend class strand;

    void push_ready(strand& target);
    static void run_trampoline(void* arg);
    void run_loop();
    std::size_t acquire_slot();
    void release_slot(std::size_t slot);

    const std::size_t capacity_;
    const BaseType_t core_id_;
    QueueHandle_t queue_ = nullptr;
    portMUX_TYPE mux_{};
    bool slot_used_[kMaxStrandsPerCore] = {};
    bool in_ring_[kMaxStrandsPerCore] = {}; // ring-membership shadow (see header deltas)
    std::atomic<bool> duplicate_fault_{false};
    std::atomic<bool> queue_fault_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> started_{false};
    std::atomic<bool> worker_done_{false};
    TaskHandle_t worker_ = nullptr;
};

// Logical serial executor (host strand, 4.1.2): one FIFO, preallocated
// pool nodes (caller arena), bounded outstanding-operations credit,
// exactly-once invoke/destroy.
class strand {
  public:
    strand(io_core& core, strand_op_node* pool, std::size_t op_capacity,
           uint64_t handler_budget_us = 1000);
    ~strand();

    strand(const strand&) = delete;
    strand& operator=(const strand&) = delete;

    // Async post only (LIFE-002: no dispatch/defer). On FRAME_OK the strand
    // owns `operation` and will call invoke then destroy exactly once. On
    // FRAME_ERR_QUEUE_FULL the credit is exhausted and the caller keeps
    // ownership (destroy is NOT called). On FRAME_ERR_PLUGIN_STOPPING the
    // strand is closing/stopped and the caller keeps ownership.
    frame_err_t post(frame_post_operation_t operation);

    // 4.1.6 step 5 / LIFE-007: close the post entry, convert every queued
    // un-run POST in place (kind -> cleanup, FIFO position preserved).
    // When the FIFO drains in closing the state becomes stopped (poll via
    // stopped()). Idempotent.
    void begin_quiescing();

    // Internal completion-delivery path (4.1.4): bypasses credit and pool
    // (caller-owned reserved node), allowed during closing (delivers cancel
    // results), cannot fail. `reserved.completion` /
    // `reserved.completion_err` must already be filled; the node is executed
    // as kind completion: complete(user, err) then destroy(user), exactly
    // once each.
    void post_completion(strand_op_node& reserved);

    // Executes one popped node; called by the io_core worker.
    void run_one();

    bool stopped() const noexcept {
        return state_.load(std::memory_order_acquire) == strand_state::stopped;
    }
    strand_state state() const noexcept { return state_.load(std::memory_order_acquire); }

    uint64_t generation() const noexcept { return generation_.load(std::memory_order_acquire); }
    uint64_t advance_generation() noexcept {
        return generation_.fetch_add(1, std::memory_order_acq_rel);
    }

    // Instrumentation (LIFE-002 handler budget, LIFE-007 stale drop).
    uint64_t invoked_count() const noexcept { return invoked_.load(std::memory_order_acquire); }
    uint64_t destroyed_count() const noexcept { return destroyed_.load(std::memory_order_acquire); }
    uint64_t handler_overruns() const noexcept { return overruns_.load(std::memory_order_acquire); }
    uint64_t stale_generation_dropped() const noexcept {
        return stale_dropped_.load(std::memory_order_acquire);
    }

  private:
    friend class io_core;

    strand_op_node* pop_fifo_locked();
    void push_fifo_locked(strand_op_node* node);
    strand_op_node* pop_freelist_locked();
    void push_freelist_locked(strand_op_node* node);
    void execute_node(strand_op_node* node) noexcept;

    io_core& core_;
    std::size_t slot_ = 0; // io_core ring-membership shadow slot
    const std::size_t capacity_;
    const uint64_t handler_budget_us_;

    portMUX_TYPE mutex_{};
    strand_op_node* fifo_head_ = nullptr;
    strand_op_node* fifo_tail_ = nullptr;
    strand_op_node* free_head_ = nullptr;
    strand_op_node* pool_ = nullptr; // caller-owned arena, not freed here
    std::size_t outstanding_ = 0;
    uint64_t ticket_ = 0;
    bool ready_pending_ = false; // in ready ring (or push in flight); guarded by mutex_
    bool closing_ = false;       // quiescing flag; guarded by mutex_

    std::atomic<strand_state> state_{strand_state::idle};
    std::atomic<uint64_t> generation_{0};
    std::atomic<uint64_t> invoked_{0};
    std::atomic<uint64_t> destroyed_{0};
    std::atomic<uint64_t> overruns_{0};
    std::atomic<uint64_t> stale_dropped_{0};
};

// Host frame_completion.hh @2a37556, verbatim: only std::atomic members,
// fully portable to the target toolchain.
//
// 4.1.4 / LIFE-005: every async operation resolves through one atomic
// terminal-state CAS. The winner (underlying completion vs cancel) decides
// the delivered error; the loser performs no delivery (a late result only
// releases underlying resources).
enum class completion_state : uint8_t { pending, completed, cancelled };

class completion_node {
  public:
    completion_node() = default;

    completion_node(const completion_node&) = delete;
    completion_node& operator=(const completion_node&) = delete;

    // Owner-task only. Returns false if the node is already reserved
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

    // Owner-task only, after the previous reservation's delivery has been
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
    strand_op_node delivery_{}; // embedded delivery node (4.1.2)
};

} // namespace frame::poc

namespace frame::poca {

// `poca sched <test>` (poca_sched.cc): runs one on-target scheduling
// semantics vector, or `all`. Each vector re-runs a host-suite assertion
// subset on the FreeRTOS realization and prints a [PASS-<vector>] marker
// driven by programmatic counter/continuity checks.
int cmd_poca_sched(const char* test);

} // namespace frame::poca

#endif
