// config/Settings.cc — load Arches-Server runtime settings from ARCHES_* env
// vars (Step 7). Falls back to the struct defaults documented in Settings.h
// when an env var is unset/empty. No TOML, no config file — env-var only (per
// the plan: "Drop the TOML internal/config legacy stack entirely").

#include "config/Settings.h"

#include <cstdlib>
#include <string>

namespace arches::server::config {

namespace {

std::string env(const char *name, const std::string &fallback) {
  const char *v = std::getenv(name);
  if (v == nullptr || v[0] == '\0')
    return fallback;
  return std::string{v};
}

int env_int(const char *name, int fallback) {
  const char *v = std::getenv(name);
  if (v == nullptr || v[0] == '\0')
    return fallback;
  try {
    return std::stoi(v);
  } catch (...) {
    return fallback;
  }
}

std::size_t env_size(const char *name, std::size_t fallback) {
  const char *v = std::getenv(name);
  if (v == nullptr || v[0] == '\0')
    return fallback;
  try {
    return static_cast<std::size_t>(std::stoull(v));
  } catch (...) {
    return fallback;
  }
}

} // namespace

Settings load_settings() {
  Settings s;
  s.grpc_listen = env("ARCHES_GRPC_LISTEN", s.grpc_listen);
  s.db_path = env("ARCHES_DB_PATH", s.db_path);
  s.registry_path = env("ARCHES_REGISTRY_PATH", s.registry_path);
  s.stream_queue_size =
      env_size("ARCHES_STREAM_QUEUE_SIZE", s.stream_queue_size);
  s.log_level = env("ARCHES_LOG_LEVEL", s.log_level);
  s.shutdown_timeout_sec =
      env_int("ARCHES_SHUTDOWN_TIMEOUT_SEC", s.shutdown_timeout_sec);
  return s;
}

} // namespace arches::server::config