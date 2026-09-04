#include "grpc_server/HalServiceImpl.h"

#include "util/ResultConvert.h"
#include "util/Time.h"
#include "util/Uuid.h"

#include "common.pb.h"

#include <google/protobuf/any.pb.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <thread>

namespace arches::server::grpc {

HalServiceImpl::HalServiceImpl(RunService &run_svc, LogService &log_svc,
                               registry::ModuleRegistry &registry,
                               const config::Settings &settings)
    : run_svc_(run_svc), log_svc_(log_svc), registry_(registry),
      settings_(settings) {}

::grpc::Status HalServiceImpl::validate_module(const std::string &module_id) {
  if (module_id.empty()) {
    return ::grpc::Status{::grpc::StatusCode::INVALID_ARGUMENT,
                          "module_id is required"};
  }
  if (!registry_.has(module_id)) {
    return ::grpc::Status{::grpc::StatusCode::INVALID_ARGUMENT,
                          "unknown module_id: " + module_id};
  }
  return ::grpc::Status::OK;
}

// §5 persist: unpack the Any payload (module Result wrapper OR bare TestResult,
// both handled by util::unpack_module_results) and ingest each TestResult.
// Falls back to building a minimal Result from the envelope fields when nothing
// could be unpacked (graceful degradation, mirroring the prior reference
// implementation).
void HalServiceImpl::persist_result(const std::string &run_id,
                                    const hal::common::ResultEnvelope &env) {
  auto results = util::unpack_module_results(env.payload(), env.module_id());
  if (results.empty()) {
    spdlog::warn(
        "grpc: persist_result: nothing unpacked type_url='{}' run_id={}",
        env.payload().type_url(), run_id);
    Result r;
    r.run_id = run_id;
    r.module_id = env.module_id();
    r.module_version = env.module_version();
    r.timestamp = util::utc_now_iso8601();
    r.mode = std::to_string(static_cast<int>(env.mode()));
    r.status = static_cast<std::int16_t>(env.status());
    r.error_message = env.error_message();
    r.payload_type_url = env.payload().type_url();
    std::string serialized = env.payload().value();
    r.payload_bytes.assign(serialized.begin(), serialized.end());
    run_svc_.ingest_result(r);
    return;
  }

  // Prior reference-implementation quirk preserved: results.mode stores the
  // ExecutionMode int as a string.
  const std::string mode = std::to_string(static_cast<int>(env.mode()));
  const std::string module_version =
      env.module_version().empty() ? mode : env.module_version();

  for (auto &tr : results) {
    if (tr.output().status() == hal::common::TEST_STATUS_UNSPECIFIED) {
      spdlog::warn(
          "grpc: persist_result: UNSPECIFIED rejected run_id={} module_id={}",
          run_id, env.module_id());
      continue;
    }
    run_svc_.ingest_test_result(tr, run_id, module_version, mode);
  }
}

::grpc::Status
HalServiceImpl::RunSingleShot(::grpc::ServerContext * /*ctx*/,
                              const hal::SingleShotRequest *req,
                              hal::common::ResultEnvelope *resp) {

  auto status = validate_module(req->module_id());
  if (!status.ok())
    return status;

  spdlog::info("grpc: RunSingleShot module_id={}", req->module_id());

  // HalService RPCs carry no board_id; Phase 1 records the run with an empty
  // board_id (not pushed to any client stream). The HalService->ClientService
  // bridge is Phase 2.
  auto run = run_svc_.start_run(/*board_id=*/"", req->module_id(),
                                hal::common::SINGLE_SHOT, req->common(),
                                req->module_config(), "single_shot");
  run_svc_.mark_running(run.id);

  // Echo the request back as the result envelope and persist it.
  resp->set_module_id(req->module_id());
  resp->set_status(hal::common::PASS);
  persist_result(run.id, *resp);
  run_svc_.complete_run(run.id, "completed");

  return ::grpc::Status::OK;
}

::grpc::Status HalServiceImpl::StartLongTerm(
    ::grpc::ServerContext *ctx, const hal::LongTermRequest *req,
    ::grpc::ServerWriter<hal::common::ResultEnvelope> *writer) {

  auto status = validate_module(req->module_id());
  if (!status.ok())
    return status;

  auto run = run_svc_.start_run(/*board_id=*/"", req->module_id(),
                                hal::common::LONG_TERM, req->common(),
                                req->module_config(), "long_term");
  run_svc_.mark_running(run.id);
  spdlog::info("grpc: StartLongTerm run_id={} module_id={}", run.id,
               req->module_id());

  hal::common::ResultEnvelope ack;
  ack.set_module_id(req->module_id());
  ack.set_status(hal::common::PASS);
  writer->Write(ack);

  // Block until client cancels or server shutdown.
  while (!ctx->IsCancelled()) { // NOLINT: mirrors the prior reference
                                // implementation; ctx provided by caller
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  run_svc_.complete_run(run.id, "completed");
  return ::grpc::Status::OK;
}

::grpc::Status HalServiceImpl::StartMonitoring(
    ::grpc::ServerContext *ctx, const hal::MonitoringRequest *req,
    ::grpc::ServerWriter<hal::common::ResultEnvelope> *writer) {

  auto status = validate_module(req->module_id());
  if (!status.ok())
    return status;

  auto run = run_svc_.start_run(/*board_id=*/"", req->module_id(),
                                hal::common::MONITORING, req->common(),
                                req->module_config(), "monitoring");
  run_svc_.mark_running(run.id);
  spdlog::info("grpc: StartMonitoring run_id={} module_id={}", run.id,
               req->module_id());

  hal::common::ResultEnvelope ack;
  ack.set_module_id(req->module_id());
  ack.set_status(hal::common::PASS);
  writer->Write(ack);

  while (!ctx->IsCancelled()) { // NOLINT
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  run_svc_.complete_run(run.id, "completed");
  return ::grpc::Status::OK;
}

::grpc::Status HalServiceImpl::StartEventBased(
    ::grpc::ServerContext *ctx, const hal::EventBasedRequest *req,
    ::grpc::ServerWriter<hal::common::ResultEnvelope> *writer) {

  auto status = validate_module(req->module_id());
  if (!status.ok())
    return status;

  auto run = run_svc_.start_run(/*board_id=*/"", req->module_id(),
                                hal::common::EVENT_BASED, req->common(),
                                req->module_config(), "event_based");
  run_svc_.mark_running(run.id);
  spdlog::info("grpc: StartEventBased run_id={} module_id={}", run.id,
               req->module_id());

  hal::common::ResultEnvelope ack;
  ack.set_module_id(req->module_id());
  ack.set_status(hal::common::PASS);
  writer->Write(ack);

  while (!ctx->IsCancelled()) { // NOLINT
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  run_svc_.complete_run(run.id, "completed");
  return ::grpc::Status::OK;
}

::grpc::Status HalServiceImpl::Pause(::grpc::ServerContext * /*ctx*/,
                                     const hal::PauseRequest *req,
                                     hal::PauseResponse *resp) {

  spdlog::info("grpc: Pause module_id={} session_id={}", req->module_id(),
               req->session_id());
  run_svc_.pause_run(req->session_id());
  resp->set_accepted(true);
  return ::grpc::Status::OK;
}

::grpc::Status HalServiceImpl::Resume(::grpc::ServerContext * /*ctx*/,
                                      const hal::ResumeRequest *req,
                                      hal::ResumeResponse *resp) {

  spdlog::info("grpc: Resume module_id={} session_id={}", req->module_id(),
               req->session_id());
  run_svc_.resume_run(req->session_id());
  resp->set_accepted(true);
  return ::grpc::Status::OK;
}

::grpc::Status HalServiceImpl::Stop(::grpc::ServerContext * /*ctx*/,
                                    const hal::StopRequest *req,
                                    hal::StopResponse *resp) {

  spdlog::info("grpc: Stop module_id={} session_id={}", req->module_id(),
               req->session_id());
  run_svc_.stop_run(req->session_id());
  resp->set_accepted(true);
  return ::grpc::Status::OK;
}

::grpc::Status
HalServiceImpl::ListModules(::grpc::ServerContext * /*ctx*/,
                            const hal::ListModulesRequest * /*req*/,
                            hal::ListModulesResponse *resp) {

  for (const auto &m : registry_.all()) {
    auto *entry = resp->add_modules();
    entry->set_id(m.id);
    entry->set_proto_package(m.proto_package);
    entry->set_config_type(m.config_type_url);
    entry->set_result_type(m.result_type_url);
    // yaml `version` is rendered as a string in ModuleInfo; parse back to
    // int32.
    try {
      entry->set_version(std::stoi(m.version));
    } catch (...) {
      entry->set_version(0);
    }
    entry->set_status(m.status.empty() ? "active" : m.status);
    entry->set_source_path(m.source_path);
  }
  return ::grpc::Status::OK;
}

} // namespace arches::server::grpc