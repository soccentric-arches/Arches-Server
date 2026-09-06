#pragma once
// grpc_server/HalServiceImpl.h — implements the hal::HalService gRPC service.
//
// Ported from the prior reference implementation. Adapted for Arches Phase 1:
//   - ModuleService (REST, Phase 2) replaced by ModuleRegistry: validate_module
//     uses registry_.has(), ListModules reads registry_.all().
//   - start_run is board-routed: HalService RPCs carry no board_id, so the run
//   is
//     created with an empty board_id and NOT pushed to any client stream (the
//     HalService -> ClientService bridge is Phase 2). The §5 result persist
//     path still works end-to-end (persist_result -> ingest_test_result).
//   - persist_result uses the §5 unpack (util::unpack_module_results) for
//     consistency with the ClientService path, with the bare-TestResult
//     fallback handled inside the helper.
//
// Secondary for interop (the client<->server path is ClientService) but ported
// for parity so the §5 fix is consistent on both Any-payload paths.

#include "config/Settings.h"
#include "registry/ModuleRegistry.h"
#include "services/LogService.h"
#include "services/RunService.h"

#include "hal_service.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace arches::server::grpc {

class HalServiceImpl final : public hal::HalService::Service {
    public:
    HalServiceImpl(RunService& run_svc, LogService& log_svc, registry::ModuleRegistry& registry,
                   const config::Settings& settings);

    ::grpc::Status RunSingleShot(::grpc::ServerContext* ctx, const hal::SingleShotRequest* req,
                                 hal::common::ResultEnvelope* resp) override;

    ::grpc::Status
    StartLongTerm(::grpc::ServerContext* ctx, const hal::LongTermRequest* req,
                  ::grpc::ServerWriter<hal::common::ResultEnvelope>* writer) override;

    ::grpc::Status
    StartMonitoring(::grpc::ServerContext* ctx, const hal::MonitoringRequest* req,
                    ::grpc::ServerWriter<hal::common::ResultEnvelope>* writer) override;

    ::grpc::Status
    StartEventBased(::grpc::ServerContext* ctx, const hal::EventBasedRequest* req,
                    ::grpc::ServerWriter<hal::common::ResultEnvelope>* writer) override;

    ::grpc::Status Pause(::grpc::ServerContext* ctx, const hal::PauseRequest* req,
                         hal::PauseResponse* resp) override;

    ::grpc::Status Resume(::grpc::ServerContext* ctx, const hal::ResumeRequest* req,
                          hal::ResumeResponse* resp) override;

    ::grpc::Status Stop(::grpc::ServerContext* ctx, const hal::StopRequest* req,
                        hal::StopResponse* resp) override;

    ::grpc::Status ListModules(::grpc::ServerContext* ctx, const hal::ListModulesRequest* req,
                               hal::ListModulesResponse* resp) override;

    private:
    // Validate module_id against the registry. Returns non-OK if invalid.
    ::grpc::Status validate_module(const std::string& module_id);

    // §5 persist: unpack the Any payload (module Result or bare TestResult) and
    // ingest each TestResult via RunService::ingest_test_result.
    void persist_result(const std::string& run_id, const hal::common::ResultEnvelope& env);

    RunService& run_svc_;
    LogService& log_svc_;
    registry::ModuleRegistry& registry_;
    const config::Settings& settings_;

    // Active streaming sessions: run_id -> cancellation flag (unused in Phase 1
    // but kept for parity; the streaming RPCs block on ctx->IsCancelled()).
    std::mutex cancel_mutex_;
    std::unordered_map<std::string, std::unique_ptr<std::atomic<bool>>> cancel_flags_;
};

} // namespace arches::server::grpc