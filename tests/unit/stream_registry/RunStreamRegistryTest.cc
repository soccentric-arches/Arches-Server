// tests/unit/stream_registry/RunStreamRegistryTest.cc — verify the SSE viewer
// fan-out primitives: BoundedEventQueue's non-blocking drop policy and
// RunStreamRegistry's register/deregister/publish semantics.
//
// These are the load-bearing primitives for the loop's "slow viewer never slows
// ingestion" and "ten viewers share one fan-out" acceptance criteria. The
// end-to-end proof (latency, multi-viewer, slow-viewer isolation through the
// real HTTP stack) lives in tests/integration/HttpSseInteropTest.cc.
#include "stream_registry/RunStreamRegistry.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using arches::server::grpc::BoundedEventQueue;
using arches::server::grpc::RunStreamRegistry;
using arches::server::grpc::SseEvent;
using arches::server::grpc::EventQueuePtr;

namespace {
SseEvent mk(std::string name, bool critical = false, std::string dedup = "") {
    return SseEvent{std::move(name), "{}", critical, std::move(dedup)};
}
}  // namespace

// ── BoundedEventQueue ────────────────────────────────────────────────────────

TEST(BoundedEventQueueTest, TryEnqueueDequeueRoundTrip) {
    BoundedEventQueue q{4};
    EXPECT_TRUE(q.try_enqueue(mk("result", false, "r1")));
    auto ev = q.dequeue();
    ASSERT_TRUE(ev.has_value());
    EXPECT_EQ(ev->name, "result");
    EXPECT_EQ(ev->dedup_key, "r1");
}

TEST(BoundedEventQueueTest, TryEnqueueNeverBlocks) {
    // Fill past capacity synchronously — try_enqueue must return promptly and
    // never block the calling (publishing) thread.
    BoundedEventQueue q{2};
    EXPECT_TRUE(q.try_enqueue(mk("result", false, "r1")));
    EXPECT_TRUE(q.try_enqueue(mk("result", false, "r2")));
    // Queue is now full; further enqueues apply the drop policy, not blocking.
    auto t0 = std::chrono::steady_clock::now();
    EXPECT_TRUE(q.try_enqueue(mk("result", false, "r3")));
    auto dt = std::chrono::steady_clock::now() - t0;
    EXPECT_LT(dt, std::chrono::milliseconds(200));  // non-blocking
    EXPECT_LE(q.size(), 2u + 1);  // at most capacity + one flow.paused marker
}

TEST(BoundedEventQueueTest, FullDropsOldestNonCriticalAndEmitsFlowPaused) {
    BoundedEventQueue q{2};
    ASSERT_TRUE(q.try_enqueue(mk("result", false, "r1")));  // non-critical
    ASSERT_TRUE(q.try_enqueue(mk("result", false, "r2")));  // non-critical
    // Full of non-critical. Enqueue another non-critical: to stay bounded at
    // capacity while emitting a flow.paused marker AND the new event, the
    // queue drops the two oldest non-critical events (r1, r2) and keeps
    // [flow.paused, r3]. The queue never grows past capacity.
    ASSERT_TRUE(q.try_enqueue(mk("result", false, "r3")));
    EXPECT_LE(q.size(), 2u);
    q.close();  // so the drain loop terminates (dequeue returns nullopt when drained)
    std::vector<std::string> names;
    while (auto ev = q.dequeue()) names.push_back(ev->name);
    ASSERT_EQ(names.size(), 2u);
    EXPECT_EQ(names[0], "flow.paused");
    EXPECT_EQ(names[1], "result");
}

TEST(BoundedEventQueueTest, FullOfCriticalKeepsCritical) {
    BoundedEventQueue q{2};
    ASSERT_TRUE(q.try_enqueue(mk("run.start", true)));
    ASSERT_TRUE(q.try_enqueue(mk("run.complete", true)));
    // Full of critical; incoming critical displaces oldest critical.
    ASSERT_TRUE(q.try_enqueue(mk("run.complete", true)));
    q.close();  // drain loop terminator
    int complete_count = 0;
    while (auto ev = q.dequeue()) {
        if (ev->name == "run.complete") ++complete_count;
    }
    // Two run.complete events remain (the displaced oldest + the new one).
    EXPECT_EQ(complete_count, 2);
}

TEST(BoundedEventQueueTest, CloseUnblocksDequeue) {
    BoundedEventQueue q{4};
    std::thread t{[&] {
        auto v = q.dequeue();
        EXPECT_FALSE(v.has_value());
    }};
    q.close();
    t.join();
}

TEST(BoundedEventQueueTest, DequeueForTimesOut) {
    BoundedEventQueue q{4};
    auto t0 = std::chrono::steady_clock::now();
    auto ev = q.dequeue_for(std::chrono::milliseconds(50));
    auto dt = std::chrono::steady_clock::now() - t0;
    EXPECT_FALSE(ev.has_value());
    EXPECT_GE(dt, std::chrono::milliseconds(40));
}

// ── RunStreamRegistry ────────────────────────────────────────────────────────

TEST(RunStreamRegistryTest, PublishReachesAllRegisteredViewers) {
    RunStreamRegistry reg;
    auto q1 = std::make_shared<BoundedEventQueue>(8);
    auto q2 = std::make_shared<BoundedEventQueue>(8);
    reg.register_stream("runA", q1);
    reg.register_stream("runA", q2);
    EXPECT_EQ(reg.publish("runA", mk("result", false, "r1")), 2u);
    auto e1 = q1->dequeue();
    auto e2 = q2->dequeue();
    ASSERT_TRUE(e1); ASSERT_TRUE(e2);
    EXPECT_EQ(e1->dedup_key, "r1");
    EXPECT_EQ(e2->dedup_key, "r1");
}

TEST(RunStreamRegistryTest, PublishToOneRunDoesNotReachAnother) {
    RunStreamRegistry reg;
    auto qA = std::make_shared<BoundedEventQueue>(8);
    auto qB = std::make_shared<BoundedEventQueue>(8);
    reg.register_stream("runA", qA);
    reg.register_stream("runB", qB);
    reg.publish("runA", mk("result", false, "r1"));
    EXPECT_TRUE(qA->dequeue().has_value());
    EXPECT_FALSE(qB->dequeue_for(std::chrono::milliseconds(20)).has_value());
}

TEST(RunStreamRegistryTest, PublishWithNoViewersIsNoOp) {
    RunStreamRegistry reg;
    EXPECT_EQ(reg.publish("ghost", mk("result")), 0u);
    EXPECT_FALSE(reg.has_stream("ghost"));
}

TEST(RunStreamRegistryTest, DeregisterStopsDelivery) {
    RunStreamRegistry reg;
    auto q = std::make_shared<BoundedEventQueue>(8);
    reg.register_stream("runA", q);
    reg.deregister_stream("runA", q);
    EXPECT_EQ(reg.publish("runA", mk("result")), 0u);
}

TEST(RunStreamRegistryTest, DeregisterByIdentityDoesNotYankFreshQueue) {
    // A reconnect race: a stale viewer deregisters after a fresh one registered.
    // The fresh queue must NOT be removed.
    RunStreamRegistry reg;
    auto stale = std::make_shared<BoundedEventQueue>(8);
    reg.register_stream("runA", stale);
    auto fresh = std::make_shared<BoundedEventQueue>(8);
    reg.register_stream("runA", fresh);  // replace
    reg.deregister_stream("runA", stale);  // stale teardown — must not erase fresh
    EXPECT_EQ(reg.publish("runA", mk("result")), 1u);
    EXPECT_TRUE(fresh->dequeue().has_value());
}

TEST(RunStreamRegistryTest, PublisherNeverBlocksOnSlowViewer) {
    // A stalled viewer (never drains) with a tiny queue: publish must still
    // return promptly. This is the core "slow viewer never slows ingestion"
    // guarantee, exercised at the primitive level here and through the real
    // HTTP stack in HttpSseInteropTest.
    RunStreamRegistry reg;
    auto slow = std::make_shared<BoundedEventQueue>(2);
    reg.register_stream("runA", slow);
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 1000; ++i) {
        reg.publish("runA", mk("result", false, std::to_string(i)));
    }
    auto dt = std::chrono::steady_clock::now() - t0;
    EXPECT_LT(dt, std::chrono::seconds(1));  // 1000 publishes to a full queue — fast
}