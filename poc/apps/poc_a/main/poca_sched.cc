#include "poca_sched.hh"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>

#include "esp_timer.h"
#include "poca_plugin.hh"

namespace frame::poc {

// ---------------------------------------------------------------------------
// io_core: FreeRTOS realization of poc/host/src/frame_strand.cc @2a37556.
// The ring is a bounded queue (capacity = max strands; a legal SCHEDULED
// transition never blocks); the in_ring_ shadow under the same mux provides
// the host ring-scan duplicate detector (see header delta note).
// ---------------------------------------------------------------------------

io_core::io_core(std::size_t max_strands, BaseType_t core_id)
    : capacity_(max_strands == 0
                    ? 1
                    : (max_strands > kMaxStrandsPerCore ? kMaxStrandsPerCore : max_strands)),
      core_id_(core_id) {
    mux_ = portMUX_INITIALIZER_UNLOCKED;
    queue_ = xQueueCreate(static_cast<UBaseType_t>(capacity_), sizeof(strand*));
}

io_core::~io_core() { stop(); }

void io_core::start() {
    if (started_.load(std::memory_order_acquire) || stopping_.load(std::memory_order_acquire)) {
        return;
    }
    started_.store(true, std::memory_order_release);
    const char* const name = core_id_ == 0 ? "poca_io0" : "poca_io1";
    if (xTaskCreatePinnedToCore(&io_core::run_trampoline, name,
                                static_cast<uint32_t>(kWorkerStackBytes) / sizeof(StackType_t),
                                this, tskIDLE_PRIORITY + kWorkerPrio, &worker_,
                                core_id_) != pdPASS) {
        worker_ = nullptr;
        queue_fault_.store(true, std::memory_order_relaxed);
    }
}

void io_core::stop() {
    if (stopping_.exchange(true, std::memory_order_acq_rel)) {
        return; // idempotent
    }
    if (worker_ != nullptr) {
        // Worker exits when stopping_ is observed with the queue drained
        // (drain-then-exit, matching host pop_ready semantics), then signals
        // worker_done_ and self-deletes.
        const int64_t deadline = esp_timer_get_time() + 2000000;
        while (!worker_done_.load(std::memory_order_acquire) && esp_timer_get_time() < deadline) {
            vTaskDelay(1);
        }
        worker_ = nullptr;
    }
    if (queue_ != nullptr) {
        vQueueDelete(queue_);
        queue_ = nullptr;
    }
}

void io_core::push_ready(strand& target) {
    if (stopping_.load(std::memory_order_acquire)) {
        return;
    }
    portENTER_CRITICAL(&mux_);
    if (stopping_.load(std::memory_order_relaxed)) {
        portEXIT_CRITICAL(&mux_);
        return;
    }
    // Duplicate detector: the max-once invariant (4.1.3) says a strand is
    // in the ring at most once. A second entry is a protocol fault.
    if (target.slot_ >= capacity_ || in_ring_[target.slot_]) {
        duplicate_fault_.store(true, std::memory_order_relaxed);
        portEXIT_CRITICAL(&mux_);
        return;
    }
    in_ring_[target.slot_] = true;
    portEXIT_CRITICAL(&mux_);
    strand* item = &target;
    if (xQueueSend(queue_, &item, 0) != pdTRUE) {
        // Capacity equals the strand bound, so this is an illegal state.
        queue_fault_.store(true, std::memory_order_relaxed);
        portENTER_CRITICAL(&mux_);
        in_ring_[target.slot_] = false;
        portEXIT_CRITICAL(&mux_);
    }
}

void io_core::run_trampoline(void* arg) { static_cast<io_core*>(arg)->run_loop(); }

void io_core::run_loop() {
    for (;;) {
        strand* target = nullptr;
        if (xQueueReceive(queue_, &target, pdMS_TO_TICKS(100)) == pdTRUE) {
            portENTER_CRITICAL(&mux_);
            if (target->slot_ < capacity_) {
                in_ring_[target->slot_] = false;
            }
            portEXIT_CRITICAL(&mux_);
            target->run_one();
        } else if (stopping_.load(std::memory_order_acquire)) {
            break;
        }
    }
    worker_done_.store(true, std::memory_order_release);
    vTaskDelete(nullptr);
}

std::size_t io_core::acquire_slot() {
    portENTER_CRITICAL(&mux_);
    for (std::size_t index = 0; index < capacity_; ++index) {
        if (!slot_used_[index]) {
            slot_used_[index] = true;
            portEXIT_CRITICAL(&mux_);
            return index;
        }
    }
    portEXIT_CRITICAL(&mux_);
    return static_cast<std::size_t>(-1);
}

void io_core::release_slot(std::size_t slot) {
    if (slot >= capacity_) {
        return;
    }
    portENTER_CRITICAL(&mux_);
    slot_used_[slot] = false;
    portEXIT_CRITICAL(&mux_);
}

// ---------------------------------------------------------------------------
// strand: line-for-line the host protocol; std::mutex -> portMUX spinlock
// (posters and worker may live on different cores), steady_clock budget ->
// esp_timer microseconds. See header deltas.
// ---------------------------------------------------------------------------

strand::strand(io_core& core, strand_op_node* pool, std::size_t op_capacity,
               uint64_t handler_budget_us)
    : core_(core), capacity_(op_capacity == 0 ? 1 : op_capacity),
      handler_budget_us_(handler_budget_us), pool_(pool) {
    mutex_ = portMUX_INITIALIZER_UNLOCKED;
    slot_ = core_.acquire_slot();
    portENTER_CRITICAL(&mutex_);
    // Caller-provided arena preallocation: zero hot-path allocation (4.1.2).
    for (std::size_t index = 0; index < capacity_; ++index) {
        pool_[index].pooled = true;
        pool_[index].next = nullptr;
        push_freelist_locked(&pool_[index]);
    }
    portEXIT_CRITICAL(&mutex_);
}

strand::~strand() { core_.release_slot(slot_); }

frame_err_t strand::post(frame_post_operation_t operation) {
    bool need_push = false;
    portENTER_CRITICAL(&mutex_);
    if (closing_) {
        portEXIT_CRITICAL(&mutex_);
        return FRAME_ERR_PLUGIN_STOPPING; // caller keeps ownership
    }
    if (outstanding_ >= capacity_) {
        portEXIT_CRITICAL(&mutex_);
        return FRAME_ERR_QUEUE_FULL; // caller keeps ownership; destroy NOT called
    }
    strand_op_node* node = pop_freelist_locked();
    if (node == nullptr) {
        portEXIT_CRITICAL(&mutex_);
        return FRAME_ERR_QUEUE_FULL;
    }
    node->kind = op_kind::post;
    node->seq = ++ticket_;
    node->generation = generation_.load(std::memory_order_relaxed);
    node->post = operation;
    push_fifo_locked(node);
    ++outstanding_;
    // IDLE -> SCHEDULED conversion happens under the same lock as the
    // enqueue (anti-lost-wakeup protocol, 4.1.3).
    strand_state expected = strand_state::idle;
    if (state_.compare_exchange_strong(expected, strand_state::scheduled,
                                       std::memory_order_relaxed)) {
        ready_pending_ = true;
        need_push = true;
    }
    portEXIT_CRITICAL(&mutex_);
    if (need_push) {
        core_.push_ready(*this);
    }
    return FRAME_OK;
}

void strand::begin_quiescing() {
    portENTER_CRITICAL(&mutex_);
    if (closing_) {
        portEXIT_CRITICAL(&mutex_);
        return; // idempotent
    }
    closing_ = true;
    // 4.1.6 step 5 / LIFE-007: convert queued un-run POSTs in place;
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
    portEXIT_CRITICAL(&mutex_);
}

void strand::post_completion(strand_op_node& reserved) {
    bool need_push = false;
    portENTER_CRITICAL(&mutex_);
    push_fifo_locked(&reserved);
    if (!ready_pending_) {
        // Allowed during closing/stopped (4.1.4): cancel results must
        // still be delivered. The closing_ flag, not the observable
        // state, drives the terminal transition in run_one.
        ready_pending_ = true;
        need_push = true;
        if (state_.load(std::memory_order_relaxed) == strand_state::idle) {
            state_.store(strand_state::scheduled, std::memory_order_relaxed);
        }
    }
    portEXIT_CRITICAL(&mutex_);
    if (need_push) {
        core_.push_ready(*this);
    }
}

void strand::run_one() {
    strand_op_node* node = nullptr;
    bool requeue = false;
    portENTER_CRITICAL(&mutex_);
    ready_pending_ = false; // worker owns the strand while not requeued
    state_.store(strand_state::running, std::memory_order_relaxed);
    node = pop_fifo_locked();
    if (fifo_head_ != nullptr) {
        // Still non-empty: round-robin fairness (4.1.3) - one operation
        // per ready visit, then back to the ring tail.
        ready_pending_ = true;
        requeue = true;
        state_.store(closing_ ? strand_state::closing : strand_state::scheduled,
                     std::memory_order_relaxed);
    } else {
        // Empty-check and IDLE/STOPPED store are atomic with respect to
        // concurrent post() under this lock: no lost wakeup is possible.
        state_.store(closing_ ? strand_state::stopped : strand_state::idle,
                     std::memory_order_relaxed);
    }
    portEXIT_CRITICAL(&mutex_);
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
            const int64_t begin = esp_timer_get_time();
            node->post.invoke(node->post.user);
            const int64_t elapsed = esp_timer_get_time() - begin;
            if (static_cast<uint64_t>(elapsed) > handler_budget_us_) {
                overruns_.fetch_add(1, std::memory_order_seq_cst);
            }
            // seq_cst: publishes handler-side writes to polling tasks.
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
        portENTER_CRITICAL(&mutex_);
        push_freelist_locked(node);
        --outstanding_;
        portEXIT_CRITICAL(&mutex_);
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

// ---------------------------------------------------------------------------
// On-target vector suite (`poca sched <test>`): re-runs the T11/T12 host
// assertion subset on the FreeRTOS realization. All state is static (the
// host lesson: op.user payloads must outlive queued nodes; and no dynamic
// allocation anywhere). Every verdict is a programmatic counter/continuity
// check; [PASS-<vector>] markers are emitted by this code only after the
// asserts hold (misleading_success guard).
// ---------------------------------------------------------------------------

namespace frame::poca {
namespace {

using frame::poc::completion_node;
using frame::poc::io_core;
using frame::poc::strand;
using frame::poc::strand_op_node;

// Shared node arena (max simultaneous need: noconc = 2 strands x 256).
constexpr std::size_t kNodeArena = 512;
strand_op_node g_nodes[kNodeArena];

constexpr UBaseType_t kHelperPrio = 3; // below the 4.1.1 worker prio 4
constexpr uint32_t kHelperStackBytes = 3072;

template <typename Pred> bool wait_until(Pred predicate, int64_t timeout_ms) {
    const int64_t deadline = esp_timer_get_time() + timeout_ms * 1000;
    while (!predicate()) {
        if (esp_timer_get_time() > deadline) {
            return false;
        }
        vTaskDelay(1);
    }
    return true;
}

void noop_destroy(void*) {}

// Generic producer: posts `op` until accepted `count` times, retrying on
// QUEUE_FULL (the documented caller contract) and yielding periodically so
// the IDLE tasks keep being scheduled (TWDT).
struct producer_ctx {
    strand* target = nullptr;
    uint32_t count = 0;
    uint32_t yield_every = 0;
    std::atomic<uint32_t> accepted{0};
    std::atomic<bool> done{false};
    frame_post_operation_t op{};
};

void producer_task(void* arg) {
    auto* ctx = static_cast<producer_ctx*>(arg);
    uint32_t retries = 0;
    for (uint32_t index = 0; index < ctx->count; ++index) {
        while (ctx->target->post(ctx->op) == FRAME_ERR_QUEUE_FULL) {
            taskYIELD();
            if (++retries % 64 == 0) {
                vTaskDelay(1);
            }
        }
        ctx->accepted.fetch_add(1, std::memory_order_relaxed);
        if (ctx->yield_every != 0 && (index % ctx->yield_every) == ctx->yield_every - 1) {
            vTaskDelay(1);
        }
    }
    ctx->done.store(true, std::memory_order_release);
    vTaskDelete(nullptr);
}

TaskHandle_t spawn_producer(producer_ctx& ctx, const char* name, BaseType_t core) {
    TaskHandle_t handle = nullptr;
    if (xTaskCreatePinnedToCore(&producer_task, name,
                                static_cast<uint32_t>(kHelperStackBytes) / sizeof(StackType_t),
                                &ctx, kHelperPrio, &handle, core) != pdPASS) {
        return nullptr;
    }
    return handle;
}

bool producers_clean(const producer_ctx* ctxs, uint32_t count) {
    for (uint32_t index = 0; index < count; ++index) {
        if (!ctxs[index].done.load(std::memory_order_acquire) ||
            ctxs[index].accepted.load(std::memory_order_acquire) != ctxs[index].count) {
            return false;
        }
    }
    return true;
}

// ---- vector: fifo ---------------------------------------------------------
// Host StrictFifoOrderAcrossProducers: 3 producer tasks x 5000 posts; the
// handler checks per-producer ticket continuity incrementally (worker is
// the single writer, so this equals the host post-hoc vector check). Host
// pool credit is 31000 (never full); target pool is 256 with the documented
// QUEUE_FULL retry (host storm vector's own caller contract) - the ticket
// is assigned at accept time, so continuity is unaffected.
constexpr int kFifoProducers = 3;
constexpr uint32_t kFifoPerProducer = 5000;
constexpr uint32_t kFifoTotal = kFifoProducers * kFifoPerProducer;
// cell = (producer << 14) | ticket: identifies the exact posted operation,
// exactly like the host ticket_payload.
uint16_t g_fifo_cells[kFifoProducers][kFifoPerProducer];
struct fifo_rec {
    std::atomic<uint32_t> next_expected{0};
    std::atomic<uint32_t> observed{0};
    std::atomic<uint32_t> order_faults{0};
};
fifo_rec g_fifo_recs[kFifoProducers];
struct fifo_prod_ctx {
    strand* target = nullptr;
    int producer = 0;
    std::atomic<bool> done{false};
};
fifo_prod_ctx g_fifo_ctxs[kFifoProducers];

void fifo_invoke(void* user) {
    const uint16_t cell = *static_cast<uint16_t*>(user);
    const uint32_t producer = static_cast<uint32_t>(cell >> 14);
    const uint32_t ticket = static_cast<uint32_t>(cell & 0x3FFFu);
    fifo_rec& rec = g_fifo_recs[producer];
    if (ticket != rec.next_expected.load(std::memory_order_relaxed)) {
        rec.order_faults.fetch_add(1, std::memory_order_relaxed);
    }
    rec.next_expected.store(ticket + 1, std::memory_order_relaxed);
    rec.observed.fetch_add(1, std::memory_order_relaxed);
}

void fifo_producer_task(void* arg) {
    auto* ctx = static_cast<fifo_prod_ctx*>(arg);
    for (uint32_t ticket = 0; ticket < kFifoPerProducer; ++ticket) {
        frame_post_operation_t op{};
        op.user = &g_fifo_cells[ctx->producer][ticket];
        op.invoke = &fifo_invoke;
        op.destroy = &noop_destroy;
        uint32_t retries = 0;
        while (ctx->target->post(op) == FRAME_ERR_QUEUE_FULL) {
            taskYIELD();
            if (++retries % 64 == 0) {
                vTaskDelay(1);
            }
        }
        if ((ticket % 256u) == 255u) {
            vTaskDelay(1);
        }
    }
    ctx->done.store(true, std::memory_order_release);
    vTaskDelete(nullptr);
}

int vector_fifo() {
    for (int producer = 0; producer < kFifoProducers; ++producer) {
        g_fifo_recs[producer].next_expected.store(0);
        g_fifo_recs[producer].observed.store(0);
        g_fifo_recs[producer].order_faults.store(0);
        for (uint32_t ticket = 0; ticket < kFifoPerProducer; ++ticket) {
            g_fifo_cells[producer][ticket] =
                static_cast<uint16_t>((static_cast<uint32_t>(producer) << 14) | ticket);
        }
    }
    io_core core(1, 0);
    strand s(core, g_nodes, 256);
    core.start();

    static const BaseType_t pins[kFifoProducers] = {0, 1, 0}; // cross-core mix
    for (int producer = 0; producer < kFifoProducers; ++producer) {
        g_fifo_ctxs[producer].target = &s;
        g_fifo_ctxs[producer].producer = producer;
        g_fifo_ctxs[producer].done.store(false);
        char name[16];
        std::snprintf(name, sizeof(name), "poca_ff%d", producer);
        if (xTaskCreatePinnedToCore(&fifo_producer_task, name,
                                    static_cast<uint32_t>(kHelperStackBytes) / sizeof(StackType_t),
                                    &g_fifo_ctxs[producer], kHelperPrio, nullptr,
                                    pins[producer]) != pdPASS) {
            std::printf("[sched-fifo] FAIL producer spawn %d\n", producer);
            core.stop();
            return 1;
        }
    }
    bool ok = wait_until(
        [&] {
            for (int producer = 0; producer < kFifoProducers; ++producer) {
                if (!g_fifo_ctxs[producer].done.load(std::memory_order_acquire)) {
                    return false;
                }
            }
            return true;
        },
        30000);
    ok = ok && wait_until([&] { return s.destroyed_count() == kFifoTotal; }, 30000);

    uint32_t order_faults = 0;
    bool observed_ok = true;
    for (int producer = 0; producer < kFifoProducers; ++producer) {
        order_faults += g_fifo_recs[producer].order_faults.load();
        observed_ok = observed_ok && g_fifo_recs[producer].observed.load() == kFifoPerProducer &&
                      g_fifo_recs[producer].next_expected.load() == kFifoPerProducer;
    }
    const uint64_t invoked = s.invoked_count();
    const bool counters_ok = invoked == kFifoTotal && s.destroyed_count() == kFifoTotal;
    const bool faults_ok = !core.duplicate_detected() && !core.queue_fault();
    core.stop();

    std::printf("[sched-fifo] producers=%d per=%u invoked=%" PRIu64 " destroyed=%" PRIu64
                " order_faults=%u dup=%d qfault=%d\n",
                kFifoProducers, static_cast<unsigned>(kFifoPerProducer), invoked,
                s.destroyed_count(), static_cast<unsigned>(order_faults),
                core.duplicate_detected() ? 1 : 0, core.queue_fault() ? 1 : 0);
    if (!ok || !observed_ok || order_faults != 0 || !counters_ok || !faults_ok) {
        std::printf("[FAIL-fifo] ok=%d observed=%d counters=%d faults=%d\n", ok ? 1 : 0,
                    observed_ok ? 1 : 0, counters_ok ? 1 : 0, faults_ok ? 1 : 0);
        return 1;
    }
    std::printf("[PASS-fifo]\n");
    return 0;
}

// ---- vector: fair ---------------------------------------------------------
// Round-robin fairness (4.1.3, plan T13 "轮转公平"): two strands, four ops
// each, posted A1 A2 B1 B2 before the worker starts; run_one executes ONE
// op per ready visit and requeues at the tail, so the global execution
// order must alternate strictly A,B,A,B,A,B,A,B. (The host suite covers
// this path implicitly via the storm drain; the target suite asserts it
// explicitly.)
struct fair_cell {
    uint8_t tag;
    uint8_t seq;
};
fair_cell g_fair_cells[8];
uint8_t g_fair_log[8];
uint8_t g_fair_next[2] = {0, 0};
std::atomic<uint32_t> g_fair_log_len{0};
std::atomic<uint32_t> g_fair_seq_faults{0};

void fair_invoke(void* user) {
    const fair_cell cell = *static_cast<fair_cell*>(user);
    g_fair_log[g_fair_log_len.fetch_add(1, std::memory_order_relaxed)] = cell.tag;
    if (cell.seq != g_fair_next[cell.tag]) {
        g_fair_seq_faults.fetch_add(1, std::memory_order_relaxed);
    }
    ++g_fair_next[cell.tag];
}

int vector_fair() {
    g_fair_log_len.store(0);
    g_fair_seq_faults.store(0);
    g_fair_next[0] = 0;
    g_fair_next[1] = 0;
    io_core core(2, 0);
    alignas(strand) uint8_t storage[2][sizeof(strand)];
    auto* strands = new (storage[0]) strand(core, g_nodes, 8);
    auto* strands_b = new (storage[1]) strand(core, g_nodes + 8, 8);
    strand* const strands_by_tag[2] = {strands, strands_b};
    for (uint8_t seq = 0; seq < 4; ++seq) {
        for (uint8_t tag = 0; tag < 2; ++tag) {
            g_fair_cells[tag * 4 + seq] = fair_cell{tag, static_cast<uint8_t>(seq)};
            frame_post_operation_t op{};
            op.user = &g_fair_cells[tag * 4 + seq];
            op.invoke = &fair_invoke;
            op.destroy = &noop_destroy;
            if (strands_by_tag[tag]->post(op) != FRAME_OK) {
                std::printf("[FAIL-fair] post rejected pre-start\n");
                strands->~strand();
                strands_b->~strand();
                core.stop();
                return 1;
            }
        }
    }
    core.start();
    const bool drained = wait_until(
        [&] { return strands->destroyed_count() + strands_b->destroyed_count() == 8; }, 10000);
    const uint32_t len = g_fair_log_len.load();
    bool alternation = len == 8;
    for (uint32_t index = 0; index < len && index < 8; ++index) {
        alternation = alternation && g_fair_log[index] == static_cast<uint8_t>(index % 2);
    }
    const bool counts_ok = strands->invoked_count() == 4 && strands_b->invoked_count() == 4;
    const bool faults_ok = !core.duplicate_detected() && !core.queue_fault();
    strands->~strand();
    strands_b->~strand();
    core.stop();

    std::printf("[sched-fair] order=");
    for (uint32_t index = 0; index < len && index < 8; ++index) {
        std::printf("%c", g_fair_log[index] == 0 ? 'A' : 'B');
    }
    std::printf(" seq_faults=%u drained=%d\n", static_cast<unsigned>(g_fair_seq_faults.load()),
                drained ? 1 : 0);
    if (!drained || !alternation || g_fair_seq_faults.load() != 0 || !counts_ok || !faults_ok) {
        std::printf("[FAIL-fair] alternation=%d counts=%d faults=%d\n", alternation ? 1 : 0,
                    counts_ok ? 1 : 0, faults_ok ? 1 : 0);
        return 1;
    }
    std::printf("[PASS-fair]\n");
    return 0;
}

// ---- vector: noconc -------------------------------------------------------
// Host SameStrandHandlersNeverConcurrent on the 4.1.1 dual-core topology:
// strand 0 bound to a core-0 worker, strand 1 to a core-1 worker; each
// strand's producers are pinned to the OPPOSITE core (cross-core posting
// stress on the portMUX protocol). Asserts: the in-flight CAS guard never
// trips (no same-strand concurrency) and every strand's handlers ran on
// exactly one task identity - its own bound worker.
struct concurrency_guard {
    std::atomic<bool> in_flight[2] = {false, false};
    std::atomic<bool> fault{false};
    std::atomic<bool> id_fault{false};
    TaskHandle_t runner[2] = {nullptr, nullptr};
};
concurrency_guard g_noconc_guard;
struct slot_payload {
    concurrency_guard* guard;
    int slot;
};
slot_payload g_noconc_payloads[2];
producer_ctx g_noconc_ctxs[4];

void noconc_invoke(void* user) {
    auto* item = static_cast<slot_payload*>(user);
    concurrency_guard* shared = item->guard;
    const int slot = item->slot;
    bool expected = false;
    if (!shared->in_flight[slot].compare_exchange_strong(expected, true)) {
        shared->fault.store(true, std::memory_order_relaxed);
    }
    const TaskHandle_t current = xTaskGetCurrentTaskHandle();
    if (shared->runner[slot] == nullptr) {
        shared->runner[slot] = current;
    } else if (shared->runner[slot] != current) {
        shared->id_fault.store(true, std::memory_order_relaxed);
    }
    for (int64_t begin = esp_timer_get_time(); esp_timer_get_time() - begin < 20;) {
        // 20us hold, mirroring the host handler nap
    }
    shared->in_flight[slot].store(false, std::memory_order_relaxed);
}

int vector_noconc() {
    g_noconc_guard.in_flight[0].store(false);
    g_noconc_guard.in_flight[1].store(false);
    g_noconc_guard.fault.store(false);
    g_noconc_guard.id_fault.store(false);
    g_noconc_guard.runner[0] = nullptr;
    g_noconc_guard.runner[1] = nullptr;
    for (int slot = 0; slot < 2; ++slot) {
        g_noconc_payloads[slot] = slot_payload{&g_noconc_guard, slot};
    }
    io_core core0(1, 0);
    io_core core1(1, 1);
    alignas(strand) uint8_t storage[2][sizeof(strand)];
    auto* strand0 = new (storage[0]) strand(core0, g_nodes, 256);
    auto* strand1 = new (storage[1]) strand(core1, g_nodes + 256, 256);
    for (int index = 0; index < 4; ++index) {
        const int slot = index % 2;
        g_noconc_ctxs[index].target = slot == 0 ? strand0 : strand1;
        g_noconc_ctxs[index].count = 400;
        g_noconc_ctxs[index].yield_every = 64;
        g_noconc_ctxs[index].accepted.store(0);
        g_noconc_ctxs[index].done.store(false);
        g_noconc_ctxs[index].op.user = &g_noconc_payloads[slot];
        g_noconc_ctxs[index].op.invoke = &noconc_invoke;
        g_noconc_ctxs[index].op.destroy = &noop_destroy;
    }
    core0.start();
    core1.start();

    static const BaseType_t pins[4] = {1, 1, 0, 0}; // opposite each strand's worker
    static const char* const names[4] = {"poca_nc0", "poca_nc1", "poca_nc2", "poca_nc3"};
    for (int index = 0; index < 4; ++index) {
        if (spawn_producer(g_noconc_ctxs[index], names[index], pins[index]) == nullptr) {
            std::printf("[FAIL-noconc] producer spawn %d\n", index);
            core0.stop();
            core1.stop();
            return 1;
        }
    }
    const bool drained =
        wait_until([&] { return strand0->destroyed_count() + strand1->destroyed_count() == 1600; },
                   30000) &&
        producers_clean(g_noconc_ctxs, 4);

    const bool guard_ok = !g_noconc_guard.fault.load() && !g_noconc_guard.id_fault.load();
    const bool identity_ok = g_noconc_guard.runner[0] == core0.worker_handle() &&
                             g_noconc_guard.runner[1] == core1.worker_handle();
    const uint64_t invoked0 = strand0->invoked_count();
    const uint64_t invoked1 = strand1->invoked_count();
    const uint64_t destroyed0 = strand0->destroyed_count();
    const uint64_t destroyed1 = strand1->destroyed_count();
    const bool counts_ok =
        invoked0 == 800 && invoked1 == 800 && destroyed0 == 800 && destroyed1 == 800;
    const bool faults_ok = !core0.duplicate_detected() && !core0.queue_fault() &&
                           !core1.duplicate_detected() && !core1.queue_fault();
    strand0->~strand();
    strand1->~strand();
    core0.stop();
    core1.stop();

    std::printf("[sched-noconc] invoked=%" PRIu64 "/%" PRIu64 " destroyed=%" PRIu64 "/%" PRIu64
                " guard_fault=%d id_fault=%d identity=%d\n",
                invoked0, invoked1, destroyed0, destroyed1, g_noconc_guard.fault.load() ? 1 : 0,
                g_noconc_guard.id_fault.load() ? 1 : 0, identity_ok ? 1 : 0);
    if (!drained || !guard_ok || !identity_ok || !counts_ok || !faults_ok) {
        std::printf("[FAIL-noconc] drained=%d guard=%d identity=%d counts=%d faults=%d\n",
                    drained ? 1 : 0, guard_ok ? 1 : 0, identity_ok ? 1 : 0, counts_ok ? 1 : 0,
                    faults_ok ? 1 : 0);
        return 1;
    }
    std::printf("[PASS-noconc]\n");
    return 0;
}

// ---- vector: dupclear -----------------------------------------------------
// Host ReadyQueueNeverDuplicatesStrand: multi-producer stress against one
// strand; the ready-ring duplicate detector must stay clear (max-once
// invariant, 4.1.3). Host: 8 x 5000 = 4e4 posts; target 8 x 1000 = 8e3
// accepted posts (same 8-way concurrency class and thousands of schedule
// transitions; the board wall-clock budget covers the full suite x3 - the
// host run remains the 4e4 long-haul authority).
constexpr int kDupProducers = 8;
constexpr uint32_t kDupPerProducer = 1000;
constexpr uint32_t kDupTotal = kDupProducers * kDupPerProducer;
producer_ctx g_dup_ctxs[kDupProducers];

int vector_dupclear() {
    io_core core(1, 0);
    strand s(core, g_nodes, 256);
    for (int index = 0; index < kDupProducers; ++index) {
        g_dup_ctxs[index].target = &s;
        g_dup_ctxs[index].count = kDupPerProducer;
        g_dup_ctxs[index].yield_every = 128;
        g_dup_ctxs[index].accepted.store(0);
        g_dup_ctxs[index].done.store(false);
        g_dup_ctxs[index].op.user = nullptr;
        g_dup_ctxs[index].op.invoke = [](void*) {};
        g_dup_ctxs[index].op.destroy = &noop_destroy;
    }
    core.start();
    for (int index = 0; index < kDupProducers; ++index) {
        char name[16];
        std::snprintf(name, sizeof(name), "poca_dup%d", index);
        if (spawn_producer(g_dup_ctxs[index], name, static_cast<BaseType_t>(index % 2)) ==
            nullptr) {
            std::printf("[FAIL-dupclear] producer spawn %d\n", index);
            core.stop();
            return 1;
        }
    }
    const bool drained = wait_until([&] { return s.destroyed_count() == kDupTotal; }, 60000) &&
                         producers_clean(g_dup_ctxs, kDupProducers);
    const bool counts_ok = s.invoked_count() == kDupTotal && s.destroyed_count() == kDupTotal;
    const bool faults_ok = !core.duplicate_detected() && !core.queue_fault();
    core.stop();

    std::printf("[sched-dupclear] producers=%d per=%u invoked=%" PRIu64 " destroyed=%" PRIu64
                " dup=%d qfault=%d\n",
                kDupProducers, static_cast<unsigned>(kDupPerProducer), s.invoked_count(),
                s.destroyed_count(), core.duplicate_detected() ? 1 : 0, core.queue_fault() ? 1 : 0);
    if (!drained || !counts_ok || !faults_ok) {
        std::printf("[FAIL-dupclear] drained=%d counts=%d faults=%d\n", drained ? 1 : 0,
                    counts_ok ? 1 : 0, faults_ok ? 1 : 0);
        return 1;
    }
    std::printf("[PASS-dupclear]\n");
    return 0;
}

// ---- vector: storm --------------------------------------------------------
// Host NoLostWakeupUnderProducerStorm: tiny credit window (64) so producers
// fight the drain; a lost wakeup would strand FIFO nodes forever. Host
// total 1e5; target 2e4 (4 producers x 5000 fixed): the race window is the
// 64-deep credit, not the total; 2e4 keeps the full x3 board rerun inside
// the wall-clock budget while exercising the same window ~312 times over.
constexpr int kStormProducers = 4;
constexpr uint32_t kStormPerProducer = 5000;
constexpr uint32_t kStormTotal = kStormProducers * kStormPerProducer;
producer_ctx g_storm_ctxs[kStormProducers];

int vector_storm() {
    io_core core(1, 0);
    strand s(core, g_nodes, 64);
    for (int index = 0; index < kStormProducers; ++index) {
        g_storm_ctxs[index].target = &s;
        g_storm_ctxs[index].count = kStormPerProducer;
        g_storm_ctxs[index].yield_every = 256;
        g_storm_ctxs[index].accepted.store(0);
        g_storm_ctxs[index].done.store(false);
        g_storm_ctxs[index].op.user = nullptr;
        g_storm_ctxs[index].op.invoke = [](void*) {};
        g_storm_ctxs[index].op.destroy = &noop_destroy;
    }
    core.start();
    for (int index = 0; index < kStormProducers; ++index) {
        char name[16];
        std::snprintf(name, sizeof(name), "poca_st%d", index);
        if (spawn_producer(g_storm_ctxs[index], name, static_cast<BaseType_t>(index % 2)) ==
            nullptr) {
            std::printf("[FAIL-storm] producer spawn %d\n", index);
            core.stop();
            return 1;
        }
    }
    const bool drained = wait_until([&] { return s.destroyed_count() == kStormTotal; }, 60000) &&
                         producers_clean(g_storm_ctxs, kStormProducers);
    s.begin_quiescing();
    const bool stopped = wait_until([&] { return s.stopped(); }, 10000);
    const bool counts_ok = s.invoked_count() == kStormTotal && s.destroyed_count() == kStormTotal;
    const bool stale_ok = s.stale_generation_dropped() == 0;
    const bool faults_ok = !core.duplicate_detected() && !core.queue_fault();
    core.stop();

    std::printf("[sched-storm] total=%u invoked=%" PRIu64 " destroyed=%" PRIu64 " stale=%" PRIu64
                " stopped=%d dup=%d\n",
                static_cast<unsigned>(kStormTotal), s.invoked_count(), s.destroyed_count(),
                s.stale_generation_dropped(), stopped ? 1 : 0, core.duplicate_detected() ? 1 : 0);
    if (!drained || !stopped || !counts_ok || !stale_ok || !faults_ok) {
        std::printf("[FAIL-storm] drained=%d stopped=%d counts=%d stale=%d faults=%d\n",
                    drained ? 1 : 0, stopped ? 1 : 0, counts_ok ? 1 : 0, stale_ok ? 1 : 0,
                    faults_ok ? 1 : 0);
        return 1;
    }
    std::printf("[PASS-storm]\n");
    return 0;
}

// ---- vector: qfull --------------------------------------------------------
// Host QueueFullRejectKeepsCallerOwnership: credit exhaustion returns
// FRAME_ERR_QUEUE_FULL and the caller keeps ownership (destroy NOT called
// on reject, 4.1.2). Posts happen before the worker starts.
std::atomic<bool> g_qfull_destroyed[8];
std::atomic<bool> g_qfull_reject_destroyed{false};

int vector_qfull() {
    io_core core(1, 0);
    strand s(core, g_nodes, 8);
    for (int index = 0; index < 8; ++index) {
        g_qfull_destroyed[index].store(false);
    }
    g_qfull_reject_destroyed.store(false);
    for (int index = 0; index < 8; ++index) {
        frame_post_operation_t op{};
        op.user = &g_qfull_destroyed[index];
        op.invoke = [](void*) {};
        op.destroy = [](void* user) { static_cast<std::atomic<bool>*>(user)->store(true); };
        if (s.post(op) != FRAME_OK) {
            std::printf("[FAIL-qfull] post %d rejected before fill\n", index);
            core.stop();
            return 1;
        }
    }
    frame_post_operation_t overflow{};
    overflow.user = &g_qfull_reject_destroyed;
    overflow.invoke = [](void*) {};
    overflow.destroy = [](void* user) { static_cast<std::atomic<bool>*>(user)->store(true); };
    const frame_err_t rejected = s.post(overflow);
    const bool reject_ok = rejected == FRAME_ERR_QUEUE_FULL && !g_qfull_reject_destroyed.load();

    core.start();
    const bool drained = wait_until([&] { return s.destroyed_count() == 8; }, 10000);
    bool flags_ok = !g_qfull_reject_destroyed.load();
    for (int index = 0; index < 8; ++index) {
        flags_ok = flags_ok && g_qfull_destroyed[index].load();
    }
    const bool counts_ok = s.invoked_count() == 8;
    core.stop();

    std::printf("[sched-qfull] reject_err=%d reject_destroyed=%d invoked=%" PRIu64 " flags=%d\n",
                static_cast<int>(rejected), g_qfull_reject_destroyed.load() ? 1 : 0,
                s.invoked_count(), flags_ok ? 1 : 0);
    if (!reject_ok || !drained || !flags_ok || !counts_ok) {
        std::printf("[FAIL-qfull] reject=%d drained=%d flags=%d counts=%d\n", reject_ok ? 1 : 0,
                    drained ? 1 : 0, flags_ok ? 1 : 0, counts_ok ? 1 : 0);
        return 1;
    }
    std::printf("[PASS-qfull]\n");
    return 0;
}

// ---- vector: completion ---------------------------------------------------
// Host CompleteVsCancelRaceExactlyOnce: two spinner tasks + atomic round
// gate. The host spin/yield gate maps to direct task notifications with
// the same semantics (round value r releases act r only; shutdown releases
// via the shutdown flag and NEVER a gate bump - the T11 lesson that a
// bumped gate unlocks one extra act on a recycled node). Host 1e5 rounds;
// target 2e4 (board wall-clock; the exactly-once CAS window is per round,
// the reserve on the console core still crosses cores each round, and the
// alternating release order exercises both CAS outcomes at full count -
// stronger than the host's lucky scheduler split).
constexpr uint32_t kCompRounds = 20000;
constexpr std::size_t kCompPool = 64;
completion_node g_comp_nodes[kCompPool];
struct comp_rec {
    std::atomic<uint64_t> complete_delivered{0};
    std::atomic<uint64_t> cancel_delivered{0};
    std::atomic<uint64_t> invalid_delivered{0};
    std::atomic<uint64_t> destroyed{0};

    uint64_t deliveries() const {
        return complete_delivered.load(std::memory_order_acquire) +
               cancel_delivered.load(std::memory_order_acquire);
    }
};
comp_rec g_comp_rec;
std::atomic<bool> g_comp_shutdown{false};
struct spinner_ctx {
    int slot = 0;
    std::atomic<uint64_t> done{0};
    TaskHandle_t handle = nullptr;
};
spinner_ctx g_comp_spinners[2];

void comp_complete_cb(void* user, frame_err_t err) {
    auto* rec = static_cast<comp_rec*>(user);
    if (err == FRAME_ERR_CANCELLED) {
        rec->cancel_delivered.fetch_add(1, std::memory_order_seq_cst);
    } else if (err == FRAME_ERR_TIMEOUT) {
        rec->complete_delivered.fetch_add(1, std::memory_order_seq_cst);
    } else {
        rec->invalid_delivered.fetch_add(1, std::memory_order_seq_cst);
    }
}

void comp_destroy_cb(void* user) {
    static_cast<comp_rec*>(user)->destroyed.fetch_add(1, std::memory_order_seq_cst);
}

void comp_spinner_task(void* arg) {
    auto* ctx = static_cast<spinner_ctx*>(arg);
    uint64_t next = 1;
    while (!g_comp_shutdown.load(std::memory_order_acquire)) {
        uint32_t value = 0;
        if (xTaskNotifyWait(0, 0, &value, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (g_comp_shutdown.load(std::memory_order_acquire)) {
            break;
        }
        if (static_cast<uint64_t>(value) < next) {
            continue;
        }
        completion_node& node = g_comp_nodes[(next - 1) % kCompPool];
        if (ctx->slot == 0) {
            (void)node.complete(FRAME_ERR_TIMEOUT); // loser CAS is legal
        } else {
            (void)node.cancel();
        }
        ctx->done.store(next, std::memory_order_release);
        ++next;
    }
    vTaskDelete(nullptr);
}

int vector_completion() {
    g_comp_rec.complete_delivered.store(0);
    g_comp_rec.cancel_delivered.store(0);
    g_comp_rec.invalid_delivered.store(0);
    g_comp_rec.destroyed.store(0);
    g_comp_shutdown.store(false);
    for (int slot = 0; slot < 2; ++slot) {
        g_comp_spinners[slot].slot = slot;
        g_comp_spinners[slot].done.store(0);
        g_comp_spinners[slot].handle = nullptr;
    }
    io_core core(1, 0);
    strand s(core, g_nodes, 8);
    frame_completion_operation_t op{};
    op.user = &g_comp_rec;
    op.complete = &comp_complete_cb;
    op.destroy = &comp_destroy_cb;

    core.start();
    // Both spinners pinned off the console's core (core 1): wake order then
    // equals notify order, so alternating the release order per round makes
    // BOTH CAS outcomes occur at full count (board wake order is
    // deterministic under pinned priorities, unlike the host's spinning
    // threads - the exactly-once assertions are identical either way).
    static const BaseType_t pins[2] = {0, 0};
    static const char* const names[2] = {"poca_cpw", "poca_cpc"};
    for (int slot = 0; slot < 2; ++slot) {
        if (xTaskCreatePinnedToCore(&comp_spinner_task, names[slot],
                                    static_cast<uint32_t>(kHelperStackBytes) / sizeof(StackType_t),
                                    &g_comp_spinners[slot], kHelperPrio,
                                    &g_comp_spinners[slot].handle, pins[slot]) != pdPASS) {
            std::printf("[FAIL-completion] spinner spawn %d\n", slot);
            g_comp_shutdown.store(true);
            core.stop();
            return 1;
        }
    }

    uint32_t rounds_done = 0;
    uint32_t reserve_failures = 0;
    for (uint32_t round = 0; round < kCompRounds; ++round) {
        completion_node& node = g_comp_nodes[round % kCompPool];
        if (!node.reserve(s, s.generation(), op)) {
            reserve_failures++;
            break;
        }
        const bool cancel_first = (round % 2u) == 1u;
        if (cancel_first) {
            xTaskNotify(g_comp_spinners[1].handle, round + 1, eSetValueWithOverwrite);
            xTaskNotify(g_comp_spinners[0].handle, round + 1, eSetValueWithOverwrite);
        } else {
            xTaskNotify(g_comp_spinners[0].handle, round + 1, eSetValueWithOverwrite);
            xTaskNotify(g_comp_spinners[1].handle, round + 1, eSetValueWithOverwrite);
        }
        const uint64_t target_round = round + 1;
        if (!wait_until(
                [&] {
                    return g_comp_spinners[0].done.load(std::memory_order_acquire) >=
                               target_round &&
                           g_comp_spinners[1].done.load(std::memory_order_acquire) >=
                               target_round &&
                           g_comp_rec.deliveries() == target_round &&
                           g_comp_rec.destroyed.load() == target_round &&
                           s.destroyed_count() == target_round;
                },
                5000)) {
            std::printf("[sched-completion] stalled at round %u\n", static_cast<unsigned>(round));
            break;
        }
        node.recycle();
        rounds_done = round + 1;
        if (rounds_done % 5000 == 0) {
            std::printf("[sched-completion] progress %u/%u\n", static_cast<unsigned>(rounds_done),
                        static_cast<unsigned>(kCompRounds));
        }
    }

    // Release spinners via the shutdown flag only (never a gate bump).
    g_comp_shutdown.store(true, std::memory_order_release);
    xTaskNotify(g_comp_spinners[0].handle, 0, eSetValueWithOverwrite);
    xTaskNotify(g_comp_spinners[1].handle, 0, eSetValueWithOverwrite);
    vTaskDelay(20);

    const bool rounds_ok = rounds_done == kCompRounds && reserve_failures == 0;
    const bool delivered_ok =
        g_comp_rec.deliveries() == kCompRounds &&
        g_comp_rec.complete_delivered.load() + g_comp_rec.cancel_delivered.load() == kCompRounds &&
        g_comp_rec.complete_delivered.load() > 0 && g_comp_rec.cancel_delivered.load() > 0 &&
        g_comp_rec.invalid_delivered.load() == 0;
    const bool destroyed_ok =
        g_comp_rec.destroyed.load() == kCompRounds && s.destroyed_count() == kCompRounds;
    const bool faults_ok = !core.duplicate_detected() && !core.queue_fault();
    core.stop();

    std::printf(
        "[sched-completion] rounds=%u reserve_failures=%u complete=%" PRIu64 " cancel=%" PRIu64
        " invalid=%" PRIu64 " rec_destroyed=%" PRIu64 " strand_destroyed=%" PRIu64 "\n",
        static_cast<unsigned>(rounds_done), static_cast<unsigned>(reserve_failures),
        g_comp_rec.complete_delivered.load(), g_comp_rec.cancel_delivered.load(),
        g_comp_rec.invalid_delivered.load(), g_comp_rec.destroyed.load(), s.destroyed_count());
    if (!rounds_ok || !delivered_ok || !destroyed_ok || !faults_ok) {
        std::printf("[FAIL-completion] rounds=%d delivered=%d destroyed=%d faults=%d\n",
                    rounds_ok ? 1 : 0, delivered_ok ? 1 : 0, destroyed_ok ? 1 : 0,
                    faults_ok ? 1 : 0);
        return 1;
    }
    std::printf("[PASS-completion]\n");
    return 0;
}

// ---- vector: quiesce ------------------------------------------------------
// Host ClosingRejectsPostsAndCleansUp + QuiescingCleanupRunsOnWorkerThread:
// queued un-run posts convert to CLEANUP in place, late posts reject with
// PLUGIN_STOPPING, and every destroy runs on the strand's bound worker task
// (thread-identity assert via TaskHandle_t).
struct quiesce_post {
    std::atomic<bool> invoked{false};
    std::atomic<bool> destroyed{false};
    TaskHandle_t destroy_runner{nullptr}; // written only by the worker
};
quiesce_post g_quiesce_posts[16];

void quiesce_invoke(void* user) { static_cast<quiesce_post*>(user)->invoked.store(true); }
void quiesce_destroy(void* user) {
    auto* post = static_cast<quiesce_post*>(user);
    post->destroy_runner = xTaskGetCurrentTaskHandle();
    post->destroyed.store(true);
}

struct quiesce_late_rec {
    std::atomic<uint64_t> delivered{0};
    std::atomic<uint64_t> destroyed{0};
};
quiesce_late_rec g_quiesce_late;

void quiesce_late_complete(void*, frame_err_t) { g_quiesce_late.delivered.fetch_add(1); }
void quiesce_late_destroy(void*) { g_quiesce_late.destroyed.fetch_add(1); }

int vector_quiesce() {
    for (auto& post : g_quiesce_posts) {
        post.invoked.store(false);
        post.destroyed.store(false);
        post.destroy_runner = nullptr;
    }
    const TaskHandle_t console_task = xTaskGetCurrentTaskHandle();
    io_core core(1, 0);
    strand s(core, g_nodes, 16);
    for (auto& post : g_quiesce_posts) {
        frame_post_operation_t op{};
        op.user = &post;
        op.invoke = &quiesce_invoke;
        op.destroy = &quiesce_destroy;
        if (s.post(op) != FRAME_OK) {
            std::printf("[FAIL-quiesce] post rejected\n");
            core.stop();
            return 1;
        }
    }
    s.begin_quiescing();
    frame_post_operation_t late{};
    late.user = nullptr;
    late.invoke = [](void*) {};
    late.destroy = &noop_destroy;
    const frame_err_t late_err = s.post(late);
    const bool late_ok = late_err == FRAME_ERR_PLUGIN_STOPPING;

    core.start();
    const bool drained =
        wait_until([&] { return s.destroyed_count() == 16 && s.stopped(); }, 10000);
    bool flags_ok = true;
    bool identity_ok = true;
    for (auto& post : g_quiesce_posts) {
        flags_ok = flags_ok && !post.invoked.load() && post.destroyed.load();
        identity_ok = identity_ok && post.destroy_runner == core.worker_handle() &&
                      post.destroy_runner != console_task;
    }
    const bool counts_ok = s.invoked_count() == 0 && s.stale_generation_dropped() == 0;

    // 4.1.4 revival (host ThreeWay property): a completion posted after the
    // strand reached stopped must still be delivered exactly once on the
    // worker - cancel results are not lost by quiesce.
    g_quiesce_late.delivered.store(0);
    g_quiesce_late.destroyed.store(0);
    frame_completion_operation_t late_op{};
    late_op.complete = &quiesce_late_complete;
    late_op.destroy = &quiesce_late_destroy;
    completion_node late_node;
    bool revive_ok = late_node.reserve(s, s.generation(), late_op);
    revive_ok = revive_ok && late_node.complete(FRAME_ERR_CANCELLED);
    const bool revived = wait_until([&] { return g_quiesce_late.destroyed.load() == 1; }, 10000) &&
                         g_quiesce_late.delivered.load() == 1 && s.destroyed_count() == 17;
    core.stop();

    std::printf("[sched-quiesce] late_err=%d invoked=%" PRIu64 " destroyed=%" PRIu64
                " flags=%d identity=%d revived=%d\n",
                static_cast<int>(late_err), s.invoked_count(), s.destroyed_count(),
                flags_ok ? 1 : 0, identity_ok ? 1 : 0, revived ? 1 : 0);
    if (!late_ok || !drained || !flags_ok || !identity_ok || !counts_ok || !revive_ok || !revived) {
        std::printf("[FAIL-quiesce] late=%d drained=%d flags=%d identity=%d counts=%d revive=%d\n",
                    late_ok ? 1 : 0, drained ? 1 : 0, flags_ok ? 1 : 0, identity_ok ? 1 : 0,
                    counts_ok ? 1 : 0, revived ? 1 : 0);
        return 1;
    }
    std::printf("[PASS-quiesce]\n");
    return 0;
}

// ---- vector: stalegen -----------------------------------------------------
// Host StaleGenerationDroppedButDestroyed: stale callbacks are dropped (no
// invoke, no complete) but destroyed exactly once and counted.
struct stale_post {
    std::atomic<bool> invoked{false};
    std::atomic<bool> destroyed{false};
};
stale_post g_stale_posts[4];
std::atomic<uint64_t> g_stale_comp_delivered{0};
std::atomic<uint64_t> g_stale_comp_destroyed{0};

void stale_invoke(void* user) { static_cast<stale_post*>(user)->invoked.store(true); }
void stale_destroy(void* user) { static_cast<stale_post*>(user)->destroyed.store(true); }
void stale_complete_cb(void*, frame_err_t) { g_stale_comp_delivered.fetch_add(1); }
void stale_destroy_cb(void*) { g_stale_comp_destroyed.fetch_add(1); }

int vector_stalegen() {
    for (auto& post : g_stale_posts) {
        post.invoked.store(false);
        post.destroyed.store(false);
    }
    g_stale_comp_delivered.store(0);
    g_stale_comp_destroyed.store(0);
    io_core core(1, 0);
    strand s(core, g_nodes, 8);
    for (auto& post : g_stale_posts) {
        frame_post_operation_t op{};
        op.user = &post;
        op.invoke = &stale_invoke;
        op.destroy = &stale_destroy;
        if (s.post(op) != FRAME_OK) {
            std::printf("[FAIL-stalegen] post rejected\n");
            core.stop();
            return 1;
        }
    }
    const uint64_t stale_generation = s.generation();
    s.advance_generation();
    core.start();
    const bool drained = wait_until([&] { return s.destroyed_count() == 4; }, 10000);
    const bool posts_ok = s.invoked_count() == 0 && s.stale_generation_dropped() == 4;
    bool flags_ok = true;
    for (auto& post : g_stale_posts) {
        flags_ok = flags_ok && !post.invoked.load() && post.destroyed.load();
    }

    completion_node node;
    frame_completion_operation_t cop{};
    cop.user = nullptr;
    cop.complete = &stale_complete_cb;
    cop.destroy = &stale_destroy_cb;
    bool flow_ok = node.reserve(s, stale_generation, cop);
    flow_ok = flow_ok && node.complete(FRAME_ERR_TIMEOUT);
    const bool comp_drained = wait_until([&] { return g_stale_comp_destroyed.load() == 1; }, 10000);
    const bool comp_ok = g_stale_comp_delivered.load() == 0 && // stale: complete() skipped
                         s.stale_generation_dropped() == 5;
    core.stop();

    std::printf("[sched-stalegen] invoked=%" PRIu64 " stale=%" PRIu64 " comp_delivered=%" PRIu64
                " comp_destroyed=%" PRIu64 "\n",
                s.invoked_count(), s.stale_generation_dropped(), g_stale_comp_delivered.load(),
                g_stale_comp_destroyed.load());
    if (!drained || !posts_ok || !flags_ok || !flow_ok || !comp_drained || !comp_ok) {
        std::printf("[FAIL-stalegen] drained=%d posts=%d flags=%d flow=%d comp=%d\n",
                    drained ? 1 : 0, posts_ok ? 1 : 0, flags_ok ? 1 : 0, flow_ok ? 1 : 0,
                    comp_ok ? 1 : 0);
        return 1;
    }
    std::printf("[PASS-stalegen]\n");
    return 0;
}

// ---- vector: loader -------------------------------------------------------
// T13 loader hookup smoke (plan 2 topology: loader -> per-instance strand
// -> entry): load the baseline plugin through the T4 pipeline, then post
// ONE operation onto a strand whose handler calls the loaded plugin's
// activate() - proving plugin-entry-via-strand on the worker task.
struct loader_rec {
    int32_t returned = 0;
    TaskHandle_t runner = nullptr;
};
loader_rec g_loader_rec;

void loader_invoke(void*) {
    using ActivateFn = int32_t (*)(void);
    auto fn = reinterpret_cast<ActivateFn>(const_cast<void*>(poca_active_activate_fn()));
    g_loader_rec.returned = fn();
    g_loader_rec.runner = xTaskGetCurrentTaskHandle();
}

int vector_loader() {
    if (cmd_poca_load("baseline") != 0) {
        std::printf("[FAIL-loader] baseline load failed\n");
        return 1;
    }
    void* const fn = const_cast<void*>(poca_active_activate_fn());
    // The called pointer must be plugin code in the PSRAM exec mirror
    // (0x42.. window, T4 finding: both-window convention).
    const uint32_t fn_addr = reinterpret_cast<uint32_t>(fn);
    if (fn == nullptr || !(fn_addr >= 0x42000000u && fn_addr < 0x44000000u)) {
        std::printf("[FAIL-loader] activate fn 0x%08" PRIx32 " not in PSRAM exec window\n",
                    fn_addr);
        cmd_poca_unload();
        return 1;
    }
    g_loader_rec = loader_rec{};
    const TaskHandle_t console_task = xTaskGetCurrentTaskHandle();
    io_core core(1, 0);
    strand s(core, g_nodes, 8);
    core.start();
    frame_post_operation_t op{};
    op.user = nullptr;
    op.invoke = &loader_invoke;
    op.destroy = &noop_destroy;
    if (s.post(op) != FRAME_OK) {
        std::printf("[FAIL-loader] post rejected\n");
        core.stop();
        cmd_poca_unload();
        return 1;
    }
    const bool drained = wait_until([&] { return s.destroyed_count() == 1; }, 10000);
    const int32_t expected = poca_active_expected_activate();
    const bool value_ok = g_loader_rec.returned == expected;
    const bool identity_ok =
        g_loader_rec.runner == core.worker_handle() && g_loader_rec.runner != console_task;
    core.stop();
    cmd_poca_unload();

    std::printf("[sched-loader] fn=0x%08" PRIx32 " returned=0x%08" PRIx32 " expected=0x%08" PRIx32
                " on_worker=%d\n",
                fn_addr, static_cast<uint32_t>(g_loader_rec.returned),
                static_cast<uint32_t>(expected), identity_ok ? 1 : 0);
    if (!drained || !value_ok || !identity_ok || s.invoked_count() != 1) {
        std::printf("[FAIL-loader] drained=%d value=%d identity=%d\n", drained ? 1 : 0,
                    value_ok ? 1 : 0, identity_ok ? 1 : 0);
        return 1;
    }
    std::printf("[PASS-loader]\n");
    return 0;
}

struct vector_entry {
    const char* name;
    int (*run)();
};

const vector_entry k_vectors[] = {
    {"fifo", &vector_fifo},
    {"fair", &vector_fair},
    {"noconc", &vector_noconc},
    {"dupclear", &vector_dupclear},
    {"storm", &vector_storm},
    {"qfull", &vector_qfull},
    {"completion", &vector_completion},
    {"quiesce", &vector_quiesce},
    {"stalegen", &vector_stalegen},
    {"loader", &vector_loader},
};
constexpr unsigned kVectorCount = sizeof(k_vectors) / sizeof(k_vectors[0]);

} // namespace

int cmd_poca_sched(const char* test) {
    if (test == nullptr) {
        std::printf("usage: poca sched <fifo|fair|noconc|dupclear|storm|qfull|completion|"
                    "quiesce|stalegen|loader|all>\n");
        return 1;
    }
    const bool all = std::strcmp(test, "all") == 0;
    unsigned failures = 0;
    unsigned ran = 0;
    for (unsigned index = 0; index < kVectorCount; ++index) {
        if (!all && std::strcmp(test, k_vectors[index].name) != 0) {
            continue;
        }
        std::printf("[sched] === vector %s ===\n", k_vectors[index].name);
        const int64_t begin = esp_timer_get_time();
        if (k_vectors[index].run() != 0) {
            failures++;
        }
        ran++;
        std::printf("[sched] === vector %s done in %lld ms ===\n", k_vectors[index].name,
                    static_cast<long long>((esp_timer_get_time() - begin) / 1000));
        if (!all) {
            break;
        }
    }
    if (ran == 0) {
        std::printf("[poca-sched] FAIL unknown vector '%s'\n", test);
        std::printf("usage: poca sched <fifo|fair|noconc|dupclear|storm|qfull|completion|"
                    "quiesce|stalegen|loader|all>\n");
        return 1;
    }
    const bool pass = failures == 0;
    std::printf("SCHED SUITE %s vectors=%u pass=%u fail=%u\n", pass ? "PASS" : "FAIL", ran,
                ran - failures, failures);
    return pass ? 0 : 1;
}

} // namespace frame::poca
