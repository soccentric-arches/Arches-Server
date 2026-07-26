#pragma once
// util/Errors.h — common error types for Arches-Server.
// Port of the prior reference implementation's util/Errors.h (namespace renamed to arches::server).
// Used by repositories (ConflictError on duplicate board, NotFoundError on
// missing rows) and the service layer.

#include <stdexcept>
#include <string>

namespace arches::server {

// Base application error.
struct AppError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct NotFoundError : AppError {
    explicit NotFoundError(const std::string& msg) : AppError("not found: " + msg) {}
};

struct ValidationError : AppError {
    explicit ValidationError(const std::string& msg) : AppError("validation: " + msg) {}
};

struct ConflictError : AppError {
    explicit ConflictError(const std::string& msg) : AppError("conflict: " + msg) {}
};

struct ServiceUnavailableError : AppError {
    explicit ServiceUnavailableError(const std::string& msg)
        : AppError("service unavailable: " + msg) {}
};

}  // namespace arches::server