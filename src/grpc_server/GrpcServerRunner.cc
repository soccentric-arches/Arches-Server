#include "grpc_server/GrpcServerRunner.h"

#include <grpcpp/server_builder.h>
#include <spdlog/spdlog.h>

#include <stdexcept>

namespace arches::server::grpc {

GrpcServerRunner::GrpcServerRunner(HalServiceImpl &hal_service,
                                   ClientServiceImpl &client_service,
                                   const config::Settings &settings)
    : hal_service_(hal_service), client_service_(client_service),
      settings_(settings), port_future_(port_promise_.get_future()) {}

void GrpcServerRunner::start() {
  ::grpc::ServerBuilder builder;
  // selected_port receives the actual bound port (meaningful when
  // grpc_listen ends in ":0"; otherwise it echoes the configured port).
  int selected_port = 0;
  builder.AddListeningPort(settings_.grpc_listen,
                           ::grpc::InsecureServerCredentials(), &selected_port);
  builder.RegisterService(&hal_service_);
  builder.RegisterService(&client_service_);

  // Reuse port, keepalive tuning (mirrors the prior reference implementation).
  builder.SetMaxReceiveMessageSize(32 * 1024 * 1024); // 32 MiB
  builder.SetMaxSendMessageSize(32 * 1024 * 1024);

  server_ = builder.BuildAndStart();
  if (!server_) {
    // Fulfil the promise with an exception so any waiter on port_future()
    // gets a clean failure instead of blocking forever or dereferencing a
    // null server. Return without Wait() — server_ is null.
    port_promise_.set_exception(std::make_exception_ptr(std::runtime_error(
        "Failed to start gRPC server on " + settings_.grpc_listen)));
    return;
  }
  spdlog::info("gRPC server listening on {} (actual port {}, HalService + "
               "ClientService)",
               settings_.grpc_listen, selected_port);
  port_promise_.set_value(selected_port);
  server_->Wait();
}

void GrpcServerRunner::shutdown() {
  if (server_) {
    spdlog::info("gRPC server shutting down");
    server_->Shutdown();
  }
}

} // namespace arches::server::grpc