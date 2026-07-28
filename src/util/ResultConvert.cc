#include "util/ResultConvert.h"
#include "repositories/ResultRepository.h"  // arches::server::Result
#include "repositories/RunRepository.h"     // arches::server::Run
#include "util/Time.h"

#include <google/protobuf/descriptor.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/reflection.h>

#include <algorithm>
#include <cstdio>
#include <sstream>
#include <utility>

namespace arches::server::util {

// ── Enum string helpers (port of the prior reference implementation, unchanged) ──────────────────────────

std::string status_to_string(std::int16_t status) {
    switch (status) {
    case 1: return "PASS";
    case 2: return "FAIL";
    case 3: return "ERROR";
    case 4: return "TIMEOUT";
    case 5: return "ABORTED";
    case 6: return "PARTIAL";   // deprecated alias — display only
    case 7: return "SKIPPED";
    default: return "UNSPECIFIED";
    }
}

std::string severity_to_string(std::int16_t severity) {
    switch (severity) {
    case 1: return "INFO";
    case 2: return "WARNING";
    case 3: return "ERROR";
    case 4: return "CRITICAL";
    default: return "UNSPECIFIED";
    }
}

std::int16_t string_to_status(const std::string& s) {
    std::string upper = s;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
    if (upper == "PASS")    return 1;
    if (upper == "FAIL")    return 2;
    if (upper == "ERROR")   return 3;
    if (upper == "TIMEOUT") return 4;
    if (upper == "ABORTED") return 5;
    if (upper == "SKIPPED") return 7;
    return 0;
}

std::int16_t string_to_severity(const std::string& s) {
    std::string upper = s;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
    if (upper == "INFO")     return 1;
    if (upper == "WARNING")  return 2;
    if (upper == "ERROR")    return 3;
    if (upper == "CRITICAL") return 4;
    return 0;
}

// ── Minimal JSON serialisation (replaces JsonCpp for Phase 1) ────────────────
namespace {

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (unsigned char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out;
}

std::string typed_value_to_json(const hal::common::TypedValue& tv) {
    switch (tv.value_case()) {
    case hal::common::TypedValue::kBoolValue:
        return std::string("{\"type\":\"bool\",\"value\":") +
               (tv.bool_value() ? "true" : "false") + "}";
    case hal::common::TypedValue::kIntValue: {
        std::ostringstream ss;
        ss << tv.int_value();
        return "{\"type\":\"int\",\"value\":" + ss.str() + "}";
    }
    case hal::common::TypedValue::kDoubleValue: {
        std::ostringstream ss;
        ss << tv.double_value();  // default precision is enough for metrics text
        std::string num = ss.str();
        // Ensure the literal looks numeric (has '.', 'e', or inf/nan) so it's valid JSON.
        if (num.find('.') == std::string::npos &&
            num.find('e') == std::string::npos &&
            num.find('E') == std::string::npos &&
            num.find("inf") == std::string::npos &&
            num.find("nan") == std::string::npos) {
            num += ".0";
        }
        return "{\"type\":\"double\",\"value\":" + num + "}";
    }
    case hal::common::TypedValue::kStringValue:
        return "{\"type\":\"string\",\"value\":\"" + json_escape(tv.string_value()) + "\"}";
    default:
        return "{\"type\":\"unspecified\",\"value\":null}";
    }
}

}  // namespace

std::string typed_map_to_json(
        const google::protobuf::Map<std::string, hal::common::TypedValue>& m) {
    if (m.empty()) return "{}";
    std::string out = "{";
    bool first = true;
    for (const auto& kv : m) {
        if (!first) out += ",";
        first = false;
        out += "\"" + json_escape(kv.first) + "\":" + typed_value_to_json(kv.second);
    }
    out += "}";
    return out;
}

std::string string_map_to_json(
        const google::protobuf::Map<std::string, std::string>& m) {
    if (m.empty()) return "{}";
    std::string out = "{";
    bool first = true;
    for (const auto& kv : m) {
        if (!first) out += ",";
        first = false;
        out += "\"" + json_escape(kv.first) + "\":\"" + json_escape(kv.second) + "\"";
    }
    out += "}";
    return out;
}

// ── Proto -> Result conversion ──────────────────────────────────────────────

Result proto_to_result(const hal::common::TestResult& tr,
                       const std::string& run_id,
                       const std::string& module_version,
                       const std::string& mode,
                       const std::string& parent_id) {
    Result r;
    // id intentionally left empty — ResultRepository::insert assigns it.
    r.run_id         = run_id;
    r.module_id      = tr.module_id();
    r.module_version = module_version;
    r.mode           = mode;
    r.parent_id      = parent_id.empty() ? std::nullopt
                                         : std::optional<std::string>{parent_id};

    r.test_id   = tr.test_id();
    r.test_name = tr.name();
    r.sequence  = static_cast<std::int64_t>(tr.sequence());

    if (tr.has_timestamp()) {
        auto tp = std::chrono::system_clock::time_point{
            std::chrono::seconds{tr.timestamp().seconds()} +
            std::chrono::nanoseconds{tr.timestamp().nanos()}};
        r.timestamp = to_iso8601(tp);
    } else {
        r.timestamp = utc_now_iso8601();
    }

    if (tr.has_duration()) {
        r.duration_ns = tr.duration().seconds() * 1'000'000'000LL + tr.duration().nanos();
    }

    const auto& out  = tr.output();
    r.status         = static_cast<std::int16_t>(out.status());
    r.severity       = static_cast<std::int16_t>(out.severity());
    r.error_message  = out.error_message();
    r.reason_json    = string_map_to_json(out.reason());
    r.metrics_json   = typed_map_to_json(out.metrics());
    r.monitoring_json = typed_map_to_json(out.monitoring());
    r.event_json     = typed_map_to_json(out.event());

    // payload_type_url / payload_bytes left empty here — set by the caller if it
    // wants to persist the packed module-Result envelope alongside the row.

    // Recurse into subtests so ResultRepository::insert can persist the whole
    // tree in one call. Subtests get parent_id="" here; insert_recursive sets
    // the real parent id from the parent's assigned id.
    r.subtests.reserve(tr.subtests_size());
    for (const auto& sub : tr.subtests()) {
        r.subtests.push_back(proto_to_result(sub, run_id, module_version, mode, ""));
    }
    return r;
}

// ── §5 module-Result unpack (server-side inverse of client pack) ─────────────

std::string module_result_type_url(const std::string& module_id) {
    return "type.googleapis.com/hal.modules." + module_id + ".Result";
}

std::vector<hal::common::TestResult> unpack_module_results(
    const google::protobuf::Any& payload,
    const std::string& module_id) {
    std::vector<hal::common::TestResult> out;
    if (payload.value().empty()) return out;

    // Resolve the message type name from the Any's type_url; fall back to the
    // module-derived name if the type_url is missing the standard prefix.
    static const std::string kPrefix = "type.googleapis.com/";
    std::string type_name;
    const std::string& tu = payload.type_url();
    if (!tu.empty() && tu.rfind(kPrefix, 0) == 0) {
        type_name = tu.substr(kPrefix.size());
    } else {
        type_name = "hal.modules." + module_id + ".Result";
    }

    // Try the module-specific Result { repeated TestResult results = 2; }.
    const auto* desc = google::protobuf::DescriptorPool::generated_pool()
                           ->FindMessageTypeByName(type_name);
    if (desc) {
        const auto* results_fld = desc->FindFieldByName("results");
        if (results_fld && results_fld->is_repeated() &&
            results_fld->type() ==
                google::protobuf::FieldDescriptor::TYPE_MESSAGE) {
            // NOTE: a function-local static factory is intentionally NOT used
            // here — DynamicMessageFactory is not guaranteed thread-safe across
            // concurrent GetPrototype/New calls. A fresh factory per call is
            // safe. Each repeated element is serialized to bytes and
            // ParseFromString'd into a *generated* TestResult rather than
            // CopyFrom'd from the DynamicMessage-backed sub-message: this never
            // crosses the DynamicMessage/generated representation boundary, so
            // the generated TestResult is always well-formed. (An earlier
            // hypothesis that CopyFrom caused intermittent TestResult corruption
            // here was a misdiagnosis — the corruption was an unrelated SQLite
            // connection-interleave race in DbClient, since fixed by lifetime-
            // locking PreparedStatement. The serialize/parse round-trip is kept
            // as a belt-and-braces measure regardless.)
            google::protobuf::DynamicMessageFactory factory;
            const auto* prototype = factory.GetPrototype(desc);
            if (prototype) {
                auto msg = std::unique_ptr<google::protobuf::Message>(prototype->New());
                if (msg->ParseFromString(payload.value())) {
                    const auto* refl = msg->GetReflection();
                    int n = refl->FieldSize(*msg, results_fld);
                    out.reserve(static_cast<std::size_t>(n));
                    for (int i = 0; i < n; ++i) {
                        const auto& sub = refl->GetRepeatedMessage(*msg, results_fld, i);
                        std::string bytes;
                        sub.SerializeToString(&bytes);
                        hal::common::TestResult tr;
                        tr.ParseFromString(bytes);
                        out.push_back(std::move(tr));
                    }
                    return out;
                }
            }
        }
    }

    // Fallback: bare hal.common.TestResult (unknown / proto-only module, or the
    // client packed a single TestResult rather than the module wrapper).
    hal::common::TestResult tr;
    if (tr.ParseFromString(payload.value())) {
        out.push_back(std::move(tr));
    }
    return out;
}

// ── JSON serialisation for the SSE live-run stream ───────────────────────────

std::string json_escape_string(const std::string& s) {
    return json_escape(s);
}

namespace {
// Emit a JSON object member `"key":value` with a leading comma when `first` is
// false, and flip `first` to false afterwards.
void emit_member(std::string& out, bool& first, const char* key, const std::string& value) {
    if (!first) out += ",";
    first = false;
    out += "\"";
    out += key;
    out += "\":";
    out += value;
}
std::string json_string(const std::string& s) {
    return "\"" + json_escape(s) + "\"";
}
std::string json_string_or_null(const std::string& s) {
    return s.empty() ? "null" : json_string(s);
}
// Embed an already-JSON string column (reason/metrics/monitoring/event) as a
// nested value, falling back to {} when empty so the field is always a valid
// JSON object on the wire.
std::string json_raw_object(const std::string& s) {
    return s.empty() ? "{}" : s;
}
}  // namespace

std::string result_to_json(const Result& r) {
    std::string out = "{";
    bool first = true;
    emit_member(out, first, "id",             json_string(r.id));
    emit_member(out, first, "run_id",         json_string(r.run_id));
    emit_member(out, first, "module_id",      json_string(r.module_id));
    emit_member(out, first, "module_version", json_string(r.module_version));
    emit_member(out, first, "test_id",        json_string(r.test_id));
    emit_member(out, first, "name",           json_string(r.test_name));
    emit_member(out, first, "sequence",       std::to_string(r.sequence));
    emit_member(out, first, "timestamp",      json_string_or_null(r.timestamp));
    emit_member(out, first, "duration_ns",    std::to_string(r.duration_ns));
    emit_member(out, first, "status",         json_string(status_to_string(r.status)));
    emit_member(out, first, "severity",       json_string(severity_to_string(r.severity)));
    emit_member(out, first, "mode",           json_string(r.mode));
    emit_member(out, first, "error_message",  json_string(r.error_message));
    emit_member(out, first, "reason",         json_raw_object(r.reason_json));
    emit_member(out, first, "metrics",        json_raw_object(r.metrics_json));
    emit_member(out, first, "monitoring",     json_raw_object(r.monitoring_json));
    emit_member(out, first, "event",          json_raw_object(r.event_json));
    emit_member(out, first, "parent_id",
                r.parent_id ? json_string(*r.parent_id) : "null");
    out += "}";
    return out;
}

std::string run_to_json(const Run& r) {
    std::string out = "{";
    bool first = true;
    emit_member(out, first, "id",           json_string(r.id));
    emit_member(out, first, "name",         json_string(r.name));
    emit_member(out, first, "mode",         json_string(r.mode));
    emit_member(out, first, "status",       json_string(r.status));
    emit_member(out, first, "board_id",     json_string_or_null(r.board_id));
    emit_member(out, first, "started_at",   json_string_or_null(r.started_at));
    emit_member(out, first, "ended_at",     json_string_or_null(r.ended_at));
    emit_member(out, first, "created_at",   json_string_or_null(r.created_at));
    emit_member(out, first, "error_message", json_string(r.error_message));
    out += "}";
    return out;
}

}  // namespace arches::server::util