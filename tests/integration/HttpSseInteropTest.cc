// tests/integration/HttpSseInteropTest.cc — end-to-end proof of the SSE live-run
// endpoint over the real HTTP stack.
//
// Drives a run through the minimal run-control REST surface and asserts the
// loop's Server-side acceptance criteria:
//   (1) events arrive in order: run.start -> result -> run.complete,
//   (2) a second concurrent viewer on the same run gets the same run.complete,
//   (3) a stalled viewer never slows ingestion (the publish path is non-blocking).
//
// Mirrors ClientStreamInteropTest's harness (temp DB, ephemeral port, port_future,
// wait_for polls) but for the HTTP/SSE server. Uses httplib::Client (the same
// vendored cpp-httplib) for both the POST control calls and the SSE GET.
#include "config/Settings.h"
#include "db/DbClient.h"
#include "db/MigrationRunner.h"
#include "services/RunService.h"
#include "services/LogService.h"
#include "stream_registry/ClientStreamRegistry.h"
#include "stream_registry/RunStreamRegistry.h"
#include "http/HttpServer.h"

// Suppress the repo's strict warning flags around the vendored third-party
// header (it uses old-style casts we don't own). Scoped to this include.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#pragma GCC diagnostic ignored "-Wcast-align"
#pragma GCC diagnostic ignored "-Woverloaded-virtual"
#pragma GCC diagnostic ignored "-Wnull-dereference"
#include "httplib.h"
#pragma GCC diagnostic pop

#include <gtest/gtest.h>

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace arches::server;
using arches::server::grpc::ClientStreamRegistry;
using arches::server::grpc::RunStreamRegistry;
using arches::server::http::HttpServer;

namespace {

template <typename Pred>
bool wait_for(Pred pred, int timeout_ms = 5000) {
    for (int elapsed = 0; elapsed < timeout_ms; elapsed += 5) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

struct TempDb {
    std::string path;
    explicit TempDb(const std::string& tag) {
        path = "/tmp/arches_httpsse_" + tag + "_" + std::to_string(::getpid()) + ".db";
        remove_files();
        db::init_db_client(path);
        db::MigrationRunner{ARCHES_MIGRATIONS_DIR}.run();
    }
    ~TempDb() { db::shutdown_db_client(); remove_files(); }
    void remove_files() {
        ::unlink(path.c_str());
        ::unlink((path + "-wal").c_str());
        ::unlink((path + "-shm").c_str());
    }
    TempDb(const TempDb&) = delete;
    TempDb& operator=(const TempDb&) = delete;
};

// A thread-safe collector that parses SSE frames from a byte stream and stops
// after receiving a `run.complete` event. Used as httplib::Client's
// ContentReceiver for the SSE GET.
struct SseCollector {
    std::mutex m;
    std::vector<std::pair<std::string, std::string>> events;  // (name, data)
    std::string buf;
    std::atomic<bool> completed{false};
    std::atomic<bool> stopped{false};

    // Returns true to continue reading, false to stop (after run.complete).
    bool on_data(const char* data, size_t len) {
        if (stopped.load()) return false;
        buf.append(data, len);
        // SSE frames are separated by a blank line ("\n\n").
        size_t pos;
        while ((pos = buf.find("\n\n")) != std::string::npos) {
            std::string frame = buf.substr(0, pos);
            buf.erase(0, pos + 2);
            std::string name, dline;
            std::istringstream ss(frame);
            std::string line;
            while (std::getline(ss, line)) {
                if (line.rfind("event: ", 0) == 0) name = line.substr(7);
                else if (line.rfind("data: ", 0) == 0) dline = line.substr(6);
            }
            if (!name.empty()) {
                std::lock_guard<std::mutex> lk(m);
                events.emplace_back(name, dline);
                if (name == "run.complete") {
                    completed.store(true);
                    stopped.store(true);
                    return false;  // end the GET
                }
            }
        }
        return true;  // keep reading
    }

    std::vector<std::pair<std::string, std::string>> snapshot() {
        std::lock_guard<std::mutex> lk(m);
        return events;
    }
};

// RAII wrapper around a viewer thread: joins on destruction so an early
// ASSERT return never leaves a joinable std::thread (which would std::terminate).
// The join is bounded by the client's read timeout (the GET returns when the
// server closes the connection on stop, or after the read timeout).
struct Viewer {
    std::shared_ptr<SseCollector> col;
    std::thread t;
    explicit Viewer(std::shared_ptr<SseCollector> c, std::thread th)
        : col(std::move(c)), t(std::move(th)) {}
    ~Viewer() { if (t.joinable()) t.join(); }
    Viewer(const Viewer&) = delete;
    Viewer& operator=(const Viewer&) = delete;
};

}  // namespace

class HttpSseInteropTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() { spdlog::set_level(spdlog::level::err); }

    void SetUp() override {
        db_ = std::make_unique<TempDb>("srv");
        settings_.http_listen = "127.0.0.1:0";
        settings_.db_path = db_->path;
        settings_.registry_path = "<unused>";
        settings_.grpc_listen = "127.0.0.1:0";  // unused — no gRPC server here

        stream_registry_ = std::make_unique<ClientStreamRegistry>();
        run_stream_registry_ = std::make_unique<RunStreamRegistry>();
        run_svc_ = std::make_unique<RunService>(*stream_registry_, *run_stream_registry_);
        http_svc_ = std::make_unique<HttpServer>(*run_svc_, *run_stream_registry_, settings_);

        http_thread_ = std::make_unique<std::thread>([this] {
            try { http_svc_->start(); } catch (const std::exception& e) {
                spdlog::error("http test server error: {}", e.what());
            }
        });
        port_ = http_svc_->port_future().get();
        ASSERT_GT(port_, 0) << "HTTP server did not bind";
    }

    void TearDown() override {
        http_svc_->stop();
        if (http_thread_->joinable()) http_thread_->join();
        http_svc_.reset();
        run_svc_.reset();
        run_stream_registry_.reset();
        stream_registry_.reset();
        db_.reset();
    }

    std::string base() const {
        return "127.0.0.1:" + std::to_string(port_);
    }

    // Create a run via POST /api/v1/runs and return its id.
    std::string create_run(const std::string& mode = "single_shot") {
        httplib::Client cli(base());
        auto res = cli.Post("/api/v1/runs", std::string("{\"mode\":\"") + mode + "\"}",
                            "application/json");
        EXPECT_TRUE(res && res->status == 200) << "POST /runs failed";
        // Extract "id" from the JSON body (minimal extraction).
        auto body = res->body;
        auto p = body.find("\"id\":\"");
        EXPECT_NE(p, std::string::npos);
        p += 6;
        auto end = body.find('"', p);
        return body.substr(p, end - p);
    }

    void post_action(const std::string& run_id, const std::string& action,
                     const std::string& body = "{}") {
        httplib::Client cli(base());
        std::string path = "/api/v1/runs/" + run_id + "/" + action;
        auto res = cli.Post(path, body, "application/json");
        EXPECT_TRUE(res && res->status == 200)
            << "POST " << path << " -> " << (res ? res->status : -1);
    }

    // Open an SSE viewer in a background thread; returns a RAII Viewer whose
    // destructor joins the thread. The viewer stops after run.complete or when
    // the read timeout elapses (bounded so a failing test doesn't hang).
    Viewer open_viewer(const std::string& run_id, int read_timeout_ms = 4000) {
        auto col = std::make_shared<SseCollector>();
        std::thread t([this, run_id, col, read_timeout_ms] {
            httplib::Client cli(base());
            cli.set_read_timeout(read_timeout_ms, 0);
            std::string path = "/api/v1/runs/" + run_id + "/stream";
            cli.Get(path, [&col](const char* data, size_t len) {
                return col->on_data(data, len);
            });
        });
        return Viewer{col, std::move(t)};
    }

    std::unique_ptr<TempDb>            db_;
    config::Settings                   settings_;
    std::unique_ptr<ClientStreamRegistry>  stream_registry_;
    std::unique_ptr<RunStreamRegistry>     run_stream_registry_;
    std::unique_ptr<RunService>            run_svc_;
    std::unique_ptr<HttpServer>            http_svc_;
    std::unique_ptr<std::thread>           http_thread_;
    int port_ = 0;
};

// ── Acceptance: events arrive in order run.start -> result -> run.complete ──
TEST_F(HttpSseInteropTest, EventsArriveInOrder) {
    const std::string run_id = create_run("single_shot");

    // Open the viewer BEFORE driving so the live run.start (from /start) is
    // observed in real time (the on-connect replay also emits run.start for the
    // queued run; the live one overwrites it on the client side).
    Viewer v = open_viewer(run_id);
    auto& col = v.col;
    // Give the viewer a moment to connect + receive the on-connect replay.
    EXPECT_TRUE(wait_for([&] { return col->snapshot().size() >= 1 || col->stopped.load(); }));

    post_action(run_id, "start");
    post_action(run_id, "results",
                "{\"module_id\":\"cpu\",\"test_id\":\"temp\",\"status\":\"pass\","
                "\"name\":\"CPU Temp\",\"sequence\":1}");
    post_action(run_id, "complete", "{\"status\":\"completed\"}");

    EXPECT_TRUE(wait_for([&] { return col->completed.load(); })) << "run.complete never arrived";
    // ~Viewer joins the thread (bounded by read timeout).

    auto evs = col->snapshot();
    ASSERT_FALSE(evs.empty());
    // Find the indices of the key events.
    auto find_last = [&](const std::string& n) {
        for (auto it = evs.rbegin(); it != evs.rend(); ++it) {
            if (it->first == n) return static_cast<int>(evs.rend() - it - 1);
        }
        return -1;
    };
    int ix_start = find_last("run.start");
    int ix_result = find_last("result");
    int ix_complete = find_last("run.complete");
    EXPECT_GE(ix_start, 0);
    EXPECT_GE(ix_result, 0);
    EXPECT_GE(ix_complete, 0);
    EXPECT_LT(ix_start, ix_result);
    EXPECT_LT(ix_result, ix_complete);
    // The complete event carries the terminal status.
    EXPECT_NE(evs[ix_complete].second.find("\"status\":\"completed\""), std::string::npos);
}

// ── Acceptance: a second concurrent viewer gets the same run.complete ────────
TEST_F(HttpSseInteropTest, SecondConcurrentViewerGetsRunComplete) {
    const std::string run_id = create_run("single_shot");
    Viewer v1 = open_viewer(run_id);
    EXPECT_TRUE(wait_for([&] { return v1.col->snapshot().size() >= 1 || v1.col->stopped.load(); }));
    post_action(run_id, "start");
    post_action(run_id, "results",
                "{\"module_id\":\"mem\",\"test_id\":\"size\",\"status\":\"pass\","
                "\"name\":\"Mem Size\",\"sequence\":1}");

    // A second viewer joining after the run has started. The /complete may be
    // published before or after this viewer registers; either way it receives
    // run.complete — live if registered in time, otherwise via the on-connect
    // replay's terminal-run.complete emission.
    Viewer v2 = open_viewer(run_id);
    post_action(run_id, "complete", "{\"status\":\"completed\"}");

    EXPECT_TRUE(wait_for([&] { return v1.col->completed.load() && v2.col->completed.load(); },
                         10000));
    // ~Viewer joins both threads.

    auto evs1 = v1.col->snapshot();
    auto evs2 = v2.col->snapshot();
    auto has_complete = [](const auto& evs) {
        return std::any_of(evs.begin(), evs.end(),
                           [](const auto& e) { return e.first == "run.complete"; });
    };
    EXPECT_TRUE(has_complete(evs1));
    EXPECT_TRUE(has_complete(evs2));
}

// ── Acceptance: a stalled viewer never slows ingestion ───────────────────────
// Register a tiny undrained viewer queue directly with the registry (a stalled
// HTTP viewer is eventually dropped by TCP backpressure, which would conflate
// the assertion); then time RunService::inject_result — it must return promptly
// because the publish path is non-blocking (try_enqueue drops on full).
TEST_F(HttpSseInteropTest, StalledViewerNeverSlowsIngestion) {
    const std::string run_id = create_run("single_shot");
    // A stalled viewer with a 2-event queue that we never drain.
    auto stalled = std::make_shared<arches::server::grpc::BoundedEventQueue>(2);
    run_stream_registry_->register_stream(run_id, stalled);

    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 200; ++i) {
        run_svc_->inject_result(run_id, "cpu", "temp", "CPU Temp",
                                 /*status=*/1, /*sequence=*/i, "single_shot");
    }
    auto dt = std::chrono::steady_clock::now() - t0;
    // 200 inserts against a full stalled queue must complete well under a
    // second (try_enqueue drops + emits flow.paused, never blocks).
    EXPECT_LT(dt, std::chrono::seconds(1));
    // The stalled queue is bounded; it never grew past capacity + a marker.
    EXPECT_LE(stalled->size(), 3u);
    run_stream_registry_->deregister_stream(run_id, stalled);
    stalled->close();
}

// ── Reconnect replays current state without duplication ──────────────────────
// A viewer that connects AFTER results have landed sees them replayed in the
// on-connect run.start + result batch, then receives run.complete (live or via
// the terminal-replay).
TEST_F(HttpSseInteropTest, LateViewerReplaysExistingResults) {
    const std::string run_id = create_run("single_shot");
    post_action(run_id, "start");
    post_action(run_id, "results",
                "{\"module_id\":\"cpu\",\"test_id\":\"temp\",\"status\":\"pass\","
                "\"name\":\"CPU Temp\",\"sequence\":1}");

    // Wait for the result to be persisted before opening the viewer.
    ASSERT_TRUE(wait_for([&] { return run_svc_->count_results(run_id) >= 1; }));

    Viewer v = open_viewer(run_id);
    auto& col = v.col;
    // The replay should include at least one result immediately.
    EXPECT_TRUE(wait_for([&] {
        auto evs = col->snapshot();
        return std::any_of(evs.begin(), evs.end(),
                           [](const auto& e) { return e.first == "result"; });
    }));
    post_action(run_id, "complete", "{\"status\":\"completed\"}");
    EXPECT_TRUE(wait_for([&] { return col->completed.load(); }));

    // The replayed result must not be duplicated by a live result event (there
    // is only one result row; the seen-set dedups it).
    auto evs = col->snapshot();
    int result_count = std::count_if(evs.begin(), evs.end(),
                                     [](const auto& e) { return e.first == "result"; });
    EXPECT_EQ(result_count, 1);
}