// stream_registry/RunStreamRegistry.cc — see header.
#include "stream_registry/RunStreamRegistry.h"

#include <algorithm>
#include <utility>

namespace arches::server::grpc {

bool BoundedEventQueue::try_enqueue(SseEvent ev) {
    std::lock_guard<std::mutex> lock{mutex_};
    if (closed_) return false;

    if (queue_.size() < capacity_) {
        queue_.push_back(std::move(ev));
        not_empty_.notify_one();
        return true;
    }

    // Queue is full. We want to place the new event, and if we drop any
    // non-critical event, also emit a flow.paused marker before it. That
    // requires room for (marker + ev) = 2 slots when dropping. Drop
    // non-critical events until there's room for both (size + 2 <= capacity_),
    // or until only critical events remain. The queue never grows past
    // capacity_.
    bool dropped = false;
    while (queue_.size() + 2 > capacity_) {
        auto it = std::find_if(queue_.begin(), queue_.end(),
                               [](const SseEvent& e) { return !e.critical; });
        if (it == queue_.end()) break;  // only critical events in flight
        queue_.erase(it);
        dropped = true;
    }

    if (queue_.size() < capacity_) {
        // There's room for the new event. Emit a flow.paused marker first if we
        // dropped something AND there's room for both marker and ev.
        if (dropped && queue_.size() + 1 < capacity_) {
            SseEvent paused{"flow.paused", "{}", /*critical=*/false, ""};
            queue_.push_back(std::move(paused));
        }
        queue_.push_back(std::move(ev));
        not_empty_.notify_one();
        return true;
    }

    // No non-critical event was droppable (queue full of critical events).
    if (!ev.critical) {
        // Drop the incoming non-critical event; leave a marker only if room
        // (there won't be — queue is full of criticals), so just drop silently.
        not_empty_.notify_one();
        return false;
    }
    // Incoming critical event: displace the oldest critical so the latest
    // terminal state wins (rare — sustained backpressure of terminal events).
    queue_.pop_front();
    queue_.push_back(std::move(ev));
    not_empty_.notify_one();
    return true;
}

std::optional<SseEvent> BoundedEventQueue::dequeue() {
    std::unique_lock<std::mutex> lock{mutex_};
    not_empty_.wait(lock, [this] { return closed_ || !queue_.empty(); });
    if (queue_.empty()) return std::nullopt;
    SseEvent item = std::move(queue_.front());
    queue_.pop_front();
    return item;
}

std::optional<SseEvent> BoundedEventQueue::dequeue_for(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock{mutex_};
    if (!not_empty_.wait_for(lock, timeout, [this] { return closed_ || !queue_.empty(); })) {
        return std::nullopt;  // timed out, still empty
    }
    if (queue_.empty()) return std::nullopt;  // closed and drained
    SseEvent item = std::move(queue_.front());
    queue_.pop_front();
    return item;
}

void BoundedEventQueue::close() {
    std::lock_guard<std::mutex> lock{mutex_};
    closed_ = true;
    not_empty_.notify_all();
}

bool BoundedEventQueue::is_closed() const {
    std::lock_guard<std::mutex> lock{mutex_};
    return closed_;
}

std::size_t BoundedEventQueue::size() const {
    std::lock_guard<std::mutex> lock{mutex_};
    return queue_.size();
}

// ── RunStreamRegistry ────────────────────────────────────────────────────────

void RunStreamRegistry::register_stream(const std::string& run_id, EventQueuePtr queue) {
    std::lock_guard<std::mutex> lock{mutex_};
    streams_[run_id].insert(std::move(queue));
}

void RunStreamRegistry::deregister_stream(const std::string& run_id, EventQueuePtr queue) {
    std::lock_guard<std::mutex> lock{mutex_};
    auto it = streams_.find(run_id);
    if (it == streams_.end()) return;
    it->second.erase(queue);
    if (it->second.empty()) streams_.erase(it);
}

std::size_t RunStreamRegistry::publish(const std::string& run_id, const SseEvent& ev) {
    std::lock_guard<std::mutex> lock{mutex_};
    auto it = streams_.find(run_id);
    if (it == streams_.end() || it->second.empty()) return 0;
    for (const auto& q : it->second) {
        q->try_enqueue(ev);  // non-blocking; never throws
    }
    return it->second.size();
}

bool RunStreamRegistry::has_stream(const std::string& run_id) {
    std::lock_guard<std::mutex> lock{mutex_};
    auto it = streams_.find(run_id);
    return it != streams_.end() && !it->second.empty();
}

}  // namespace arches::server::grpc