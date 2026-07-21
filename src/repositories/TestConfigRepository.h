#pragma once
// repositories/TestConfigRepository.h — SQLite port of Acadia-Server's
// TestConfigRepository. Stores reusable test configurations (which modules,
// which mode, per-module params). RunService snapshots a TestConfig into
// runs.test_config_snapshot when starting a run.
//
// Phase 1 implements: create, find_by_id, list, update, archive, remove.
// (created_by user FK dropped in the SQLite migration 0002 — auth is Phase 2.)

#include <optional>
#include <string>
#include <vector>

namespace arches::server {

struct TestConfig {
    std::string id;
    std::string name;
    std::string description;
    std::string mode;                 // single_shot | long_term | monitoring | event_based
    std::string common_config_json;  // JSON object, default "{}"
    std::string modules_json;        // JSON array of per-module entries, default "[]"
    std::string created_at;          // ISO-8601
    bool is_archived = false;
};

}  // namespace arches::server

namespace arches::server::repositories {

class TestConfigRepository {
public:
    TestConfig create(const TestConfig& c);
    std::optional<TestConfig> find_by_id(const std::string& id);
    std::vector<TestConfig> list(bool include_archived = false);
    void update(const TestConfig& c);
    void archive(const std::string& id);
    void remove(const std::string& id);
};

}  // namespace arches::server::repositories