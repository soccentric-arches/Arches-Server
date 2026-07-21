#pragma once
// util/Time.h — timestamp helpers (port of Acadia-Server util/Time.h).
#include <chrono>
#include <string>

namespace arches::server::util {

using Clock     = std::chrono::system_clock;
using TimePoint = Clock::time_point;

// Returns current UTC time as an ISO-8601 string: "2026-01-02T15:04:05.000000Z".
std::string utc_now_iso8601();

// Format a TimePoint to ISO-8601 string.
std::string to_iso8601(TimePoint tp);

// Parse an ISO-8601 (UTC) string to TimePoint. Returns epoch on parse failure.
TimePoint parse_iso8601(const std::string& s);

}  // namespace arches::server::util