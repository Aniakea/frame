#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "frame_poc/frame_strand.hh"

using frame::poc::io_core;
using frame::poc::strand;

namespace {

constexpr auto kPollDelay = std::chrono::microseconds{50};

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

struct ticket_cell {
    std::vector<uint64_t> observed; // written only by the worker, read after destroy gate
};

struct ticket_payload {
    ticket_cell* cell;
    uint64_t ticket;
};

struct concurrency_guard {
    std::atomic<bool> in_flight[2] = {false, false};
    std::atomic<bool> fault{false};
    std::atomic<bool> first{true};
    std::atomic<bool> id_mismatch{false};
    std::thread::id worker{}; // touched only by the single worker thread
};

struct slot_payload {
    concurrency_guard* guard;
    int slot;
};

struct flag_payload {
    std::atomic<bool>* invoked;
    std::atomic<bool>* destroyed;
};

struct slow_payload {
    std::chrono::milliseconds nap;
};

} // namespace

// T11 (1): one strand, 3 producer threads x 10k posts each. Per-producer
// ticket order must be strictly increasing in the handler (FIFO + 4.1.2
// linearization at the strand mutex).
TEST(FrameStrand, StrictFifoOrderAcrossProducers) {
    io_core core(1);
    strand s(core, 31000);
    constexpr int kProducers = 3;
    constexpr uint64_t kPerProducer = 10000;
    std::vector<ticket_cell> cells(kProducers);
    std::vector<ticket_payload> payloads;
    payloads.reserve(static_cast<size_t>(kProducers) * kPerProducer);
    for (int producer = 0; producer < kProducers; ++producer) {
        for (uint64_t ticket = 0; ticket < kPerProducer; ++ticket) {
            payloads.push_back(ticket_payload{&cells[static_cast<size_t>(producer)], ticket});
        }
    }
    core.start();

    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (int producer = 0; producer < kProducers; ++producer) {
        producers.emplace_back([&, producer] {
            for (uint64_t ticket = 0; ticket < kPerProducer; ++ticket) {
                ticket_payload& payload =
                    payloads[static_cast<size_t>(producer) * kPerProducer + ticket];
                frame_post_operation_t op{};
                op.user = &payload;
                op.invoke = [](void* user) {
                    auto* item = static_cast<ticket_payload*>(user);
                    item->cell->observed.push_back(item->ticket);
                };
                op.destroy = [](void*) {};
                ASSERT_EQ(s.post(op), FRAME_OK);
            }
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }

    const uint64_t total = static_cast<uint64_t>(kProducers) * kPerProducer;
    ASSERT_TRUE(wait_until([&] { return s.destroyed_count() == total; }, std::chrono::seconds{30}));
    for (int producer = 0; producer < kProducers; ++producer) {
        const auto& observed = cells[static_cast<size_t>(producer)].observed;
        ASSERT_EQ(observed.size(), kPerProducer);
        for (uint64_t index = 0; index < kPerProducer; ++index) {
            ASSERT_EQ(observed[index], index)
                << "producer " << producer << " ticket order broken at " << index;
        }
    }
    EXPECT_EQ(s.invoked_count(), total);
    core.stop();
}

// T11 (2): same-strand handlers never overlap (in-flight CAS guard) and all
// handlers run on one worker thread (LIFE-001 serialization).
TEST(FrameStrand, SameStrandHandlersNeverConcurrent) {
    io_core core(2);
    strand strands[2]{strand(core, 512), strand(core, 512)};
    concurrency_guard guard;
    slot_payload payloads[2]{{&guard, 0}, {&guard, 1}};
    core.start();

    auto launch = [&](int slot, int count) {
        return std::thread([&, this_slot = slot, this_count = count] {
            strand& target = strands[this_slot];
            for (int index = 0; index < this_count; ++index) {
                frame_post_operation_t op{};
                op.user = &payloads[this_slot];
                op.invoke = [](void* user) {
                    auto* item = static_cast<slot_payload*>(user);
                    auto* shared = item->guard;
                    const int slot = item->slot;
                    bool expected = false;
                    if (!shared->in_flight[slot].compare_exchange_strong(expected, true)) {
                        shared->fault.store(true);
                    }
                    const std::thread::id current = std::this_thread::get_id();
                    if (shared->first.exchange(false)) {
                        shared->worker = current;
                    } else if (current != shared->worker) {
                        shared->id_mismatch.store(true);
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds{20});
                    shared->in_flight[slot].store(false);
                };
                op.destroy = [](void*) {};
                while (target.post(op) == FRAME_ERR_QUEUE_FULL) {
                    std::this_thread::yield();
                }
            }
        });
    };
    std::vector<std::thread> threads;
    for (int index = 0; index < 2; ++index) {
        threads.push_back(launch(0, 400));
        threads.push_back(launch(1, 400));
    }
    for (auto& thread : threads) {
        thread.join();
    }

    const uint64_t total = 1600;
    ASSERT_TRUE(wait_until(
        [&] { return strands[0].destroyed_count() + strands[1].destroyed_count() == total; },
        std::chrono::seconds{30}));
    EXPECT_FALSE(guard.fault.load());
    EXPECT_FALSE(guard.id_mismatch.load());
    core.stop();
}

// T11 (3): the ready-ring duplicate detector stays clear under multi-producer
// stress against a single strand (max-once invariant, 4.1.3).
TEST(FrameStrand, ReadyQueueNeverDuplicatesStrand) {
    io_core core(1);
    strand s(core, 256);
    constexpr int kProducers = 8;
    constexpr int kPerProducer = 5000;
    core.start();

    auto producer = [&] {
        for (int index = 0; index < kPerProducer; ++index) {
            frame_post_operation_t op{};
            op.user = nullptr;
            op.invoke = [](void*) {};
            op.destroy = [](void*) {};
            while (s.post(op) == FRAME_ERR_QUEUE_FULL) {
                std::this_thread::yield();
            }
        }
    };
    std::vector<std::thread> threads;
    threads.reserve(kProducers);
    for (int index = 0; index < kProducers; ++index) {
        threads.emplace_back(producer);
    }
    for (auto& thread : threads) {
        thread.join();
    }
    const uint64_t total = static_cast<uint64_t>(kProducers) * kPerProducer;
    ASSERT_TRUE(wait_until([&] { return s.destroyed_count() == total; }, std::chrono::seconds{30}));
    EXPECT_FALSE(core.duplicate_detected());
    EXPECT_EQ(s.invoked_count(), total);
    core.stop();
}

// T11 (4): producer storm vs drain with a tiny credit window (capacity 64):
// after 1e5 accepted posts every operation must be invoked and destroyed —
// a lost wakeup would strand FIFO nodes forever (4.1.3 anti-lost-wakeup).
TEST(FrameStrand, NoLostWakeupUnderProducerStorm) {
    io_core core(1);
    strand s(core, 64);
    constexpr int kProducers = 4;
    constexpr uint64_t kTotal = 100000;
    std::atomic<uint64_t> accepted{0};
    core.start();

    auto producer = [&] {
        for (;;) {
            if (accepted.load() >= kTotal) {
                return;
            }
            frame_post_operation_t op{};
            op.user = nullptr;
            op.invoke = [](void*) {};
            op.destroy = [](void*) {};
            while (s.post(op) == FRAME_ERR_QUEUE_FULL) {
                std::this_thread::yield();
            }
            accepted.fetch_add(1);
        }
    };
    std::vector<std::thread> threads;
    threads.reserve(kProducers);
    for (int index = 0; index < kProducers; ++index) {
        threads.emplace_back(producer);
    }
    for (auto& thread : threads) {
        thread.join();
    }
    // The claim check races, so the exact accepted total can exceed kTotal by
    // up to kProducers-1; every accepted post must still be invoked+destroyed.
    const uint64_t total = accepted.load(std::memory_order_acquire);
    ASSERT_GE(total, kTotal);
    ASSERT_TRUE(wait_until([&] { return s.invoked_count() == total; }, std::chrono::seconds{60}));
    ASSERT_TRUE(wait_until([&] { return s.destroyed_count() == total; }, std::chrono::seconds{60}));
    s.begin_quiescing();
    ASSERT_TRUE(wait_until([&] { return s.stopped(); }, std::chrono::seconds{10}));
    EXPECT_EQ(s.stale_generation_dropped(), 0u);
    core.stop();
}

// T11 (5): credit exhaustion returns FRAME_ERR_QUEUE_FULL and the caller
// keeps ownership (destroy NOT called on reject, 4.1.2).
TEST(FrameStrand, QueueFullRejectKeepsCallerOwnership) {
    io_core core(1);
    strand s(core, 8);
    std::vector<std::atomic<bool>> invoked_flags(8);
    std::atomic<bool> reject_destroyed{false};
    for (auto& flag : invoked_flags) {
        flag.store(false);
    }

    for (auto& flag : invoked_flags) {
        frame_post_operation_t op{};
        op.user = &flag;
        op.invoke = [](void*) {};
        op.destroy = [](void* user) { static_cast<std::atomic<bool>*>(user)->store(true); };
        ASSERT_EQ(s.post(op), FRAME_OK);
    }
    frame_post_operation_t overflow{};
    overflow.user = &reject_destroyed;
    overflow.invoke = [](void*) {};
    overflow.destroy = [](void* user) { static_cast<std::atomic<bool>*>(user)->store(true); };
    ASSERT_EQ(s.post(overflow), FRAME_ERR_QUEUE_FULL);
    ASSERT_FALSE(reject_destroyed.load());

    core.start();
    ASSERT_TRUE(wait_until([&] { return s.destroyed_count() == 8; }, std::chrono::seconds{10}));
    EXPECT_EQ(s.invoked_count(), 8u);
    EXPECT_FALSE(reject_destroyed.load());
    for (const auto& flag : invoked_flags) {
        EXPECT_TRUE(flag.load());
    }
    core.stop();
}

// T11 (6): a handler slower than the budget bumps handler_overruns
// (LIFE-002 metric only — no preemption, no isolation).
TEST(FrameStrand, OverrunCounterIncrementsOnSlowHandler) {
    io_core core(1);
    strand s(core, 8);
    core.start();

    frame_post_operation_t fast{};
    fast.user = nullptr;
    fast.invoke = [](void*) {};
    fast.destroy = [](void*) {};
    ASSERT_EQ(s.post(fast), FRAME_OK);
    ASSERT_TRUE(wait_until([&] { return s.destroyed_count() == 1; }, std::chrono::seconds{10}));
    EXPECT_EQ(s.handler_overruns(), 0u);

    slow_payload slow{std::chrono::milliseconds{3}};
    for (int index = 0; index < 3; ++index) {
        frame_post_operation_t op{};
        op.user = &slow;
        op.invoke = [](void* user) {
            std::this_thread::sleep_for(static_cast<slow_payload*>(user)->nap);
        };
        op.destroy = [](void*) {};
        ASSERT_EQ(s.post(op), FRAME_OK);
    }
    ASSERT_TRUE(wait_until([&] { return s.destroyed_count() == 4; }, std::chrono::seconds{10}));
    EXPECT_EQ(s.handler_overruns(), 3u);
    core.stop();
}

// T11 (7): posts are rejected with FRAME_ERR_PLUGIN_STOPPING once the strand
// is closing, and queued un-run posts are cleaned up (invoke skipped,
// destroy exactly once, LIFE-007).
TEST(FrameStrand, ClosingRejectsPostsAndCleansUp) {
    io_core core(1);
    strand s(core, 4);
    std::vector<std::atomic<bool>> invoked_flags(2);
    std::vector<std::atomic<bool>> destroy_flags(2);
    for (size_t index = 0; index < 2; ++index) {
        invoked_flags[index].store(false);
        destroy_flags[index].store(false);
    }
    std::vector<flag_payload> payloads;
    for (size_t index = 0; index < 2; ++index) {
        payloads.push_back(flag_payload{&invoked_flags[index], &destroy_flags[index]});
    }
    for (auto& payload : payloads) {
        frame_post_operation_t op{};
        op.user = &payload;
        op.invoke = [](void* user) { static_cast<flag_payload*>(user)->invoked->store(true); };
        op.destroy = [](void* user) { static_cast<flag_payload*>(user)->destroyed->store(true); };
        ASSERT_EQ(s.post(op), FRAME_OK);
    }
    s.begin_quiescing();
    frame_post_operation_t late{};
    late.user = nullptr;
    late.invoke = [](void*) {};
    late.destroy = [](void*) {};
    ASSERT_EQ(s.post(late), FRAME_ERR_PLUGIN_STOPPING);

    core.start();
    ASSERT_TRUE(wait_until([&] { return s.destroyed_count() == 2; }, std::chrono::seconds{10}));
    ASSERT_TRUE(wait_until([&] { return s.stopped(); }, std::chrono::seconds{10}));
    EXPECT_EQ(s.invoked_count(), 0u);
    for (size_t index = 0; index < 2; ++index) {
        EXPECT_FALSE(invoked_flags[index].load());
        EXPECT_TRUE(destroy_flags[index].load());
    }
    core.stop();
}
