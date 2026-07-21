// tests/integration/ClientStreamInteropTest.cc — end-to-end client<->server
// interop proof (the deliverable of the whole port).
//
// Spins up the real gRPC server (GrpcServerRunner: HalService + ClientService)
// in-process against a temp SQLite DB. A mock gRPC client connects over the
// bidi ClientService::Connect stream and the test asserts the two gap-closures:
//   (1) Run-control push: RunService::start_run -> the client receives
//       ConfigureRun + StartRun on the stream (Acadia-Server never sent these).
//   (2) §5 module-Result unpack + persist + batch.state() mapping: the client
//       sends a terminal TestResultBatch carrying a hal.modules.cpu.Result
//       payload + state=COMPLETED; the server §5-unpacks it, persists a results
//       row, acks, and the runs row becomes "completed" (not hardcoded).
//   (3) CANCELLED mapping: stop_run -> StopRun delivered; a terminal batch with
//       state=CANCELLED -> runs.status "cancelled".
//
// Self-contained — does NOT need the real Arches-Client binary.

#include "config/Settings.h"
#include "db/DbClient.h"
#include "db/MigrationRunner.h"
#include "registry/ModuleRegistry.h"
#include "repositories/ResultRepository.h"
#include "services/RunService.h"
#include "services/LogService.h"
#include "stream_registry/ClientStreamRegistry.h"
#include "grpc_server/HalServiceImpl.h"
#include "grpc_server/ClientServiceImpl.h"
#include "grpc_server/GrpcServerRunner.h"

#include "client_service.grpc.pb.h"
#include "client_service.pb.h"
#include "common.pb.h"
#include "modules/cpu.pb.h"

#include <grpcpp/grpcpp.h>

#include <gtest/gtest.h>

#include <google/protobuf/any.h>
#include <google/protobuf/duration.pb.h>
#include <google/protobuf/timestamp.pb.h>

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <unistd.h>  // getpid, unlink (TempDb cleanup)

using namespace arches::server;
using arches::server::grpc::ClientStreamRegistry;
using arches::server::grpc::ClientServiceImpl;
using arches::server::grpc::HalServiceImpl;
using arches::server::grpc::GrpcServerRunner;
namespace s = acadia::stream;

namespace {

// Wait up to `timeout_ms` for `pred` to become true, polling every 5ms.
template <typename Pred>
bool wait_for(Pred pred, int timeout_ms = 4000) {
    for (int elapsed = 0; elapsed < timeout_ms; elapsed += 5) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

// A throwaway SQLite DB file (PID-based name, with -wal/-shm siblings).
struct TempDb {
    std::string path;
    explicit TempDb(const std::string& tag) {
        path = "/tmp/arches_interop_" + tag + "_" +
               std::to_string(::getpid()) + ".db";
        remove_files();
        db::init_db_client(path);
        db::MigrationRunner{ARCHES_MIGRATIONS_DIR}.run();
    }
    ~TempDb() {
        db::shutdown_db_client();
        remove_files();
    }
    void remove_files() {
        ::unlink(path.c_str());
        ::unlink((path + "-wal").c_str());
        ::unlink((path + "-shm").c_str());
    }
    TempDb(const TempDb&) = delete;
    TempDb& operator=(const TempDb&) = delete;
};

// Build a hal.modules.cpu.Result wrapping one TestResult, packed into the
// ResultSample payload Any — exactly what the Arches client emits (§5).
acadia::stream::ResultSample make_cpu_sample() {
    hal::common::TestResult tr;
    tr.set_module_id("cpu");
    tr.set_test_id("temp");
    tr.set_name("CPU Temp");
    tr.set_sequence(1);
    auto* ts = tr.mutable_timestamp();
    ts->set_seconds(1700000000);
    ts->set_nanos(0);
    auto* dur = tr.mutable_duration();
    dur->set_seconds(0);
    dur->set_nanos(2'000'000);
    auto* out = tr.mutable_output();
    out->set_status(hal::common::TEST_STATUS_PASS);
    out->set_severity(hal::common::TEST_SEVERITY_INFO);
    out->set_error_message("");
    hal::common::TypedValue temp_c;
    temp_c.set_double_value(55.0);
    (*out->mutable_metrics())["temp_c"] = temp_c;

    hal::modules::cpu::Result mod_result;
    *mod_result.add_results() = tr;

    acadia::stream::ResultSample sample;
    sample.mutable_payload()->PackFrom(mod_result);
    sample.set_type_url("type.googleapis.com/hal.modules.cpu.Result");
    sample.set_status(hal::common::TEST_STATUS_PASS);
    return sample;
}

}  // namespace

// Fixture: temp DB + full server stack up on an ephemeral port + a client stub.
class ClientStreamInteropTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        // Silence spdlog during the interop test (the server logs on its own).
        spdlog::set_level(spdlog::level::err);
    }

    void SetUp() override {
        db_ = std::make_unique<TempDb>("srv");

        // Bind to ":0" — the kernel chooses a free port and GrpcServerRunner
        // publishes it via port_future(). This eliminates the pick_free_port()
        // TOCTOU race (bind→close→return→server-bind) that caused intermittent
        // segfaults when prior test processes left ports in flux.
        settings_.grpc_listen = "127.0.0.1:0";
        settings_.db_path = db_->path;
        settings_.registry_path = "<unused>";  // empty registry — no HalService RPCs here

        // Full stack (mirrors main.cpp), minus loading modules.yaml.
        stream_registry_ = std::make_unique<ClientStreamRegistry>();
        run_svc_ = std::make_unique<RunService>(*stream_registry_);
        log_svc_ = std::make_unique<LogService>();
        modules_ = std::make_unique<registry::ModuleRegistry>();  // empty
        hal_svc_ = std::make_unique<HalServiceImpl>(*run_svc_, *log_svc_,
                                                      *modules_, settings_);
        client_svc_ = std::make_unique<ClientServiceImpl>(*run_svc_, *log_svc_,
                                                            *stream_registry_, 64);
        runner_ = std::make_unique<GrpcServerRunner>(*hal_svc_, *client_svc_, settings_);

        server_thread_ = std::make_unique<std::thread>([this] { runner_->start(); });

        // Block until the server has actually bound a port. port_future().get()
        // throws cleanly if BuildAndStart failed (instead of segfaulting on a
        // null server) — a bind failure becomes an explicit test failure.
        int port = runner_->port_future().get();
        ASSERT_NE(port, 0) << "server bound an unexpected port 0";

        // Client channel + stub.  (`::grpc` qualified — `using namespace
        // arches::server;` above would otherwise make bare `grpc::` ambiguous
        // with `arches::server::grpc`.)
        const std::string target = "127.0.0.1:" + std::to_string(port);
        channel_ = ::grpc::CreateChannel(target,
                                         ::grpc::InsecureChannelCredentials());
        ASSERT_TRUE(channel_->WaitForConnected(
            std::chrono::system_clock::now() + std::chrono::seconds(5)))
            << "client could not connect to " << target;
        stub_ = s::ClientService::NewStub(channel_);
    }

    void TearDown() override {
        stub_.reset();
        channel_.reset();
        runner_->shutdown();
        if (server_thread_->joinable()) server_thread_->join();
        hal_svc_.reset();
        client_svc_.reset();
        run_svc_.reset();
        log_svc_.reset();
        modules_.reset();
        stream_registry_.reset();
        runner_.reset();
        db_.reset();
    }

    // Open the bidi stream and send Hello; poll until the server has registered
    // the board (so a subsequent start_run's push will be delivered).
    std::unique_ptr<::grpc::ClientReaderWriterInterface<s::ClientMessage, s::ServerMessage>>
    connect_and_hello(const std::string& board_id) {
        auto ctx = std::make_unique<::grpc::ClientContext>();
        ctx->set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(8));
        auto stream = stub_->Connect(ctx.get());
        s::ClientMessage hello_msg;
        auto* hello = hello_msg.mutable_hello();
        hello->set_board_id(board_id);
        hello->set_client_version("interop-test");
        EXPECT_TRUE(stream->Write(hello_msg));
        // Wait for the server's Connect handler to process Hello + register.
        EXPECT_TRUE(wait_for([&] { return stream_registry_->has_stream(board_id); }))
            << "server did not register board " << board_id;
        // Stash the context so it lives as long as the stream (kept open).
        contexts_.push_back(std::move(ctx));
        return stream;
    }

    std::unique_ptr<TempDb> db_;
    config::Settings settings_;
    std::unique_ptr<ClientStreamRegistry> stream_registry_;
    std::unique_ptr<RunService> run_svc_;
    std::unique_ptr<LogService> log_svc_;
    std::unique_ptr<registry::ModuleRegistry> modules_;
    std::unique_ptr<HalServiceImpl> hal_svc_;
    std::unique_ptr<ClientServiceImpl> client_svc_;
    std::unique_ptr<GrpcServerRunner> runner_;
    std::unique_ptr<std::thread> server_thread_;
    std::shared_ptr<::grpc::Channel> channel_;
    std::unique_ptr<s::ClientService::Stub> stub_;
    std::vector<std::unique_ptr<::grpc::ClientContext>> contexts_;
};

// ── (1)+(2): run-control push + §5 unpack + COMPLETED mapping ───────────────────
TEST_F(ClientStreamInteropTest, StartRunPushesControlAndCompletedBatchMapsToStatus) {
    const std::string board = "interop-board-1";
    auto stream = connect_and_hello(board);

    // start_run pushes ConfigureRun + StartRun to the client stream.
    hal::common::CommonConfig common;
    common.set_interval_ms(100);
    google::protobuf::Any empty_module_config;  // cpu has no Config fields needed
    auto run = run_svc_->start_run(board, "cpu", hal::common::SINGLE_SHOT,
                                    common, empty_module_config, "interop-run");

    // Read ConfigureRun.
    s::ServerMessage msg1;
    ASSERT_TRUE(stream->Read(&msg1)) << "did not receive ConfigureRun";
    ASSERT_EQ(msg1.payload_case(), s::ServerMessage::kConfigureRun);
    EXPECT_EQ(msg1.configure_run().run_id(), run.id);
    EXPECT_EQ(msg1.configure_run().module_id(), "cpu");
    EXPECT_EQ(msg1.configure_run().mode(), hal::common::SINGLE_SHOT);

    // Read StartRun.
    s::ServerMessage msg2;
    ASSERT_TRUE(stream->Read(&msg2)) << "did not receive StartRun";
    ASSERT_EQ(msg2.payload_case(), s::ServerMessage::kStartRun);
    EXPECT_EQ(msg2.start_run().run_id(), run.id);

    // Client emits a terminal result batch with a §5 module-Result payload +
    // state=COMPLETED.
    s::ClientMessage batch_msg;
    auto* batch = batch_msg.mutable_result_batch();
    batch->set_run_id(run.id);
    batch->set_batch_seq(1);
    batch->set_module_id("cpu");
    batch->set_mode(hal::common::SINGLE_SHOT);
    batch->set_state(s::COMPLETED);
    batch->set_terminal(true);
    *batch->add_samples() = make_cpu_sample();
    ASSERT_TRUE(stream->Write(batch_msg));

    // Server sends a BatchAck.
    s::ServerMessage ack;
    ASSERT_TRUE(stream->Read(&ack)) << "did not receive BatchAck";
    ASSERT_EQ(ack.payload_case(), s::ServerMessage::kBatchAck);
    EXPECT_EQ(ack.batch_ack().run_id(), run.id);
    EXPECT_EQ(ack.batch_ack().batch_seq(), 1);

    // (2a) runs.status -> "completed" (the batch.state() mapping, not hardcoded).
    ASSERT_TRUE(wait_for([&] {
        try { return run_svc_->get_run(run.id).status == "completed"; }
        catch (...) { return false; }
    })) << "runs.status did not become completed";
    EXPECT_EQ(run_svc_->get_run(run.id).status, "completed");

    // (2b) the §5-unpacked TestResult was persisted (async DB writer thread).
    repositories::ResultRepository result_repo;
    ASSERT_TRUE(wait_for([&] { return result_repo.count_for_run(run.id) >= 1; }))
        << "result row was not persisted";
    auto summaries = run_svc_->get_module_summaries(run.id);
    ASSERT_FALSE(summaries.empty());
    EXPECT_EQ(summaries[0].module_id, "cpu");
    EXPECT_EQ(summaries[0].last_status, 1);  // PASS
}

// ── (3): stop_run pushes StopRun; a CANCELLED terminal batch maps to "cancelled" ─
TEST_F(ClientStreamInteropTest, StopRunPushesStopAndCancelledBatchMapsToStatus) {
    const std::string board = "interop-board-2";
    auto stream = connect_and_hello(board);

    hal::common::CommonConfig common;
    google::protobuf::Any empty_module_config;
    auto run = run_svc_->start_run(board, "cpu", hal::common::SINGLE_SHOT,
                                    common, empty_module_config, "interop-run2");

    // Drain ConfigureRun + StartRun.
    s::ServerMessage m;
    ASSERT_TRUE(stream->Read(&m));
    ASSERT_EQ(m.payload_case(), s::ServerMessage::kConfigureRun);
    ASSERT_TRUE(stream->Read(&m));
    ASSERT_EQ(m.payload_case(), s::ServerMessage::kStartRun);

    // stop_run pushes StopRun to the client.
    run_svc_->stop_run(run.id);
    ASSERT_TRUE(stream->Read(&m)) << "did not receive StopRun";
    ASSERT_EQ(m.payload_case(), s::ServerMessage::kStopRun);
    EXPECT_EQ(m.stop_run().run_id(), run.id);

    // Client emits a terminal batch with state=CANCELLED.
    s::ClientMessage batch_msg;
    auto* batch = batch_msg.mutable_result_batch();
    batch->set_run_id(run.id);
    batch->set_batch_seq(1);
    batch->set_module_id("cpu");
    batch->set_mode(hal::common::SINGLE_SHOT);
    batch->set_state(s::CANCELLED);
    batch->set_terminal(true);
    *batch->add_samples() = make_cpu_sample();
    ASSERT_TRUE(stream->Write(batch_msg));

    s::ServerMessage ack;
    ASSERT_TRUE(stream->Read(&ack)) << "did not receive BatchAck";
    ASSERT_EQ(ack.payload_case(), s::ServerMessage::kBatchAck);

    ASSERT_TRUE(wait_for([&] {
        try { return run_svc_->get_run(run.id).status == "cancelled"; }
        catch (...) { return false; }
    })) << "runs.status did not become cancelled";
    EXPECT_EQ(run_svc_->get_run(run.id).status, "cancelled");
}