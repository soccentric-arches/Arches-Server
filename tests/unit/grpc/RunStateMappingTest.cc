// tests/unit/grpc/RunStateMappingTest.cc — assert the RunState -> runs.status
// mapping used by RunService::set_run_status (gap-closure #2: the server maps
// the client's RunState instead of hardcoding "completed"). Pure static
// methods — no DB, no gRPC.

#include "services/RunService.h"

#include "client_service.pb.h"

#include <gtest/gtest.h>

#include <set>
#include <string>

using arches::server::RunService;
namespace s = arches::stream;

TEST(RunStateMappingTest, MapsEachRunStateToStatus) {
  EXPECT_EQ(RunService::run_state_to_status(s::COMPLETED), "completed");
  EXPECT_EQ(RunService::run_state_to_status(s::FAILED), "failed");
  EXPECT_EQ(RunService::run_state_to_status(s::CANCELLED), "cancelled");
  EXPECT_EQ(RunService::run_state_to_status(s::PAUSED), "paused");
  EXPECT_EQ(RunService::run_state_to_status(s::RUNNING), "running");
  EXPECT_EQ(RunService::run_state_to_status(s::CONFIGURED), "queued");
  EXPECT_EQ(RunService::run_state_to_status(s::RUN_STATE_UNSPECIFIED),
            "unspecified");
}

TEST(RunStateMappingTest, TerminalStatesAreCompletedFailedCancelled) {
  EXPECT_TRUE(RunService::run_state_is_terminal(s::COMPLETED));
  EXPECT_TRUE(RunService::run_state_is_terminal(s::FAILED));
  EXPECT_TRUE(RunService::run_state_is_terminal(s::CANCELLED));
  EXPECT_FALSE(RunService::run_state_is_terminal(s::PAUSED));
  EXPECT_FALSE(RunService::run_state_is_terminal(s::RUNNING));
  EXPECT_FALSE(RunService::run_state_is_terminal(s::CONFIGURED));
  EXPECT_FALSE(RunService::run_state_is_terminal(s::RUN_STATE_UNSPECIFIED));
}

// The set_run_status contract: terminal states set ended_at; RUNNING sets
// started_at; PAUSED/CONFIGURED update status only. We assert the status string
// each enum resolves to so the wiring in RunService.cc stays consistent with
// the runs.status CHECK constraint (queued|running|paused|completed|failed|
// cancelled). UNSPECIFIED is rejected by the caller before reaching the repo.
TEST(RunStateMappingTest, AllNonUnspecifiedStatesMapToValidRunsStatus) {
  const std::set<std::string> valid = {"queued",    "running", "paused",
                                       "completed", "failed",  "cancelled"};
  for (auto st : {s::COMPLETED, s::FAILED, s::CANCELLED, s::PAUSED, s::RUNNING,
                  s::CONFIGURED}) {
    const std::string s2 = RunService::run_state_to_status(st);
    EXPECT_EQ(valid.count(s2), 1u)
        << "RunState " << static_cast<int>(st) << " mapped to '" << s2 << "'";
  }
  // UNSPECIFIED maps to a sentinel the caller rejects (not a runs.status).
  EXPECT_EQ(RunService::run_state_to_status(s::RUN_STATE_UNSPECIFIED),
            "unspecified");
}