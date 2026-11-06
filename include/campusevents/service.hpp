#pragma once
// EventService: the business rules. Every public method is one atomic operation:
// it takes the service lock, opens a database transaction, checks the rules, changes data, commits.
#include <mutex>
#include <string>

#include "campusevents/repository.hpp"

namespace ce {

struct NewVenue {
    std::string name;
    int capacity = 0;
    std::string location;
};

struct NewEvent {
    std::string title;
    std::string category;
    std::string description;
    int64_t venueId = 0;
    std::string start;  // "YYYY-MM-DD HH:MM"
    std::string end;
    int capacity = 0;
};

// Raw (still unvalidated) search input as it arrives from the API.
struct EventQuery {
    std::string text, category, status = "active", sort = "start", order = "asc";
    int64_t venueId = 0;
    std::string from, to;  // "YYYY-MM-DD"
    int limit = 50;
    int offset = 0;
};

class EventService {
public:
    explicit EventService(Database& db) : db_(db), repo_(db) {}

    // limits, exposed so tests and the API can reference the same numbers
    static constexpr int kMaxTitle = 100;
    static constexpr int kMaxDescription = 500;
    static constexpr int kMaxCategory = 30;
    static constexpr int kMaxLocation = 100;
    static constexpr int kMaxCapacity = 100000;
    static constexpr int kMinEventMinutes = 15;
    static constexpr int kMaxEventMinutes = 12 * 60;

    json createVenue(const NewVenue& v);
    json listVenues();
    json createEvent(const NewEvent& e);
    json cancelEvent(int64_t eventId);
    json getEvent(int64_t eventId);
    json searchEvents(const EventQuery& q);
    json categories();

    json registerStudent(int64_t eventId, const std::string& studentId, const std::string& name);
    json cancelRegistration(int64_t eventId, const std::string& studentId);
    json checkIn(int64_t eventId, const std::string& studentId);
    json attendees(int64_t eventId);
    std::string attendeesCsv(int64_t eventId);
    json studentRegistrations(const std::string& studentId);

    // Free time slots in a venue on one day. dayStart / dayEnd are "HH:MM" ("24:00" allowed).
    json freeSlots(int64_t venueId, const std::string& date, const std::string& dayStart, const std::string& dayEnd,
                   int minMinutes);

    json attendanceReport(bool includeCancelled);
    json popularityReport(int limit);
    json venueUsageReport();
    json waitlistReport();
    json stats();

    // Normalises a student id: trims, upper-cases, validates (3-20 letters, digits, '-' or '_').
    static std::string normaliseStudentId(const std::string& raw);

private:
    Database& db_;
    Repository repo_;
    std::mutex mu_;
};

}  // namespace ce
