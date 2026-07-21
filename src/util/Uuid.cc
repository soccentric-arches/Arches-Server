#include "util/Uuid.h"

#include <cstdio>
#include <random>
#include <regex>

namespace arches::server::util {

std::string generate_uuid() {
    // 16 random bytes via std::random_device (no OpenSSL dependency).
    std::random_device rd;
    unsigned char bytes[16];
    for (int i = 0; i < 16; ++i) bytes[i] = static_cast<unsigned char>(rd());

    // RFC 4122 version 4 + variant bits.
    bytes[6] = (bytes[6] & 0x0f) | 0x40;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;

    char buf[37];
    std::snprintf(buf, sizeof(buf),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
        "%02x%02x%02x%02x%02x%02x",
        bytes[0],  bytes[1],  bytes[2],  bytes[3],
        bytes[4],  bytes[5],
        bytes[6],  bytes[7],
        bytes[8],  bytes[9],
        bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    return buf;
}

bool is_valid_uuid(std::string_view s) {
    static const std::regex kUuidRe{
        "[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}",
        std::regex::icase
    };
    return std::regex_match(std::string{s}, kUuidRe);
}

}  // namespace arches::server::util