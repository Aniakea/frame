#ifndef FRAME_POC_FRAME_STRAND_HH
#define FRAME_POC_FRAME_STRAND_HH

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "frame_poc/frame_abi.h"

namespace frame::poc {

// Scheduling protocol mapping (project.md §4.1.1-4.1.3):
//
//   post(operation)
//     -> operation enters target strand FIFO (linearization = strand mutex)
//     -> strand IDLE -> SCHEDULED (CAS under the same lock)
//     -> strand pointer enters the io_core ready ring (max-once invariant:
//        a strand is in the ready ring only between a successful
//        IDLE->SCHEDULED conversion and the worker's run_one, therefore a
//        strand can appear in the ring at most once)
//
//   io_core.run_loop()
//     -> pop a ready strand
//     -> run_one(): pop ONE node; FIFO still non-empty -> requeue at ring
//        tail; FIFO empty -> IDLE (or STOPPED when closing)
//     -> execute the popped node OUTSIDE the strand lock
//
// Anti-lost-wakeup protocol (§4.1.3): the FIFO empty-check and the IDLE
// store happen under the same strand mutex as enqueue + IDLE->SCHEDULED
// CAS. A post that acquires the lock before run_one's check sees a
// non-empty FIFO and keeps the strand scheduled; a post that acquires it
// after sees IDLE and schedules the strand itself. Either way a non-empty
// FIFO is always matched by a pending or upcoming ready-ring entry.
//
// FreeRTOS port note (T13): the strand mutex maps to a critical section
// (taskENTER_CRITICAL/taskEXIT_CRITICAL) around the identical FIFO +
// state-transition sequence; the ready ring maps to a bounded queue of
// strand handles; run_loop maps to the per-core worker task. The protocol
// itself is unchanged.
enum class strand_state : uint8_t { idle, scheduled, running, closing, stopped };

enum class op_kind : uint8_t { post, cleanup, completion };

// Intrusive FIFO node. Pool nodes (kind post) come from the strand's
// preallocated array; completion nodes (kind completion) are embedded in
// caller-owned storage and never touch the pool.
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

// One worker thread (host stand-in for one fixed-core FreeRTOS worker, §4.1.1).
// Preallocated fixed-capacity ring of strand pointers; capacity equals the
// maximum number of strands, so a legal SCHEDULED transition never blocks.
class io_core {
  public:
    explicit io_core(std::size_t max_strands);
    ~io_core();

    io_core(const io_core&) = delete;
    io_core& operator=(const io_core&) = delete;

    // Spawns the worker thread. push_ready may be called before start();
    // queued strands are drained once started.
    void start();
    // Idempotent. Wakes the worker, drains nothing, joins the thread.
    void stop();

    bool duplicate_detected() const noexcept {
        return duplicate_fault_.load(std::memory_order_relaxed);
    }

  private:
    friend class strand;

    void push_ready(strand& target);
    strand* pop_ready();
    void run_loop();

    const std::size_t capacity_;
    std::vector<strand*> ring_;
    std::size_t head_ = 0;
    std::size_t count_ = 0;
    std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    bool stopping_ = false;
    std::thread worker_;
    bool started_ = false;
    // Fault flag set under mutex_ when a strand is pushed while already
    // enqueued (protocol violation detector; tests assert it stays clear).
    std::atomic<bool> duplicate_fault_{false};
};

// Logical serial executor (§4.1.2): one FIFO, preallocated pool nodes,
// bounded outstanding-operations credit, exactly-once invoke/destroy.
class strand {
  public:
    strand(io_core& core, std::size_t op_capacity,
           std::chrono::microseconds handler_budget = std::chrono::milliseconds{1});
    ~strand();

    strand(const strand&) = delete;
    strand& operator=(const strand&) = delete;

    // Async post only (LIFE-002: no dispatch/defer). On FRAME_OK the strand
    // owns `operation` and will call invoke then destroy exactly once. On
    // FRAME_ERR_QUEUE_FULL the credit is exhausted and the caller keeps
    // ownership (destroy is NOT called). On FRAME_ERR_PLUGIN_STOPPING the
    // strand is closing/stopped and the caller keeps ownership.
    frame_err_t post(frame_post_operation_t operation);

    // §4.1.6 step 5 / LIFE-007: close the post entry, convert every queued
    // un-run POST in place (kind -> cleanup, FIFO position preserved).
    // When the FIFO drains in closing the state becomes stopped (poll via
    // stopped()). Idempotent.
    void begin_quiescing();

    // Internal completion-delivery path (§4.1.4): bypasses credit and pool
    // (caller-owned reserved node), allowed during closing (delivers cancel
    // results), cannot fail. `reserved.completion` / `reserved.completion_err`
    // must already be filled; the node is executed as kind completion:
    // complete(user, err) then destroy(user), exactly once each.
    void post_completion(strand_op_node& reserved);

    // Executes one popped node; called by io_core's worker.
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
    const std::size_t capacity_;
    const std::chrono::microseconds handler_budget_;

    std::mutex mutex_;
    strand_op_node* fifo_head_ = nullptr;
    strand_op_node* fifo_tail_ = nullptr;
    strand_op_node* free_head_ = nullptr;
    std::vector<strand_op_node> pool_;
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

} // namespace frame::poc

#endif
