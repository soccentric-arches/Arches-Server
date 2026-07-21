#include "repositories/TargetRepository.h"
#include "db/DbClient.h"
#include "util/Errors.h"
#include "util/Time.h"

#include <stdexcept>
#include <string>

namespace arches::server::repositories {

namespace {

using arches::server::db::PreparedStatement;

constexpr const char* kSelectColumns =
    "id, name, hostname, model, os_version, hal_version, client_version, "
    "tags, hal_module_inventory, first_seen, last_seen, status";

Board read_board(PreparedStatement& stmt) {
    Board b;
    b.id                       = stmt.column_text(0);
    b.name                     = stmt.column_text(1);
    b.hostname                 = stmt.column_is_null(2) ? "" : stmt.column_text(2);
    b.model                    = stmt.column_is_null(3) ? "" : stmt.column_text(3);
    b.os_version               = stmt.column_is_null(4) ? "" : stmt.column_text(4);
    b.hal_version              = stmt.column_is_null(5) ? "" : stmt.column_text(5);
    b.client_version           = stmt.column_is_null(6) ? "" : stmt.column_text(6);
    b.tags_json                = stmt.column_text(7);
    b.hal_module_inventory_json = stmt.column_text(8);
    b.first_seen               = stmt.column_text(9);
    b.last_seen                = stmt.column_text(10);
    b.status                   = stmt.column_text(11);
    return b;
}

}  // namespace

std::optional<Board> TargetRepository::get() {
    PreparedStatement stmt(std::string("SELECT ") + kSelectColumns +
                           " FROM board LIMIT 1");
    if (!stmt.step()) return std::nullopt;
    return read_board(stmt);
}

Board TargetRepository::create(const Board& b) {
    if (b.id.empty()) {
        throw arches::server::ValidationError("TargetRepository::create: board.id must be set");
    }
    // Guard the single-row constraint up front (the DB trigger also enforces it,
    // but a friendly ConflictError here gives the service layer a clear signal).
    if (get().has_value()) {
        throw arches::server::ConflictError("board already registered");
    }
    PreparedStatement stmt(
        "INSERT INTO board ("
        "  id, name, hostname, model, os_version, hal_version, client_version, "
        "  tags, hal_module_inventory, first_seen, last_seen, status"
        ") VALUES (?,?,?,?,?,?,?,?,?,?,?,?)");
    stmt.bind(1, b.id);
    stmt.bind(2, b.name);
    stmt.bind_text_or_null(3, b.hostname.empty() ? std::nullopt
                                                : std::optional<std::string>(b.hostname));
    stmt.bind_text_or_null(4, b.model.empty() ? std::nullopt
                                              : std::optional<std::string>(b.model));
    stmt.bind_text_or_null(5, b.os_version.empty() ? std::nullopt
                                                   : std::optional<std::string>(b.os_version));
    stmt.bind_text_or_null(6, b.hal_version.empty() ? std::nullopt
                                                    : std::optional<std::string>(b.hal_version));
    stmt.bind_text_or_null(7, b.client_version.empty() ? std::nullopt
                                                       : std::optional<std::string>(b.client_version));
    stmt.bind(8, b.tags_json.empty() ? "{}" : b.tags_json);
    stmt.bind(9, b.hal_module_inventory_json.empty() ? "[]" : b.hal_module_inventory_json);
    stmt.bind(10, b.first_seen.empty() ? arches::server::util::utc_now_iso8601() : b.first_seen);
    stmt.bind(11, b.last_seen.empty() ? arches::server::util::utc_now_iso8601() : b.last_seen);
    stmt.bind(12, b.status.empty() ? "offline" : b.status);
    stmt.step();
    return b;
}

void TargetRepository::update(const Board& b) {
    PreparedStatement stmt(
        "UPDATE board SET "
        "  name = ?, hostname = ?, model = ?, os_version = ?, hal_version = ?, "
        "  client_version = ?, tags = ?, status = ?, last_seen = CURRENT_TIMESTAMP "
        "WHERE id = ?");
    stmt.bind(1, b.name);
    stmt.bind_text_or_null(2, b.hostname.empty() ? std::nullopt
                                                : std::optional<std::string>(b.hostname));
    stmt.bind_text_or_null(3, b.model.empty() ? std::nullopt
                                             : std::optional<std::string>(b.model));
    stmt.bind_text_or_null(4, b.os_version.empty() ? std::nullopt
                                                  : std::optional<std::string>(b.os_version));
    stmt.bind_text_or_null(5, b.hal_version.empty() ? std::nullopt
                                                   : std::optional<std::string>(b.hal_version));
    stmt.bind_text_or_null(6, b.client_version.empty() ? std::nullopt
                                                       : std::optional<std::string>(b.client_version));
    stmt.bind(7, b.tags_json.empty() ? "{}" : b.tags_json);
    stmt.bind(8, b.status.empty() ? "offline" : b.status);
    stmt.bind(9, b.id);
    stmt.step();
}

void TargetRepository::heartbeat(const std::string& id, const std::string& status) {
    PreparedStatement stmt(
        "UPDATE board SET status = ?, last_seen = CURRENT_TIMESTAMP WHERE id = ?");
    stmt.bind(1, status.empty() ? "online" : status);
    stmt.bind(2, id);
    stmt.step();
}

void TargetRepository::update_inventory(const std::string& id,
                                        const std::string& inventory_json) {
    PreparedStatement stmt(
        "UPDATE board SET hal_module_inventory = ?, last_seen = CURRENT_TIMESTAMP WHERE id = ?");
    stmt.bind(1, inventory_json.empty() ? "[]" : inventory_json);
    stmt.bind(2, id);
    stmt.step();
}

}  // namespace arches::server::repositories