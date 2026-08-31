#include "frame_poc/frame_strand.hh"

#include <utility>

namespace frame::poc {

io_core::io_core(std::size_t max_strands)
    : capacity_(max_strands == 0 ? 1 : max_strands), ring_(capacity_, nullptr) {}

io_core::~io_core() { stop(); }

void io_core::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_ || stopping_) {
        return;
    }
    started_ = true;
    // Worker launched outside the lock; run_loop only touches members via
    // pop_ready/push_ready which take the lock themselves.
    worker_ = std::thread([this] { run_loop(); });
}

void io_core::stop() {
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            return;
        }
        stopping_ = true;
        worker = std::move(worker_);
    }
    not_empty_.notify_all();
    not_full_.notify_all();
    if (worker.joinable()) {
        worker.join();
    }
}

void io_core::push_ready(strand& target) {
    std::unique_lock<std::mutex> lock(mutex_);
    // Duplicate detector: the max-once invariant (§4.1.3) says a strand is
    // in the ring at most once. A second entry is a protocol fault.
    for (std::size_t offset = 0; offset < count_; ++offset) {
        if (ring_[(head_ + offset) % capacity_] == &target) {
            duplicate_fault_.store(true, std::memory_order_relaxed);
            return;
        }
    }
    not_full_.wait(lock, [this] { return count_ < capacity_ || stopping_; });
    if (count_ >= capacity_) {
        return; // ring full during shutdown; drop (worker is exiting anyway)
    }
    ring_[(head_ + count_) % capacity_] = &target;
    ++count_;
    not_empty_.notify_one();
}

strand* io_core::pop_ready() {
    std::unique_lock<std::mutex> lock(mutex_);
    not_empty_.wait(lock, [this] { return count_ > 0 || stopping_; });
    if (count_ == 0) {
        return nullptr;
    }
    strand* target = ring_[head_];
    ring_[head_] = nullptr;
    head_ = (head_ + 1) % capacity_;
    --count_;
    not_full_.notify_one();
    return target;
}

void io_core::run_loop() {
    while (strand* target = pop_ready()) {
        target->run_one();
    }
}

strand::strand(io_core& core, std::size_t op_capacity, std::chrono::microseconds handler_budget)
    : core_(core), capacity_(op_capacity), handler_budget_(handler_budget) {
    // Construction-time preallocation: zero hot-path allocation (§4.1.2).
    pool_.resize(capacity_);
    for (std::size_t index = 0; index < capacity_; ++index) {
        pool_[index].pooled = true;
        push_freelist_locked(&pool_[index]);
    }
}

strand::~strand() {
    // Caller contract: quiesce and drain (or stop the core) before destruction.
}

frame_err_t strand::post(frame_post_operation_t operation) {
    bool need_push = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closing_) {
            return FRAME_ERR_PLUGIN_STOPPING; // caller keeps ownership
        }
        if (outstanding_ >= capacity_) {
            return FRAME_ERR_QUEUE_FULL; // caller keeps ownership; destroy NOT called
        }
        strand_op_node* node = pop_freelist_locked();
        if (node == nullptr) {
            return FRAME_ERR_QUEUE_FULL;
        }
        node->kind = op_kind::post;
        node->seq = ++ticket_;
        node->generation = generation_.load(std::memory_order_relaxed);
        node->post = operation;
        push_fifo_locked(node);
        ++outstanding_;
        // IDLE -> SCHEDULED conversion happens under the same lock as the
        // enqueue (anti-lost-wakeup protocol, §4.1.3). The CAS mirrors the
        // FreeRTOS critical-section sequence used by the T13 port; under the
        // host mutex it cannot observe a concurrent state.
        strand_state expected = strand_state::idle;
        if (state_.compare_exchange_strong(expected, strand_state::scheduled,
                                           std::memory_order_relaxed)) {
            ready_pending_ = true;
            need_push = true;
        }
    }
    if (need_push) {
        core_.push_ready(*this);
    }
    return FRAME_OK;
}

void strand::begin_quiescing() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closing_) {
        return; // idempotent
    }
    closing_ = true;
    // §4.1.6 step 5 / LIFE-007: convert queued un-run POSTs in place;
    // FIFO position preserved so destroys stay on-strand and ordered.
    for (strand_op_node* node = fifo_head_; node != nullptr; node = node->next) {
        if (node->kind == op_kind::post) {
            node->kind = op_kind::cleanup;
        }
    }
    if (!ready_pending_ && fifo_head_ == nullptr) {
        state_.store(strand_state::stopped, std::memory_order_relaxed);
    } else {
        state_.store(strand_state::closing, std::memory_order_relaxed);
    }
}

void strand::post_completion(strand_op_node& reserved) {
    bool need_push = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        push_fifo_locked(&reserved);
        if (!ready_pending_) {
            // Allowed during closing/stopped (§4.1.4): cancel results must
            // still be delivered. The closing_ flag, not the observable
            // state, drives the terminal transition in run_one.
            ready_pending_ = true;
            need_push = true;
            if (state_.load(std::memory_order_relaxed) == strand_state::idle) {
                state_.store(strand_state::scheduled, std::memory_order_relaxed);
            }
        }
    }
    if (need_push) {
        core_.push_ready(*this);
    }
}

void strand::run_one() {
    strand_op_node* node = nullptr;
    bool requeue = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ready_pending_ = false; // worker owns the strand while not requeued
        state_.store(strand_state::running, std::memory_order_relaxed);
        node = pop_fifo_locked();
        if (fifo_head_ != nullptr) {
            // Still non-empty: round-robin fairness (§4.1.3) — one operation
            // per ready visit, then back to the ring tail.
            ready_pending_ = true;
            requeue = true;
            state_.store(closing_ ? strand_state::closing : strand_state::scheduled,
                         std::memory_order_relaxed);
        } else {
            // Empty-check and IDLE/STOPPED store are atomic with respect to
            // concurrent post() under this mutex: no lost wakeup is possible.
            state_.store(closing_ ? strand_state::stopped : strand_state::idle,
                         std::memory_order_relaxed);
        }
    }
    if (requeue) {
        core_.push_ready(*this);
    }
    if (node != nullptr) {
        execute_node(node);
    }
}

void strand::execute_node(strand_op_node* node) noexcept {
    // Generation check before any user code (LIFE-007): stale callbacks are
    // skipped but still destroyed exactly once.
    const bool fresh = node->generation == generation_.load(std::memory_order_acquire);
    switch (node->kind) {
    case op_kind::post:
        if (fresh) {
            const auto begin = std::chrono::steady_clock::now();
            node->post.invoke(node->post.user);
            const auto elapsed = std::chrono::steady_clock::now() - begin;
            if (elapsed > handler_budget_) {
                overruns_.fetch_add(1, std::memory_order_seq_cst);
            }
            // seq_cst: publishes handler-side writes to polling test threads.
            invoked_.fetch_add(1, std::memory_order_seq_cst);
        } else {
            stale_dropped_.fetch_add(1, std::memory_order_seq_cst);
        }
        node->post.destroy(node->post.user);
        destroyed_.fetch_add(1, std::memory_order_seq_cst);
        break;
    case op_kind::cleanup:
        // Converted POST: skip invoke, destroy exactly once on-strand.
        node->post.destroy(node->post.user);
        destroyed_.fetch_add(1, std::memory_order_seq_cst);
        break;
    case op_kind::completion:
        if (fresh) {
            node->completion.complete(node->completion.user, node->completion_err);
        } else {
            stale_dropped_.fetch_add(1, std::memory_order_seq_cst);
        }
        node->completion.destroy(node->completion.user);
        destroyed_.fetch_add(1, std::memory_order_seq_cst);
        break;
    }
    if (node->pooled) {
        std::lock_guard<std::mutex> lock(mutex_);
        push_freelist_locked(node);
        --outstanding_;
    }
}

strand_op_node* strand::pop_fifo_locked() {
    strand_op_node* node = fifo_head_;
    if (node == nullptr) {
        return nullptr;
    }
    fifo_head_ = node->next;
    if (fifo_head_ == nullptr) {
        fifo_tail_ = nullptr;
    }
    node->next = nullptr;
    return node;
}

void strand::push_fifo_locked(strand_op_node* node) {
    node->next = nullptr;
    if (fifo_tail_ == nullptr) {
        fifo_head_ = node;
    } else {
        fifo_tail_->next = node;
    }
    fifo_tail_ = node;
}

strand_op_node* strand::pop_freelist_locked() {
    strand_op_node* node = free_head_;
    if (node != nullptr) {
        free_head_ = node->next;
        node->next = nullptr;
    }
    return node;
}

void strand::push_freelist_locked(strand_op_node* node) {
    node->next = free_head_;
    free_head_ = node;
}

} // namespace frame::poc
