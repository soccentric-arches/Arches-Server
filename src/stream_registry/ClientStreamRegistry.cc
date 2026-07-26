#include "stream_registry/ClientStreamRegistry.h"

#include <spdlog/spdlog.h>

namespace arches::server::grpc {

void ClientStreamRegistry::register_stream(const std::string& board_id,
                                             OutQueuePtr queue) {
    std::lock_guard<std::mutex> lock{mutex_};
    auto it = streams_.find(board_id);
    if (it != streams_.end() && it->second == queue) return;  // same queue, no-op
    if (it != streams_.end()) {
        spdlog::info("client_registry: re-registering board_id={} (reconnect)", board_id);
        // Close the stale queue so its writer thread exits promptly.
        it->second->close();
    }
    streams_[board_id] = std::move(queue);
}

void ClientStreamRegistry::deregister_stream(const std::string& board_id,
                                               OutQueuePtr queue) {
    std::lock_guard<std::mutex> lock{mutex_};
    auto it = streams_.find(board_id);
    if (it == streams_.end()) return;
    // Only erase if the registered queue is the one being deregistered — a
    // reconnect may have already swapped in a fresh queue.
    if (it->second == queue) {
        streams_.erase(it);
    }
}

bool ClientStreamRegistry::push(const std::string& board_id,
                                 const arches::stream::ServerMessage& msg) {
    OutQueuePtr queue;
    {
        std::lock_guard<std::mutex> lock{mutex_};
        auto it = streams_.find(board_id);
        if (it == streams_.end()) return false;  // board not connected
        queue = it->second;
    }
    // Enqueue outside the registry lock so a full queue backpressures only the
    // pushing caller, not every other board's push.
    return queue->enqueue(msg);
}

bool ClientStreamRegistry::has_stream(const std::string& board_id) {
    std::lock_guard<std::mutex> lock{mutex_};
    return streams_.find(board_id) != streams_.end();
}

}  // namespace arches::server::grpc