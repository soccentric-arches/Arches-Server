#include "services/LogService.h"

namespace arches::server {

LogEntry LogService::ingest(const LogEntry& e) {
    return log_repo_.insert(e);
}

std::vector<LogEntry> LogService::list(const LogFilter& f) {
    return log_repo_.list(f);
}

}  // namespace arches::server