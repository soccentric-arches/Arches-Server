#pragma once
// grpc_server/ClientStreamRegistry.h — board_id → outbound command sink.
//
// NEW for Arches (gap-closure #1 in the porting note). the prior reference implementation never emits
// run-control ServerMessages over the ClientService bidi stream — it only ever
// sends BatchAck/LogBatchAck inline from the read thread. The Arches client, by
// contrast, drives runs *exclusively* from ServerMessages it receives on that
// stream (ConfigureRun/StartRun/PauseRun/ResumeRun/StopRun/RestartRun). Without
// a way to push those from the server side, the client sits idle.
//
// This registry closes that gap. Each connected client's Connect() handler owns a
// `BoundedQueue<ServerMessage>` (the outbound queue) plus a writer thread that
// drains it into the stream. The handler registers that queue under the client's
// board_id on HelloReport, and deregisters on disconnect. RunService looks up the
// queue by board_id and enqueues run-control ServerMessages; the per-stream writer
// thread serializes all stream->Write() calls (gRPC forbids concurrent writes on
// one stream).
//
// Reconnect tolerance: a board may re-Hello (duplicate registration) — register()
// replaces the previous queue for that board_id. The stale stream's Connect() is
// tearing down and will deregister; deregister() only erases the map entry when
// the stored queue matches the one being removed, so a reconnect cannot yank the
// fresh queue out from under the new stream.

#include "grpc_server/StreamBackpressure.h"

#include "client_service.pb.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace arches::server::grpc {

// An outbound queue of ServerMessages for one connected client.
using OutQueue = BoundedQueue<arches::stream::ServerMessage>;
using OutQueuePtr = std::shared_ptr<OutQueue>;

class ClientStreamRegistry {
public:
    ClientStreamRegistry() = default;

    // Register (or replace) the outbound queue for board_id. Called by
    // ClientServiceImpl::Connect on HelloReport. Thread-safe.
    void register_stream(const std::string& board_id, OutQueuePtr queue);

    // Deregister the outbound queue for board_id, but only if the currently
    // registered queue is `queue` (guards against the reconnect race described
    // above). No-op if board_id is unknown or the queue was already replaced.
    void deregister_stream(const std::string& board_id, OutQueuePtr queue);

    // Enqueue a run-control ServerMessage to the board's outbound queue.
    // Returns false if no stream is registered for board_id (board not
    // connected) or the queue is closed (stream tearing down). The caller
    // (RunService) treats a false return as "client not reachable" and logs it.
    bool push(const std::string& board_id, const arches::stream::ServerMessage& msg);

    // Whether a board is currently connected (has a registered outbound queue).
    bool has_stream(const std::string& board_id);

private:
    std::mutex mutex_;
    std::unordered_map<std::string, OutQueuePtr> streams_;
};

}  // namespace arches::server::grpc