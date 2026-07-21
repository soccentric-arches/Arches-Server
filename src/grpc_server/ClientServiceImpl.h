#pragma once
// grpc_server/ClientServiceImpl.h — implements the acadia.stream::ClientService
// bidi stream (the client<->server interop path).
//
// Ported from Acadia-Server with the two §5/interop gap-closures:
//   - §5 module-Result unpack in handle_result_batch: each ResultSample.payload
//     (google.protobuf.Any) is unpacked via util::unpack_module_results, which
//     handles the hal.modules.<id>.Result wrapper AND the bare-TestResult fallback.
//     Acadia only unpacked bare hal.common.TestResult and rejected everything else.
//   - batch.state() mapping: on a terminal batch, RunService::set_run_status maps
//     the client's RunState -> runs.status instead of Acadia's blind
//     complete_run("completed"). The UNSPECIFIED guard is preserved.
//
// New vs Acadia: a per-stream outbound writer thread drains an out_queue of
// ServerMessages (run-control from RunService via ClientStreamRegistry + acks
// produced here) into the stream, so all stream->Write() calls are serialized
// (gRPC forbids concurrent writes on one stream). Acadia wrote acks inline from
// the read thread and never wrote run-control, so it had no concurrency hazard.
//
// HelloReport registers the board_id -> out_queue in the ClientStreamRegistry so
// RunService can push run-control to the right client. Reconnect (duplicate
// Hello) re-registers; the registry tolerates it.

#include "services/RunService.h"
#include "services/LogService.h"
#include "grpc_server/StreamBackpressure.h"
#include "stream_registry/ClientStreamRegistry.h"

#include "client_service.grpc.pb.h"
#include "client_service.pb.h"
#include "common.pb.h"

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <thread>

namespace arches::server::grpc {

class ClientServiceImpl final
    : public acadia::stream::ClientService::Service {
public:
    ClientServiceImpl(RunService& run_svc, LogService& log_svc,
                      ClientStreamRegistry& stream_registry,
                      std::size_t queue_capacity = 1024);

    ::grpc::Status Connect(
        ::grpc::ServerContext*                                                    ctx,
        ::grpc::ServerReaderWriter<acadia::stream::ServerMessage,
                                   acadia::stream::ClientMessage>* stream) override;

private:
    struct IngestWork {
        std::string              run_id;
        std::string              module_id;
        std::string              module_version;
        std::string              mode;
        hal::common::TestResult  tr;
    };

    RunService&              run_svc_;
    LogService&              log_svc_;
    ClientStreamRegistry&   stream_registry_;
    std::size_t             queue_capacity_;

    // §5 unpack: per ResultSample, unpack the module-specific Result (or bare
    // TestResult fallback) and enqueue each TestResult for the DB writer thread.
    void handle_result_batch(const acadia::stream::TestResultBatch& batch,
                              BoundedQueue<IngestWork>& ingest_queue);

    void handle_log_batch(const acadia::stream::LogBatch& batch);
};

}  // namespace arches::server::grpc