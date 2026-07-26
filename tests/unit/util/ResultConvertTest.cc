// tests/unit/util/ResultConvertTest.cc — unit tests for ResultConvert.
// Ports the prior reference implementation's ResultConvertTest (enum + JSON-serialise assertions,
// adapted to substring checks since Arches drops the JsonCpp parse direction)
// and ADDS the §5 module-Result unpack cases + a proto_to_result test.
#include "util/ResultConvert.h"
#include "repositories/ResultRepository.h"  // arches::server::Result (full def)

#include "common.pb.h"          // hal::common::TestResult, TypedValue, TestStatus
#include "modules/cpu.pb.h"     // hal::modules::cpu::Result (the §5 module wrapper)

#include <google/protobuf/any.h>
#include <google/protobuf/duration.pb.h>
#include <google/protobuf/timestamp.pb.h>

#include <gtest/gtest.h>

#include <string>

using namespace arches::server::util;

// ── status_to_string / string_to_status (port of the prior reference implementation) ─────────────────────

TEST(StatusConvertTest, ToStringRoundTrip) {
    EXPECT_EQ(status_to_string(1), "PASS");
    EXPECT_EQ(status_to_string(2), "FAIL");
    EXPECT_EQ(status_to_string(3), "ERROR");
    EXPECT_EQ(status_to_string(4), "TIMEOUT");
    EXPECT_EQ(status_to_string(5), "ABORTED");
    EXPECT_EQ(status_to_string(7), "SKIPPED");
    EXPECT_EQ(status_to_string(0), "UNSPECIFIED");
}

TEST(StatusConvertTest, FromStringRoundTrip) {
    EXPECT_EQ(string_to_status("PASS"), 1);
    EXPECT_EQ(string_to_status("FAIL"), 2);
    EXPECT_EQ(string_to_status("ERROR"), 3);
    EXPECT_EQ(string_to_status("TIMEOUT"), 4);
    EXPECT_EQ(string_to_status("ABORTED"), 5);
    EXPECT_EQ(string_to_status("SKIPPED"), 7);
    EXPECT_EQ(string_to_status("UNSPECIFIED"), 0);
    EXPECT_EQ(string_to_status("unknown"), 0);
}

TEST(StatusConvertTest, LowercaseInput) {
    EXPECT_EQ(string_to_status("pass"), 1);
    EXPECT_EQ(string_to_status("fail"), 2);
}

// ── severity_to_string / string_to_severity (port of the prior reference implementation) ─────────────────

TEST(SeverityConvertTest, ToStringRoundTrip) {
    EXPECT_EQ(severity_to_string(1), "INFO");
    EXPECT_EQ(severity_to_string(2), "WARNING");
    EXPECT_EQ(severity_to_string(3), "ERROR");
    EXPECT_EQ(severity_to_string(4), "CRITICAL");
    EXPECT_EQ(severity_to_string(0), "UNSPECIFIED");
}

TEST(SeverityConvertTest, FromStringRoundTrip) {
    EXPECT_EQ(string_to_severity("INFO"), 1);
    EXPECT_EQ(string_to_severity("WARNING"), 2);
    EXPECT_EQ(string_to_severity("ERROR"), 3);
    EXPECT_EQ(string_to_severity("CRITICAL"), 4);
    EXPECT_EQ(string_to_severity("UNSPECIFIED"), 0);
}

// ── typed_map_to_json / string_map_to_json ───────────────────────────────────
// Arches has no JsonCpp parse direction (Phase 2), so these assert on the
// serialized string directly. protobuf Map iteration order is unspecified, so
// each key/type/value is checked as an independent substring.

TEST(TypedMapTest, DoubleValue) {
    google::protobuf::Map<std::string, hal::common::TypedValue> m;
    hal::common::TypedValue tv;
    tv.set_double_value(42.5);
    m["throughput"] = tv;
    const std::string s = typed_map_to_json(m);
    EXPECT_NE(s.find("\"throughput\""), std::string::npos);
    EXPECT_NE(s.find("\"type\":\"double\""), std::string::npos);
    EXPECT_NE(s.find("\"value\":42.5"), std::string::npos);
}

TEST(TypedMapTest, IntValue) {
    google::protobuf::Map<std::string, hal::common::TypedValue> m;
    hal::common::TypedValue tv;
    tv.set_int_value(1234);
    m["count"] = tv;
    const std::string s = typed_map_to_json(m);
    EXPECT_NE(s.find("\"count\""), std::string::npos);
    EXPECT_NE(s.find("\"type\":\"int\""), std::string::npos);
    EXPECT_NE(s.find("\"value\":1234"), std::string::npos);
}

TEST(TypedMapTest, StringValue) {
    google::protobuf::Map<std::string, hal::common::TypedValue> m;
    hal::common::TypedValue tv;
    tv.set_string_value("hello");
    m["label"] = tv;
    const std::string s = typed_map_to_json(m);
    EXPECT_NE(s.find("\"label\""), std::string::npos);
    EXPECT_NE(s.find("\"type\":\"string\""), std::string::npos);
    EXPECT_NE(s.find("\"value\":\"hello\""), std::string::npos);
}

TEST(TypedMapTest, BoolValue) {
    google::protobuf::Map<std::string, hal::common::TypedValue> m;
    hal::common::TypedValue tv;
    tv.set_bool_value(true);
    m["ok"] = tv;
    const std::string s = typed_map_to_json(m);
    EXPECT_NE(s.find("\"ok\""), std::string::npos);
    EXPECT_NE(s.find("\"type\":\"bool\""), std::string::npos);
    EXPECT_NE(s.find("\"value\":true"), std::string::npos);
}

TEST(TypedMapTest, EmptyMapSerialisesToObject) {
    google::protobuf::Map<std::string, hal::common::TypedValue> m;
    EXPECT_EQ(typed_map_to_json(m), "{}");
}

TEST(StringMapTest, ReasonMap) {
    google::protobuf::Map<std::string, std::string> m;
    m["why"] = "threshold exceeded";
    m["limit"] = "80C";
    const std::string s = string_map_to_json(m);
    EXPECT_NE(s.find("\"why\":\"threshold exceeded\""), std::string::npos);
    EXPECT_NE(s.find("\"limit\":\"80C\""), std::string::npos);
    EXPECT_EQ(string_map_to_json(google::protobuf::Map<std::string, std::string>{}), "{}");
}

// ── proto_to_result ──────────────────────────────────────────────────────────

namespace {

hal::common::TestResult make_test_result(const std::string& test_id,
                                          const std::string& name,
                                          std::uint64_t seq,
                                          hal::common::TestStatus status,
                                          hal::common::TestSeverity severity) {
    hal::common::TestResult tr;
    tr.set_module_id("cpu");
    tr.set_test_id(test_id);
    tr.set_name(name);
    tr.set_sequence(seq);
    auto* ts = tr.mutable_timestamp();
    ts->set_seconds(1700000000);
    ts->set_nanos(500000000);
    auto* dur = tr.mutable_duration();
    dur->set_seconds(0);
    dur->set_nanos(2'000'000);
    auto* out = tr.mutable_output();
    out->set_status(status);
    out->set_severity(severity);
    out->set_error_message("");
    (*out->mutable_reason())["why"] = "ok";
    hal::common::TypedValue temp_c;
    temp_c.set_double_value(55.0);
    (*out->mutable_metrics())["temp_c"] = temp_c;
    return tr;
}

}  // namespace

TEST(ProtoToResultTest, MapsFields) {
    auto tr = make_test_result("temp", "CPU Temp", 1,
                               hal::common::TEST_STATUS_PASS,
                               hal::common::TEST_SEVERITY_INFO);
    auto r = proto_to_result(tr, "run-1", "1.0.0", "single_shot");
    EXPECT_EQ(r.run_id, "run-1");
    EXPECT_EQ(r.module_id, "cpu");
    EXPECT_EQ(r.module_version, "1.0.0");
    EXPECT_EQ(r.mode, "single_shot");
    EXPECT_EQ(r.test_id, "temp");
    EXPECT_EQ(r.test_name, "CPU Temp");
    EXPECT_EQ(r.sequence, 1);
    EXPECT_EQ(r.status, 1);
    EXPECT_EQ(r.severity, 1);
    EXPECT_EQ(r.duration_ns, 2'000'000LL);
    EXPECT_FALSE(r.timestamp.empty());
    EXPECT_NE(r.reason_json.find("\"why\":\"ok\""), std::string::npos);
    EXPECT_NE(r.metrics_json.find("\"temp_c\""), std::string::npos);
    EXPECT_NE(r.metrics_json.find("55"), std::string::npos);
    EXPECT_TRUE(r.parent_id == std::nullopt);  // top-level
    EXPECT_TRUE(r.id.empty());                  // id assigned by the repo, not here
    EXPECT_TRUE(r.subtests.empty());
}

TEST(ProtoToResultTest, RecursesIntoSubtests) {
    auto tr = make_test_result("parent", "Parent", 1,
                               hal::common::TEST_STATUS_PASS,
                               hal::common::TEST_SEVERITY_INFO);
    auto* sub = tr.add_subtests();
    sub->set_module_id("cpu");
    sub->set_test_id("child");
    sub->set_name("Child");
    sub->set_sequence(2);
    sub->mutable_output()->set_status(hal::common::TEST_STATUS_FAIL);
    sub->mutable_output()->set_severity(hal::common::TEST_SEVERITY_WARNING);

    auto r = proto_to_result(tr, "run-1", "1.0.0", "single_shot");
    ASSERT_EQ(r.subtests.size(), 1u);
    EXPECT_EQ(r.subtests[0].test_id, "child");
    EXPECT_EQ(r.subtests[0].status, 2);  // FAIL
    EXPECT_EQ(r.subtests[0].severity, 2);
    // Subtest parent_id left empty here; ResultRepository::insert sets the real
    // parent from the parent's assigned id.
    EXPECT_TRUE(r.subtests[0].parent_id == std::nullopt);
}

// ── §5 module-Result unpack (the core of the client↔server contract) ─────────

TEST(UnpackModuleResultsTest, UnpacksModuleWrapperWithMultipleResults) {
    hal::modules::cpu::Result mod_result;
    auto* r1 = mod_result.add_results();
    *r1 = make_test_result("temp", "CPU Temp", 1,
                           hal::common::TEST_STATUS_PASS,
                           hal::common::TEST_SEVERITY_INFO);
    auto* r2 = mod_result.add_results();
    *r2 = make_test_result("freq", "CPU Freq", 2,
                           hal::common::TEST_STATUS_FAIL,
                           hal::common::TEST_SEVERITY_WARNING);

    google::protobuf::Any any;
    ASSERT_TRUE(any.PackFrom(mod_result));
    EXPECT_EQ(any.type_url(), "type.googleapis.com/hal.modules.cpu.Result");

    auto got = unpack_module_results(any, "cpu");
    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(got[0].test_id(), "temp");
    EXPECT_EQ(got[0].name(), "CPU Temp");
    EXPECT_EQ(got[0].output().status(), hal::common::TEST_STATUS_PASS);
    EXPECT_EQ(got[0].module_id(), "cpu");
    EXPECT_EQ(got[1].test_id(), "freq");
    EXPECT_EQ(got[1].output().status(), hal::common::TEST_STATUS_FAIL);
    EXPECT_EQ(got[1].output().severity(), hal::common::TEST_SEVERITY_WARNING);
}

TEST(UnpackModuleResultsTest, EmptyWrapperReturnsZeroResults) {
    hal::modules::cpu::Result mod_result;  // no results added
    google::protobuf::Any any;
    ASSERT_TRUE(any.PackFrom(mod_result));
    auto got = unpack_module_results(any, "cpu");
    EXPECT_TRUE(got.empty());
}

TEST(UnpackModuleResultsTest, BareTestResultFallback) {
    // An unknown / proto-only module packs a bare hal.common.TestResult (no
    // wrapper). The type_url is hal.common.TestResult, which has no `results`
    // field, so unpack falls through to the bare-TestResult path: 1 element.
    auto tr = make_test_result("only", "Lonely Test", 1,
                               hal::common::TEST_STATUS_PASS,
                               hal::common::TEST_SEVERITY_INFO);
    google::protobuf::Any any;
    ASSERT_TRUE(any.PackFrom(tr));
    EXPECT_EQ(any.type_url(), "type.googleapis.com/hal.common.TestResult");

    auto got = unpack_module_results(any, "unknownmod");
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0].test_id(), "only");
    EXPECT_EQ(got[0].module_id(), "cpu");  // preserved from the TestResult
}

TEST(UnpackModuleResultsTest, EmptyPayloadReturnsZero) {
    google::protobuf::Any any;  // value() is empty
    auto got = unpack_module_results(any, "cpu");
    EXPECT_TRUE(got.empty());
}

TEST(ModuleResultTypeUrlTest, MatchesClientConvention) {
    EXPECT_EQ(module_result_type_url("cpu"),
              "type.googleapis.com/hal.modules.cpu.Result");
}