#pragma once
// grpc_server/StreamBackpressure.h — bounded MPSC queue for gRPC stream
// handlers. Ported as-is from the prior reference implementation (pure std, no
// Drogon). If the queue is full, enqueue() blocks until space is available
// (backpressure). The DB writer / outbound writer threads drain the queue from
// separate threads.
//
// Used by ClientServiceImpl for two queues:
//   - ingest_queue  (ClientMessage::kResultBatch -> DB writer thread ->
//   RunService)
//   - out_queue      (ServerMessage run-control + acks -> outbound writer ->
//   stream)

#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <spdlog/spdlog.h>

namespace arches::server::grpc {

template <typename T> class BoundedQueue {
    public:
    explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {}

    // Enqueue an item.  Blocks if the queue is at capacity.
    // Returns false if the queue has been closed.
    //
    // Saturation is logged (Task.md E-07). A blocked enqueue IS the
    // backpressure working as designed, but it was entirely silent: ingest
    // falling behind looked identical to ingest keeping up, from outside. The
    // client on the other end is being throttled and its spool is growing, and
    // nobody could see why.
    //
    // Logged once per saturation episode rather than per call, so a sustained
    // backlog does not drown the log -- and logged again when it drains, so
    // the episode has an end as well as a start.
    bool enqueue(T item) {
        std::unique_lock<std::mutex> lock{mutex_};
        if (!closed_ && queue_.size() >= capacity_) {
            if (!saturated_) {
                saturated_ = true;
                spdlog::warn("ingest queue saturated at {} items: producer is being throttled",
                             capacity_);
            }
        }
        not_full_.wait(lock, [this] { return closed_ || queue_.size() < capacity_; });
        if (closed_)
            return false;
        queue_.push_back(std::move(item));
        not_empty_.notify_one();
        return true;
    }

    // Dequeue an item.  Blocks until an item is available or the queue is closed.
    std::optional<T> dequeue() {
        std::unique_lock<std::mutex> lock{mutex_};
        not_empty_.wait(lock, [this] { return closed_ || !queue_.empty(); });
        if (queue_.empty())
            return std::nullopt;
        T item = std::move(queue_.front());
        queue_.pop_front();
        if (saturated_ && queue_.size() * 2 <= capacity_) {
            // Hysteresis at half capacity: recovering at capacity-1 would flap
            // the log on every item while sitting near the limit.
            saturated_ = false;
            spdlog::info("ingest queue recovered: {}/{} items", queue_.size(), capacity_);
        }
        not_full_.notify_one();
        return item;
    }

    // Close the queue.  All blocked dequeue() calls will return nullopt.
    void close() {
        std::lock_guard<std::mutex> lock{mutex_};
        closed_ = true;
        not_full_.notify_all();
        not_empty_.notify_all();
    }

    [[nodiscard]] bool is_closed() const {
        std::lock_guard<std::mutex> lock{mutex_};
        return closed_;
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard<std::mutex> lock{mutex_};
        return queue_.size();
    }

    private:
    mutable std::mutex mutex_;
    std::condition_variable not_full_;
    // True while an episode of saturation is in progress; see enqueue().
    bool saturated_{false};
    std::condition_variable not_empty_;
    std::deque<T> queue_;
    std::size_t capacity_;
    bool closed_ = false;
};

} // namespace arches::server::grpc