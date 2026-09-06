#pragma once
// util/ResultConvert.h — conversion helpers between hal::common::TestResult
// proto and the arches::server::Result DB struct, plus the §5 module-Result
// unpack.
//
// Port of the prior reference implementation's util/ResultConvert.h with two
// changes:
//   1. No JsonCpp dependency — TypedValue/string-map JSON serialisation uses a
//      minimal built-in serializer (serialize direction only; the parse
//      direction used by the prior reference implementation's REST output is
//      Phase 2).
//   2. NEW: unpack_module_results() — the server-side inverse of the client's
//      pack_module_result (Arches-Client/src/hal_bridge/ResultSerializer.cc).
//      Given a google.protobuf.Any carrying hal.modules.<id>.Result
//      { repeated hal.common.TestResult results = 2; }, returns the list of
//      TestResults. Falls back to a single bare hal.common.TestResult for
//      unknown / proto-only modules. This is the §5-approved contract: the
//      client packs the module-specific Result, the server unpacks it via
//      reflection on the generated pool (no per-module headers).

#include "common.pb.h" // hal::common::TestResult, TypedValue, TestStatus, TestSeverity

#include <google/protobuf/any.h>
#include <google/protobuf/message.h>

#include <cstdint>
#include <string>
#include <vector>

namespace arches::server {
// Forward-declare to avoid circular include with
// repositories/ResultRepository.h.
struct Result;
} // namespace arches::server

namespace arches::server::util {

// ── Enum string helpers ──────────────────────────────────────────────────────

// Canonical uppercase string for a TestStatus integer. e.g. 1 -> "PASS".
// Unknown values return "UNSPECIFIED".
std::string status_to_string(std::int16_t status);
// Canonical uppercase string for a TestSeverity integer. e.g. 1 -> "INFO".
std::string severity_to_string(std::int16_t severity);
// Parse a status string (case-insensitive) back to an integer; 0 on miss.
std::int16_t string_to_status(const std::string& s);
// Parse a severity string (case-insensitive) back to an integer; 0 on miss.
std::int16_t string_to_severity(const std::string& s);

// ── TypedValue JSON serialisation (minimal, no JsonCpp) ──────────────────────

// Serialise a map<string, TypedValue> to a JSON string.
// Output: { "key": { "type": "double", "value": 42.5 }, ... }
std::string typed_map_to_json(const google::protobuf::Map<std::string, hal::common::TypedValue>& m);

// Serialise a map<string, string> (reason) to a JSON string.
// Output: { "key": "value", ... }
std::string string_map_to_json(const google::protobuf::Map<std::string, std::string>& m);

// ── Proto -> Result conversion ───────────────────────────────────────────────

// Convert a hal::common::TestResult proto to an arches::server::Result DB
// struct. parent_id is empty for top-level results; set by the recursive caller
// for subtests. The caller must set result.id before inserting (the server
// assigns ids).
Result proto_to_result(const hal::common::TestResult& tr, const std::string& run_id,
                       const std::string& module_version, const std::string& mode,
                       const std::string& parent_id = "");

// ── §5 module-Result unpack (NEW — server-side inverse of client pack) ──────

// Unpack the google.protobuf.Any payload of a ResultSample (or ResultEnvelope)
// into the list of hal::common::TestResult it carries.
//
// Contract (mirrors Arches-Client ResultSerializer::pack_module_result):
//   - type_url = "type.googleapis.com/hal.modules.<id>.Result"
//   - payload = serialized hal.modules.<id>.Result { repeated TestResult
//   results = 2; }
//   - For unknown / proto-only modules (no descriptor in the generated pool),
//     the client packs a bare hal.common.TestResult; this returns a 1-element
//     list in that case. If neither parses, returns an empty list.
//
// Relies on hal_protos_shared having registered every hal.modules.<id>.Result
// descriptor in DescriptorPool::generated_pool() (it does — the server links
// the same SHARED proto lib as the client).
std::vector<hal::common::TestResult> unpack_module_results(const google::protobuf::Any& payload,
                                                           const std::string& module_id);

// Build the type_url string for a module Result (mirror of the client helper).
std::string module_result_type_url(const std::string& module_id);

} // namespace arches::server::util