#pragma once
// registry/ModuleRegistry.h — loads protos/registry/modules.yaml at startup.
// Ported from the prior reference implementation (YAML, no DB) and adapted to the Arches modules.yaml
// schema, which uses `result_type`/`config_type`/`proto_package`/`version`(int)/
// `status`/`category`/`source_path` — not the prior reference implementation's `result_type_url`/
// `config_type_url`. Thread-safe for concurrent reads after load().
//
// Carries `result_type_url` per module — the §5 dispatch key the server uses to
// pick the right hal.modules.<id>.Result descriptor when unpacking an Any payload.

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace arches::server::registry {

struct ModuleInfo {
    std::string id;
    std::string name;          // defaults to id (yaml has no name field)
    std::string description;   // empty (yaml has no description field)
    std::string version;       // yaml `version` (int) rendered as a string
    std::string category;      // e.g. "compute", "peripherals", "connectivity"
    std::string status;        // "active" | "deprecated"
    std::string proto_package;  // e.g. "hal.modules.cpu"
    std::string source_path;   // repo-relative path to the C++ header
    // proto type_url for the module's Result message (from yaml `result_type`).
    std::string result_type_url;
    // proto type_url for the module's Config message (from yaml `config_type`).
    std::string config_type_url;
    // Supported execution modes (yaml has no modes list; left empty).
    std::vector<std::string> modes;
};

class ModuleRegistry {
public:
    ModuleRegistry() = default;
    ~ModuleRegistry() = default;

    // Load modules.yaml from path.  Throws std::runtime_error on failure.
    void load(const std::string& path);

    // Returns true if the module_id is present in the registry.
    [[nodiscard]] bool has(std::string_view module_id) const;

    // Returns module info, or std::nullopt if not found.
    [[nodiscard]] std::optional<ModuleInfo> find(std::string_view module_id) const;

    // Returns all modules (read-only).
    [[nodiscard]] const std::vector<ModuleInfo>& all() const { return modules_; }

    [[nodiscard]] std::size_t size() const { return modules_.size(); }

private:
    std::unordered_map<std::string, std::size_t> index_;  // id → index in modules_
    std::vector<ModuleInfo>                       modules_;
};

}  // namespace arches::server::registry