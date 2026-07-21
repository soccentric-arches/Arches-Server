#include "grpc_server/GrpcServerRunner.h"

#include <grpcpp/server_builder.h>
#include <spdlog/spdlog.h>

#include <stdexcept>

namespace arches::server::grpc {

GrpcServerRunner::GrpcServerRunner(HalServiceImpl&    hal_service,
                                    ClientServiceImpl& client_service,
                                    const config::Settings& settings)
    : hal_service_(hal_service),
      client_service_(client_service),
      settings_(settings) {}

void GrpcServerRunner::start() {
    ::grpc::ServerBuilder builder;
    builder.AddListeningPort(settings_.grpc_listen,
                             ::grpc::InsecureServerCredentials());
    builder.RegisterService(&hal_service_);
    builder.RegisterService(&client_service_);

    // Reuse port, keepalive tuning (mirrors Acadia).
    builder.SetMaxReceiveMessageSize(32 * 1024 * 1024);  // 32 MiB
    builder.SetMaxSendMessageSize(32 * 1024 * 1024);

    server_ = builder.BuildAndStart();
    if (!server_) {
        throw std::runtime_error("Failed to start gRPC server on " + settings_.grpc_listen);
    }
    spdlog::info("gRPC server listening on {} (HalService + ClientService)",
                 settings_.grpc_listen);
    server_->Wait();
}

void GrpcServerRunner::shutdown() {
    if (server_) {
        spdlog::info("gRPC server shutting down");
        server_->Shutdown();
    }
}

}  // namespace arches::server::grpc