#pragma once
// util/Uuid.h — UUID v4 generation (port of the prior reference implementation util/Uuid.h).
// Arches uses std::random_device instead of OpenSSL RAND_bytes to avoid pulling
// a crypto dependency just for non-secret run/result ids. std::random_device is
// non-deterministic on Linux (reads /dev/urandom). Run/result ids are not
// security-sensitive, so this is sufficient and dependency-light.

#include <string>
#include <string_view>

namespace arches::server::util {

// Generate a random UUID v4 string (lowercase hex, with hyphens).
std::string generate_uuid();

// Returns true if s is a well-formed UUID string.
bool is_valid_uuid(std::string_view s);

}  // namespace arches::server::util