#include "grpc_server/ClientServiceImpl.h"

#include "util/ResultConvert.h"

#include <spdlog/spdlog.h>

#include <thread>

namespace arches::server::grpc {

ClientServiceImpl::ClientServiceImpl(RunService& run_svc, LogService& log_svc,
                                       ClientStreamRegistry& stream_registry,
                                       std::size_t queue_capacity)
    : run_svc_(run_svc),
      log_svc_(log_svc),
      stream_registry_(stream_registry),
      queue_capacity_(queue_capacity) {}

// ── §5 unpack: drain a TestResultBatch into per-TestResult ingest work ──────────
void ClientServiceImpl::handle_result_batch(
    const acadia::stream::TestResultBatch& batch,
    BoundedQueue<IngestWork>&              ingest_queue) {

    // NOTE: results.mode stores the ExecutionMode int as a string, matching
    // Acadia-Server's behaviour (a known quirk flagged in the porting note for
    // Phase-2 cleanup; runs.mode uses the readable name via RunService).
    const std::string mode = std::to_string(static_cast<int>(batch.mode()));

    for (const auto& sample : batch.samples()) {
        // §5: unpack the module-specific Result (hal.modules.<id>.Result {
        // repeated hal.common.TestResult results }) or fall back to a bare
        // hal.common.TestResult when the descriptor is absent / the type_url is
        // the bare form. This is the server-side inverse of the client's
        // pack_module_result.
        auto results = util::unpack_module_results(sample.payload(), batch.module_id());
        if (results.empty()) {
            spdlog::debug("client_svc: no results unpacked run_id={} module_id={}",
                          batch.run_id(), batch.module_id());
            continue;
        }

        for (auto& tr : results) {
            // UNSPECIFIED guard — the client should never emit it.
            if (tr.output().status() == hal::common::TEST_STATUS_UNSPECIFIED) {
                spdlog::warn("client_svc: UNSPECIFIED status rejected run_id={} module_id={}",
                             batch.run_id(), batch.module_id());
                continue;
            }

            IngestWork work;
            work.run_id         = batch.run_id();
            work.module_id      = batch.module_id();
            work.module_version = "";  // not carried in the batch; resolved at ingest
            work.mode           = mode;
            work.tr             = std::move(tr);

            if (!ingest_queue.enqueue(std::move(work))) {
                // Queue closed — stream is shutting down.
                spdlog::info("client_svc: ingest queue closed, dropping run_id={}",
                             batch.run_id());
                return;
            }
        }
    }
}

// ── Handle a LogBatch ──────────────────────────────────────────────────────────
void ClientServiceImpl::handle_log_batch(const acadia::stream::LogBatch& batch) {
    for (const auto& line : batch.lines()) {
        LogEntry e;
        e.run_id   = line.run_id().empty() ? batch.run_id() : line.run_id();
        e.source   = line.source();
        e.severity = line.level();
        e.message  = line.text();
        log_svc_.ingest(e);
    }
}

// ── Connect (bidi stream) ───────────────────────────────────────────────────────
::grpc::Status ClientServiceImpl::Connect(
    ::grpc::ServerContext* ctx,
    ::grpc::ServerReaderWriter<acadia::stream::ServerMessage,
                                acadia::stream::ClientMessage>* stream) {

    BoundedQueue<IngestWork> ingest_queue{queue_capacity_};
    // Outbound queue: run-control (from RunService via the registry) + acks
    // (produced here). Shared so the registry can push to it. One writer thread
    // drains it -> stream->Write so all writes are serialized.
    auto out_queue = std::make_shared<OutQueue>(queue_capacity_);

    // DB writer thread: drain ingest_queue -> RunService::ingest_test_result.
    std::thread db_writer{[&] {
        while (true) {
            auto work = ingest_queue.dequeue();
            if (!work.has_value()) break;  // queue closed
            try {
                run_svc_.ingest_test_result(work->tr, work->run_id,
                                             work->module_version, work->mode);
            } catch (const std::exception& e) {
                spdlog::error("client_svc: ingest error run_id={}: {}",
                              work->run_id, e.what());
                // Do not abort the stream for a single failed ingest.
            }
        }
    }};

    // Outbound writer thread: drain out_queue -> stream->Write (serialized).
    std::thread out_writer{[out_queue, stream] {
        while (true) {
            auto msg = out_queue->dequeue();
            if (!msg.has_value()) break;  // queue closed
            if (!stream->Write(*msg)) {
                spdlog::warn("client_svc: stream->Write failed; outbound writer exiting");
                break;
            }
        }
    }};

    std::string registered_board_id;
    bool registered = false;

    acadia::stream::ClientMessage msg;
    while (!ctx->IsCancelled() && stream->Read(&msg)) {
        switch (msg.payload_case()) {
        case acadia::stream::ClientMessage::kHello: {
            const auto& hello = msg.hello();
            registered_board_id = hello.board_id();
            stream_registry_.register_stream(registered_board_id, out_queue);
            registered = true;
            spdlog::info("client_svc: HelloReport board_id={} client_ver={}",
                         hello.board_id(), hello.client_version());
            break;
        }

        case acadia::stream::ClientMessage::kResultBatch: {
            const auto& batch = msg.result_batch();
            handle_result_batch(batch, ingest_queue);

            // Send BatchAck via the outbound queue (serialized with run-control).
            acadia::stream::ServerMessage ack_msg;
            auto* ack = ack_msg.mutable_batch_ack();
            ack->set_run_id(batch.run_id());
            ack->set_batch_seq(batch.batch_seq());
            if (!out_queue->enqueue(ack_msg)) {
                spdlog::warn("client_svc: failed to enqueue BatchAck run_id={}",
                             batch.run_id());
            }

            // Consume batch.state(): map the client's RunState -> runs.status
            // instead of Acadia's blind complete_run("completed").
            if (batch.terminal()) {
                spdlog::info("client_svc: terminal batch run_id={} module_id={} state={}",
                             batch.run_id(), batch.module_id(),
                             static_cast<int>(batch.state()));
                try {
                    run_svc_.set_run_status(batch.run_id(), batch.state());
                } catch (const std::exception& e) {
                    spdlog::error("client_svc: set_run_status failed run_id={}: {}",
                                  batch.run_id(), e.what());
                }
            }
            break;
        }

        case acadia::stream::ClientMessage::kLogBatch: {
            const auto& batch = msg.log_batch();
            handle_log_batch(batch);

            acadia::stream::ServerMessage ack_msg;
            auto* ack = ack_msg.mutable_log_batch_ack();
            ack->set_run_id(batch.run_id());
            ack->set_batch_seq(batch.batch_seq());
            if (!out_queue->enqueue(ack_msg)) {
                spdlog::warn("client_svc: failed to enqueue LogBatchAck run_id={}",
                             batch.run_id());
            }
            break;
        }

        case acadia::stream::ClientMessage::kStateReport: {
            spdlog::debug("client_svc: StateReport board_id={} state={}",
                          msg.state_report().board_id(),
                          static_cast<int>(msg.state_report().overall_state()));
            break;
        }

        case acadia::stream::ClientMessage::kConfigureAck: {
            const auto& ack = msg.configure_ack();
            if (!ack.ok()) {
                spdlog::warn("client_svc: ConfigureAck NACK run_id={} field={} error={}",
                             ack.run_id(), ack.field(), ack.error());
            }
            break;
        }

        default:
            spdlog::debug("client_svc: unknown payload_case={}", msg.payload_case());
            break;
        }
    }

    // Deregister before closing the outbound queue so RunService stops pushing.
    if (registered) {
        stream_registry_.deregister_stream(registered_board_id, out_queue);
    }

    // Close both queues and join both writer threads (flush in-flight work).
    ingest_queue.close();
    out_queue->close();
    if (db_writer.joinable()) db_writer.join();
    if (out_writer.joinable()) out_writer.join();

    spdlog::info("client_svc: stream ended board_id={} (cancelled={})",
                 registered_board_id, ctx->IsCancelled());
    return ::grpc::Status::OK;
}

}  // namespace arches::server::grpc