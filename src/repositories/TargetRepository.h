#pragma once
// repositories/TargetRepository.h — SQLite port of the prior reference implementation's TargetRepository.
//
// The "target" is the single board under validation, registered when the client
// sends its first HelloReport on the ClientService stream. The server upserts/
// heartbeats this row as the client (re)connects and reports state.
//
// Phase 1 implements: get, create (throws ConflictError if a board is already
// registered — the board table is single-row), update, heartbeat,
// update_inventory. The remove() admin surface is deferred.

#include <optional>
#include <string>

namespace arches::server {

struct Board {
    std::string id;
    std::string name;
    std::string hostname;
    std::string model;
    std::string os_version;
    std::string hal_version;
    std::string client_version;
    std::string tags_json;                  // JSON object, default "{}"
    std::string hal_module_inventory_json;  // JSON array, default "[]"
    std::string first_seen;                  // ISO-8601
    std::string last_seen;                  // ISO-8601
    std::string status;                      // online | offline
};

}  // namespace arches::server

namespace arches::server::repositories {

class TargetRepository {
public:
    // The single registered board, or nullopt if none has registered yet.
    std::optional<Board> get();

    // Register the board. Throws ConflictError if a board is already registered
    // (the board table is single-row; see migration 0001's BEFORE INSERT trigger).
    Board create(const Board& b);

    // Patch the board's identity + software versions + tags + status.
    void update(const Board& b);

    // Bump last_seen to now and set online/offline status.
    void heartbeat(const std::string& id, const std::string& status);

    // Replace the HAL module inventory (reported by the client on connect).
    void update_inventory(const std::string& id, const std::string& inventory_json);
};

}  // namespace arches::server::repositories