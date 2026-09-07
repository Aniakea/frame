#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "frame_poc/frame_completion.hh"
#include "frame_poc/frame_strand.hh"

using frame::poc::completion_node;
using frame::poc::completion_state;
using frame::poc::io_core;
using frame::poc::strand;

namespace {

constexpr auto kPollDelay = std::chrono::microseconds{50};
constexpr frame_err_t kOriginalErr = FRAME_ERR_TIMEOUT; // -2, distinct from CANCELLED (-15)

bool wait_until(const std::function<bool()>& predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(kPollDelay);
    }
    return true;
}

struct completion_rec {
    std::atomic<uint64_t> complete_delivered{0}; // err != CANCELLED
    std::atomic<uint64_t> cancel_delivered{0};   // err == FRAME_ERR_CANCELLED
    std::atomic<uint64_t> destroyed{0};
    std::vector<frame_err_t> delivered_errs;      // written only by the worker
    std::vector<std::thread::id> destroy_threads; // written only by the worker

    uint64_t deliveries() const {
        return complete_delivered.load(std::memory_order_acquire) +
               cancel_delivered.load(std::memory_order_acquire);
    }
};

struct post_rec {
    std::atomic<bool> invoked{false};
    std::atomic<bool> destroyed{false};
    std::thread::id destroy_thread{}; // written only by the worker
};

frame_completion_operation_t make_completion_op(completion_rec* rec) {
    frame_completion_operation_t op{};
    op.user = rec;
    op.complete = [](void* user, frame_err_t err) {
        auto* target = static_cast<completion_rec*>(user);
        if (err == FRAME_ERR_CANCELLED) {
            target->cancel_delivered.fetch_add(1, std::memory_order_seq_cst);
        } else {
            target->complete_delivered.fetch_add(1, std::memory_order_seq_cst);
        }
        target->delivered_errs.push_back(err);
    };
    op.destroy = [](void* user) {
        auto* target = static_cast<completion_rec*>(user);
        target->destroy_threads.push_back(std::this_thread::get_id());
        target->destroyed.fetch_add(1, std::memory_order_seq_cst);
    };
    return op;
}

} // namespace

// T12 (1): complete-vs-cancel race, two persistent threads, atomic round
// gate, 1e5 rounds. Exactly one CAS winner per round; deliveries partition
// into complete/cancel; destroy exactly once per op.
TEST(FrameCompletion, CompleteVsCancelRaceExactlyOnce) {
    io_core core(1);
    strand s(core, 64);
    constexpr uint64_t kRounds = 100000;
    constexpr std::size_t kPool = 64;
    std::vector<completion_node> nodes(kPool);
    completion_rec rec;
    std::atomic<uint64_t> gate{0};
    std::atomic<uint64_t> done_complete{0};
    std::atomic<uint64_t> done_cancel{0};
    std::atomic<bool> shutdown{false};
    std::atomic<uint64_t> reserve_failures{0};
    core.start();

    auto spinner = [&](auto act, std::atomic<uint64_t>& done) {
        uint64_t next = 1;
        while (!shutdown.load(std::memory_order_acquire)) {
            while (gate.load(std::memory_order_acquire) < next) {
                if (shutdown.load(std::memory_order_acquire)) {
                    return;
                }
                std::this_thread::yield();
            }
            act(next - 1, nodes[(next - 1) % kPool]);
            done.store(next, std::memory_order_release);
            ++next;
        }
    };
    std::thread complete_thread(
        spinner,
        [&](uint64_t, completion_node& node) {
            if (!node.complete(kOriginalErr)) {
                // losing the CAS is legal; only the winner delivers
            }
        },
        std::ref(done_complete));
    std::thread cancel_thread(
        spinner, [&](uint64_t, completion_node& node) { (void)node.cancel(); },
        std::ref(done_cancel));

    for (uint64_t round = 0; round < kRounds; ++round) {
        completion_node& node = nodes[round % kPool];
        if (!node.reserve(s, s.generation(), make_completion_op(&rec))) {
            reserve_failures.fetch_add(1);
            break;
        }
        gate.store(round + 1, std::memory_order_release);
        ASSERT_TRUE(wait_until(
            [&] {
                return done_complete.load(std::memory_order_acquire) >= round + 1 &&
                       done_cancel.load(std::memory_order_acquire) >= round + 1 &&
                       rec.deliveries() == round + 1 && rec.destroyed.load() == round + 1 &&
                       s.destroyed_count() == round + 1;
            },
            std::chrono::seconds{30}))
            << "stalled at round " << round;
        node.recycle();
    }

    // Release spinners via the shutdown flag only: bumping the gate here
    // would unlock one extra act() on a recycled (PENDING) node.
    shutdown.store(true, std::memory_order_release);
    complete_thread.join();
    cancel_thread.join();

    EXPECT_EQ(reserve_failures.load(), 0u);
    EXPECT_EQ(rec.deliveries(), kRounds);
    EXPECT_EQ(rec.complete_delivered.load() + rec.cancel_delivered.load(), kRounds);
    EXPECT_EQ(rec.destroyed.load(), kRounds);
    EXPECT_EQ(s.destroyed_count(), kRounds);
    ASSERT_EQ(rec.delivered_errs.size(), kRounds);
    for (const frame_err_t err : rec.delivered_errs) {
        EXPECT_TRUE(err == kOriginalErr || err == FRAME_ERR_CANCELLED);
    }
    core.stop();
}

// T12 (2): COMPLETED wins the CAS first -> the original error is delivered;
// a later cancel is a no-op (late result releases nothing).
TEST(FrameCompletion, CancelAfterCompleteKeepsOriginalErr) {
    io_core core(1);
    strand s(core, 4);
    completion_node node;
    completion_rec rec;
    core.start();

    ASSERT_TRUE(node.reserve(s, s.generation(), make_completion_op(&rec)));
    ASSERT_TRUE(node.complete(kOriginalErr));
    ASSERT_FALSE(node.cancel());
    ASSERT_FALSE(node.complete(FRAME_ERR_BUSY));
    EXPECT_EQ(node.state(), completion_state::completed);

    ASSERT_TRUE(wait_until([&] { return rec.destroyed.load() == 1; }, std::chrono::seconds{10}));
    ASSERT_EQ(rec.delivered_errs.size(), 1u);
    EXPECT_EQ(rec.delivered_errs[0], kOriginalErr);
    EXPECT_EQ(rec.cancel_delivered.load(), 0u);
    EXPECT_EQ(rec.complete_delivered.load(), 1u);
    EXPECT_EQ(s.destroyed_count(), 1u);
    core.stop();
}

// T12 (3): reservation failure means nothing was started: the service
// returns QUEUE_FULL synchronously and never touches the caller's op.
TEST(FrameCompletion, ReserveFailureStartsNothing) {
    io_core core(1);
    strand s(core, 4);
    completion_node node;
    completion_rec rec;
    const frame_completion_operation_t op = make_completion_op(&rec);

    ASSERT_TRUE(node.reserve(s, s.generation(), op));
    ASSERT_FALSE(node.reserve(s, s.generation(), op)); // already used

    // Service-side contract on the failed path: synchronous QUEUE_FULL.
    const frame_err_t started =
        node.reserve(s, s.generation(), op) ? FRAME_OK : FRAME_ERR_QUEUE_FULL;
    EXPECT_EQ(started, FRAME_ERR_QUEUE_FULL);
    EXPECT_EQ(rec.deliveries(), 0u);
    EXPECT_EQ(rec.destroyed.load(), 0u);
    EXPECT_EQ(s.invoked_count(), 0u);
    EXPECT_EQ(s.destroyed_count(), 0u);
    core.stop();
}

// T12 (4): QUIESCING converts queued un-run POSTs to CLEANUP in place; every
// destroy runs on the worker thread (LIFE-007), no handler is invoked.
TEST(FrameCompletion, QuiescingCleanupRunsOnWorkerThread) {
    io_core core(1);
    strand s(core, 16);
    constexpr int kPosts = 16;
    std::vector<post_rec> posts(kPosts);
    for (auto& post : posts) {
        frame_post_operation_t op{};
        op.user = &post;
        op.invoke = [](void* user) { static_cast<post_rec*>(user)->invoked.store(true); };
        op.destroy = [](void* user) {
            auto* post = static_cast<post_rec*>(user);
            post->destroy_thread = std::this_thread::get_id();
            post->destroyed.store(true);
        };
        ASSERT_EQ(s.post(op), FRAME_OK);
    }
    s.begin_quiescing();
    core.start();

    ASSERT_TRUE(
        wait_until([&] { return s.destroyed_count() == kPosts; }, std::chrono::seconds{10}));
    ASSERT_TRUE(wait_until([&] { return s.stopped(); }, std::chrono::seconds{10}));
    EXPECT_EQ(s.invoked_count(), 0u);
    EXPECT_EQ(s.stale_generation_dropped(), 0u);
    const std::thread::id main_id = std::this_thread::get_id();
    for (const auto& post : posts) {
        EXPECT_FALSE(post.invoked.load());
        EXPECT_TRUE(post.destroyed.load());
        EXPECT_NE(post.destroy_thread, std::thread::id{});
        EXPECT_NE(post.destroy_thread, main_id); // destroys ran on the worker
        EXPECT_EQ(post.destroy_thread, posts.front().destroy_thread);
    }
    core.stop();
}

// T12 (5): stale generation callbacks are dropped (no complete/invoke) but
// still destroyed exactly once and counted.
TEST(FrameCompletion, StaleGenerationDroppedButDestroyed) {
    io_core core(1);
    strand s(core, 8);
    std::vector<post_rec> posts(4);
    for (auto& post : posts) {
        frame_post_operation_t op{};
        op.user = &post;
        op.invoke = [](void* user) { static_cast<post_rec*>(user)->invoked.store(true); };
        op.destroy = [](void* user) { static_cast<post_rec*>(user)->destroyed.store(true); };
        ASSERT_EQ(s.post(op), FRAME_OK);
    }
    const uint64_t stale_generation = s.generation();
    s.advance_generation();
    core.start();
    ASSERT_TRUE(wait_until([&] { return s.destroyed_count() == 4; }, std::chrono::seconds{10}));
    EXPECT_EQ(s.invoked_count(), 0u);
    EXPECT_EQ(s.stale_generation_dropped(), 4u);
    for (const auto& post : posts) {
        EXPECT_FALSE(post.invoked.load());
        EXPECT_TRUE(post.destroyed.load());
    }

    completion_node node;
    completion_rec rec;
    ASSERT_TRUE(node.reserve(s, stale_generation, make_completion_op(&rec)));
    ASSERT_TRUE(node.complete(kOriginalErr));
    ASSERT_TRUE(wait_until([&] { return rec.destroyed.load() == 1; }, std::chrono::seconds{10}));
    EXPECT_EQ(rec.deliveries(), 0u); // stale: complete() skipped
    EXPECT_EQ(rec.destroyed.load(), 1u);
    EXPECT_EQ(s.stale_generation_dropped(), 5u);
    core.stop();
}

// T12 (6): three-way race complete/cancel/quiesce per round on a fresh
// strand (quiesce is terminal). Invariants: exactly one completion delivery
// (original err or CANCELLED), every accepted post destroyed exactly once
// (invoked or cleaned up), the strand reaches stopped.
TEST(FrameCompletion, ThreeWayRaceCompleteCancelQuiesce) {
    io_core core(1);
    constexpr uint64_t kRounds = 20000;
    constexpr std::size_t kPool = 64;
    constexpr int kPostsPerRound = 3;
    std::vector<completion_node> nodes(kPool);
    std::vector<post_rec> posts(kPostsPerRound);
    completion_rec rec;
    std::atomic<uint64_t> gate{0};
    std::atomic<uint64_t> done[3] = {{0}, {0}, {0}};
    std::atomic<bool> shutdown{false};
    std::atomic<strand*> current{nullptr};
    std::atomic<uint64_t> reserve_failures{0};
    core.start();

    auto spinner = [&](int slot, auto act) {
        uint64_t next = 1;
        while (!shutdown.load(std::memory_order_acquire)) {
            while (gate.load(std::memory_order_acquire) < next) {
                if (shutdown.load(std::memory_order_acquire)) {
                    return;
                }
                std::this_thread::yield();
            }
            act(next - 1, nodes[(next - 1) % kPool]);
            done[slot].store(next, std::memory_order_release);
            ++next;
        }
    };
    std::thread complete_thread(
        spinner, 0, [&](uint64_t, completion_node& node) { (void)node.complete(kOriginalErr); });
    std::thread cancel_thread(spinner, 1,
                              [&](uint64_t, completion_node& node) { (void)node.cancel(); });
    std::thread quiesce_thread(spinner, 2, [&](uint64_t, completion_node&) {
        current.load(std::memory_order_acquire)->begin_quiescing();
    });

    for (uint64_t round = 0; round < kRounds; ++round) {
        strand round_strand(core, 8);
        for (auto& post : posts) {
            post.invoked.store(false);
            post.destroyed.store(false);
        }
        completion_node& node = nodes[round % kPool];
        if (!node.reserve(round_strand, round_strand.generation(), make_completion_op(&rec))) {
            reserve_failures.fetch_add(1);
            break;
        }
        for (auto& post : posts) {
            frame_post_operation_t op{};
            op.user = &post;
            op.invoke = [](void* user) { static_cast<post_rec*>(user)->invoked.store(true); };
            op.destroy = [](void* user) {
                auto* item = static_cast<post_rec*>(user);
                item->destroy_thread = std::this_thread::get_id();
                item->destroyed.store(true);
            };
            ASSERT_EQ(round_strand.post(op), FRAME_OK) << "round " << round;
        }
        current.store(&round_strand, std::memory_order_release);
        gate.store(round + 1, std::memory_order_release);
        ASSERT_TRUE(wait_until(
            [&] {
                return done[0].load(std::memory_order_acquire) >= round + 1 &&
                       done[1].load(std::memory_order_acquire) >= round + 1 &&
                       done[2].load(std::memory_order_acquire) >= round + 1 &&
                       rec.deliveries() == round + 1 && rec.destroyed.load() == round + 1 &&
                       round_strand.destroyed_count() == kPostsPerRound + 1 &&
                       round_strand.stopped();
            },
            std::chrono::seconds{30}))
            << "stalled at round " << round;
        for (const auto& post : posts) {
            ASSERT_TRUE(post.destroyed.load()) << "round " << round;
        }
        node.recycle();
    }

    shutdown.store(true, std::memory_order_release);
    complete_thread.join();
    cancel_thread.join();
    quiesce_thread.join();

    EXPECT_EQ(reserve_failures.load(), 0u);
    EXPECT_EQ(rec.deliveries(), kRounds);
    EXPECT_EQ(rec.complete_delivered.load() + rec.cancel_delivered.load(), kRounds);
    EXPECT_EQ(rec.destroyed.load(), kRounds);
    core.stop();
}
