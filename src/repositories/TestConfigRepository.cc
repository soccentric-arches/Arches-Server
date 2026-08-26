#include "repositories/TestConfigRepository.h"
#include "db/DbClient.h"
#include "util/Errors.h"
#include "util/Time.h"

#include <stdexcept>
#include <string>

namespace arches::server::repositories {

namespace {

using arches::server::db::PreparedStatement;

constexpr const char* kSelectColumns =
    "id, name, description, mode, common_config, modules, created_at, is_archived";

TestConfig read_config(const PreparedStatement& stmt) {
    TestConfig c;
    c.id                  = stmt.column_text(0);
    c.name                = stmt.column_text(1);
    c.description         = stmt.column_text(2);
    c.mode                = stmt.column_text(3);
    c.common_config_json = stmt.column_text(4);
    c.modules_json        = stmt.column_text(5);
    c.created_at          = stmt.column_text(6);
    c.is_archived         = stmt.column_int(7) != 0;
    return c;
}

}  // namespace

TestConfig TestConfigRepository::create(const TestConfig& c) {
    if (c.id.empty()) {
        throw arches::server::ValidationError("TestConfigRepository::create: config.id must be set");
    }
    PreparedStatement stmt(
        "INSERT INTO test_configs ("
        "  id, name, description, mode, common_config, modules, created_at, is_archived"
        ") VALUES (?,?,?,?,?,?,?,0)");
    stmt.bind(1, c.id);
    stmt.bind(2, c.name);
    stmt.bind(3, c.description);
    stmt.bind(4, c.mode.empty() ? "single_shot" : c.mode);
    stmt.bind(5, c.common_config_json.empty() ? "{}" : c.common_config_json);
    stmt.bind(6, c.modules_json.empty() ? "[]" : c.modules_json);
    stmt.bind(7, c.created_at.empty() ? arches::server::util::utc_now_iso8601() : c.created_at);
    stmt.step();
    return c;
}

std::optional<TestConfig> TestConfigRepository::find_by_id(const std::string& id) {
    PreparedStatement stmt(std::string("SELECT ") + kSelectColumns +
                           " FROM test_configs WHERE id = ?");
    stmt.bind(1, id);
    if (!stmt.step()) return std::nullopt;
    return read_config(stmt);
}

std::vector<TestConfig> TestConfigRepository::list(bool include_archived) {
    std::string sql = std::string("SELECT ") + kSelectColumns + " FROM test_configs";
    if (!include_archived) sql += " WHERE is_archived = 0";
    sql += " ORDER BY created_at DESC";

    PreparedStatement stmt(sql);
    std::vector<TestConfig> out;
    while (stmt.step()) out.push_back(read_config(stmt));
    return out;
}

void TestConfigRepository::update(const TestConfig& c) {
    PreparedStatement stmt(
        "UPDATE test_configs SET "
        "  name = ?, description = ?, mode = ?, common_config = ?, modules = ?, is_archived = ? "
        "WHERE id = ?");
    stmt.bind(1, c.name);
    stmt.bind(2, c.description);
    stmt.bind(3, c.mode.empty() ? "single_shot" : c.mode);
    stmt.bind(4, c.common_config_json.empty() ? "{}" : c.common_config_json);
    stmt.bind(5, c.modules_json.empty() ? "[]" : c.modules_json);
    stmt.bind(6, c.is_archived ? 1 : 0);
    stmt.bind(7, c.id);
    stmt.step();
}

void TestConfigRepository::archive(const std::string& id) {
    PreparedStatement stmt("UPDATE test_configs SET is_archived = 1 WHERE id = ?");
    stmt.bind(1, id);
    stmt.step();
}

void TestConfigRepository::remove(const std::string& id) {
    PreparedStatement stmt("DELETE FROM test_configs WHERE id = ?");
    stmt.bind(1, id);
    stmt.step();
}

}  // namespace arches::server::repositories