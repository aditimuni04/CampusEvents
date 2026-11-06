#pragma once
// Repository: every SQL statement of the application lives here. It knows nothing about rules or HTTP.
#include <optional>
#include <string>
#include <vector>

#include "campusevents/db.hpp"
#include "campusevents/types.hpp"

namespace ce {

struct EventFilter {
    std::string text;      // matches title, description, category or venue name
    int64_t venueId = 0;   // 0 = any
    std::string category;  // "" = any
    std::optional<int64_t> from;       // event must START at or after this time
    std::optional<int64_t> toExclusive; // ... and strictly before this time
    std::string status = "active";     // active | cancelled | all
    std::string sort = "start";        // start | title | popularity | seats
    bool descending = false;
    int limit = 50;
    int offset = 0;
};

class Repository {
public:
    explicit Repository(Database& db) : db_(db) {}

    // venues
    Venue addVenue(const std::string& name, int capacity, const std::string& location);
    std::optional<Venue> venue(int64_t id);
    std::vector<Venue> venues();

    // events
    Event addEvent(const Event& e);
    std::optional<Event> event(int64_t id);
    void setCancelled(int64_t id);
    // First active event in the venue whose time overlaps `iv` (optionally ignoring one event id).
    std::optional<Event> findVenueClash(int64_t venueId, Interval iv, int64_t ignoreEventId = 0);
    // Active events of a venue that touch the window, ordered by start (for the free-slot finder).
    std::vector<Event> venueEventsIn(int64_t venueId, Interval window);
    json eventSummary(int64_t id);        // one event with venue name and counts (null if missing)
    json searchEvents(const EventFilter& f);  // {"items": [...], "total": N}
    std::vector<std::string> categories();

    // students & registrations
    void addStudentIfMissing(const std::string& id, const std::string& name);
    std::optional<Registration> registration(int64_t eventId, const std::string& studentId);
    Registration addRegistration(int64_t eventId, const std::string& studentId, RegStatus status);
    void removeRegistration(int64_t regId);
    void setRegistrationStatus(int64_t regId, RegStatus status);
    void markCheckedIn(int64_t regId);
    int64_t countWithStatus(int64_t eventId, RegStatus status);
    std::optional<Registration> firstWaiting(int64_t eventId);           // oldest waiting sign-up
    int64_t waitingPosition(int64_t eventId, int64_t regId);             // 1 = next in line
    // An active event the student is already signed up for (confirmed or waiting) that overlaps `iv`.
    std::optional<Event> findStudentClash(const std::string& studentId, Interval iv, int64_t ignoreEventId);
    json attendees(int64_t eventId);
    json studentRegistrations(const std::string& studentId);

    // reports (JOIN + GROUP BY)
    json attendanceReport(bool includeCancelled);
    json popularityReport(int limit);
    json venueUsageReport();
    json waitlistReport();
    json stats();

private:
    Database& db_;
};

}  // namespace ce
