#pragma once
// grpc_server/GrpcServerRunner.h — owns and manages the gRPC server lifecycle.
// Ported from the prior reference implementation; takes the flat arches::server::config::Settings
// (the prior reference implementation used a NetworkSettings sub-struct — dropped per the plan).
//
// If Settings::grpc_listen ends in ":0", the kernel chooses a free port and
// the chosen port is published via port_future() (resolved after BuildAndStart,
// before Wait() blocks). This lets tests bind to ":0" (no pick_free_port TOCTOU
// race) and connect to the real port. If the server fails to start, port_future()
// delivers an exception instead — so a bind failure surfaces as a clean test
// failure rather than a null-deref segfault.

#include "grpc_server/HalServiceImpl.h"
#include "grpc_server/ClientServiceImpl.h"
#include "config/Settings.h"

#include <grpcpp/grpcpp.h>

#include <future>
#include <memory>
#include <string>

namespace arches::server::grpc {

class GrpcServerRunner {
public:
    GrpcServerRunner(HalServiceImpl&    hal_service,
                     ClientServiceImpl& client_service,
                     const config::Settings& settings);

    // Start the gRPC server.  Blocks until shutdown() is called.  Resolves
    // port_future() (value on success, exception on bind/build failure) before
    // blocking on Wait().
    void start();

    // Signal graceful shutdown.
    void shutdown();

    // A future that resolves to the actual bound port once the server is up.
    // get() throws std::runtime_error if the server failed to start. Safe to
    // ignore (never call get()) — e.g. main() with a fixed port doesn't need it.
    std::future<int>& port_future() { return port_future_; }

private:
    HalServiceImpl&           hal_service_;
    ClientServiceImpl&        client_service_;
    const config::Settings&   settings_;
    std::unique_ptr<::grpc::Server> server_;
    std::promise<int>         port_promise_;
    std::future<int>          port_future_;
};

}  // namespace arches::server::grpc