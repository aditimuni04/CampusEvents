#pragma once
// Domain entities and the application error type.
#include <nlohmann/json.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>

#include "campusevents/util.hpp"

namespace ce {

using json = nlohmann::json;

// Every expected failure (bad input, missing record, rule violation) is an AppError.
// The REST layer maps `status` straight to the HTTP status code, so rules never know about HTTP.
class AppError : public std::runtime_error {
public:
    AppError(int status, std::string code, const std::string& message, json details = nullptr)
        : std::runtime_error(message), status_(status), code_(std::move(code)), details_(std::move(details)) {}

    int status() const { return status_; }
    const std::string& code() const { return code_; }
    const json& details() const { return details_; }

    static AppError invalid(const std::string& msg) { return AppError(400, "invalid_input", msg); }
    static AppError notFound(const std::string& msg) { return AppError(404, "not_found", msg); }
    static AppError conflict(const std::string& code, const std::string& msg, json details = nullptr) {
        return AppError(409, code, msg, std::move(details));
    }
    static AppError unauthorized(const std::string& msg) { return AppError(401, "unauthorized", msg); }

private:
    int status_;
    std::string code_;
    json details_;
};

// Raised for unexpected database failures (these become HTTP 500, never shown in detail to clients).
class DbError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

enum class RegStatus { Confirmed, Waiting };
inline const char* toString(RegStatus s) { return s == RegStatus::Confirmed ? "confirmed" : "waiting"; }

class Venue {
public:
    int64_t id = 0;
    std::string name;
    int capacity = 0;
    std::string location;

    json toJson() const { return {{"id", id}, {"name", name}, {"capacity", capacity}, {"location", location}}; }
};

class Event {
public:
    int64_t id = 0;
    std::string title;
    std::string category;
    std::string description;
    int64_t venueId = 0;
    int64_t start = 0;  // seconds since epoch (wall clock)
    int64_t end = 0;
    int capacity = 0;
    bool cancelled = false;

    Interval interval() const { return {start, end}; }
    bool clashesWith(const Event& other) const { return venueId == other.venueId && overlaps(interval(), other.interval()); }
    std::string timeText() const { return formatDateTime(start) + " - " + formatDateTime(end).substr(11); }
};

class Registration {
public:
    int64_t id = 0;  // increases with every sign-up, so ordering by id IS first-come-first-served
    int64_t eventId = 0;
    std::string studentId;
    std::string name;
    RegStatus status = RegStatus::Confirmed;
    bool checkedIn = false;
    int64_t createdAt = 0;
};

}  // namespace ce
