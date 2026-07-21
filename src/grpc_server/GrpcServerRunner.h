#pragma once
// grpc_server/GrpcServerRunner.h — owns and manages the gRPC server lifecycle.
// Ported from Acadia-Server; takes the flat arches::server::config::Settings
// (Acadia used a NetworkSettings sub-struct — dropped per the plan).

#include "grpc_server/HalServiceImpl.h"
#include "grpc_server/ClientServiceImpl.h"
#include "config/Settings.h"

#include <grpcpp/grpcpp.h>

#include <memory>
#include <string>

namespace arches::server::grpc {

class GrpcServerRunner {
public:
    GrpcServerRunner(HalServiceImpl&    hal_service,
                     ClientServiceImpl& client_service,
                     const config::Settings& settings);

    // Start the gRPC server.  Blocks until shutdown() is called.
    void start();

    // Signal graceful shutdown.
    void shutdown();

private:
    HalServiceImpl&           hal_service_;
    ClientServiceImpl&        client_service_;
    const config::Settings&   settings_;
    std::unique_ptr<::grpc::Server> server_;
};

}  // namespace arches::server::grpc