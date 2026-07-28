#pragma once
// stream_registry/RunStreamRegistry.h — run_id -> SSE viewer fan-out registry.
//
// NEW for the live-run-monitoring loop. Mirrors ClientStreamRegistry's pattern
// (board_id -> outbound BoundedQueue), applied to SSE viewers: each HTTP/SSE
// connection registered for a run owns a BoundedEventQueue; the RunService
// publishing path (ingest_result / set_run_status / complete_run) calls
// publish(run_id, event) which fans the event out to every registered viewer's
// queue in memory — no per-viewer DB query, ten viewers share one fan-out.
//
// Hard contract (from the loop plan):
//   - The publisher NEVER blocks. publish() calls try_enqueue() on each queue,
//     which is non-blocking: on a full queue it drops the oldest non-critical
//     event, emits a flow.paused marker, and returns. A stalled viewer therefore
//     never slows RunService::insert (the ingestion path).
//   - run.start and run.complete are marked critical (never dropped); result and
//     module.transition events are non-critical (droppable under pressure).
//
// Layering: lives in arches_stream_registry (the base layer) so RunService
// (arches_services) can depend on it without a cycle with arches_http, which
// consumes the registry from the layer above.

#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace arches::server::grpc {

// One SSE event to broadcast to a viewer.
struct SseEvent {
    std::string name;        // run.start | module.transition | result | run.complete | flow.paused
    std::string data;        // JSON payload (already serialized)
    bool critical = false;   // critical events (run.start/run.complete) are never dropped
    std::string dedup_key;   // for result events: the result id; empty for non-dedup events
};

// Bounded MPSC queue of SseEvents for one SSE viewer connection.
//
// Differences vs the blocking BoundedQueue in StreamBackpressure.h:
//   - try_enqueue() is NON-BLOCKING: returns immediately, dropping on full.
//     The publishing path must never block on a slow viewer.
//   - dequeue() blocks (the SSE writer thread drains at its own pace).
//   - On a full try_enqueue: drop the oldest non-critical event, push a
//     flow.paused marker, then push the new event. If the queue is full of
//     critical events only, the incoming non-critical event is dropped (and a
//     flow.paused marker is emitted in its place); an incoming critical event
//     displaces the oldest critical event (rare — only under sustained
//     backpressure of terminal events, which should not happen for one run).
class BoundedEventQueue {
public:
    explicit BoundedEventQueue(std::size_t capacity) : capacity_(capacity) {}

    // Non-blocking enqueue. Returns true if the event (or a flow.paused marker
    // representing it) was placed in the queue. Never blocks.
    bool try_enqueue(SseEvent ev);

    // Blocking dequeue. Returns nullopt when the queue is closed AND drained.
    std::optional<SseEvent> dequeue();

    // Timed dequeue. Waits up to `timeout` for an event. Returns nullopt on
    // timeout OR when the queue is closed and drained. Used by the SSE writer
    // loop so it can periodically check whether the HTTP client is still
    // connected (a plain blocking dequeue would never wake on a silent
    // disconnect while the queue is idle).
    std::optional<SseEvent> dequeue_for(std::chrono::milliseconds timeout);

    void close();
    [[nodiscard]] bool is_closed() const;
    [[nodiscard]] std::size_t size() const;

private:
    mutable std::mutex      mutex_;
    std::condition_variable not_empty_;
    std::deque<SseEvent>    queue_;
    std::size_t             capacity_;
    bool                    closed_ = false;
};

using EventQueue = BoundedEventQueue;
using EventQueuePtr = std::shared_ptr<EventQueue>;

class RunStreamRegistry {
public:
    RunStreamRegistry() = default;

    // Register (or replace) the viewer queue for run_id. Called by the SSE
    // handler on connect. Thread-safe.
    void register_stream(const std::string& run_id, EventQueuePtr queue);

    // Deregister the viewer queue for run_id, but only if the currently
    // registered entry matches `queue` (identity guard against reconnect/teardown
    // races, mirroring ClientStreamRegistry::deregister_stream).
    void deregister_stream(const std::string& run_id, EventQueuePtr queue);

    // Fan `ev` out to every viewer registered for run_id. Non-blocking per
    // viewer (try_enqueue). Returns the number of viewers the event reached
    // (a viewer whose queue was full and dropped the event still counts as
    // reached — a flow.paused marker is left in its place).
    std::size_t publish(const std::string& run_id, const SseEvent& ev);

    // Whether any viewer is currently registered for run_id.
    bool has_stream(const std::string& run_id);

private:
    std::mutex mutex_;
    // A run can have N concurrent viewers, so the value is a set of queues.
    // std::unordered_set<shared_ptr> hashes by the shared_ptr's stored pointer
    // (pointer identity), which is what we want — register/deregister by the
    // same shared_ptr instance.
    std::unordered_map<std::string, std::unordered_set<EventQueuePtr>> streams_;
};

}  // namespace arches::server::grpc