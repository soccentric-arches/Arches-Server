#pragma once
// services/LogService.h — log ingestion and query (Phase-1 subset).
//
// Ported from the prior reference implementation. Phase 1 keeps ingest + list;
// the live-tail subscriber broadcast and the per-user bookmark/saved-view
// surfaces (Dashboard, Phase 2) are dropped. LogService just forwards to
// LogRepository on SQLite.

#include "repositories/LogRepository.h"

#include <vector>

namespace arches::server {

class LogService {
public:
  LogService() = default;

  // Ingest a log entry (called from ClientServiceImpl's kLogBatch handler).
  LogEntry ingest(const LogEntry &e);

  std::vector<LogEntry> list(const LogFilter &f);

private:
  repositories::LogRepository log_repo_;
};

} // namespace arches::server