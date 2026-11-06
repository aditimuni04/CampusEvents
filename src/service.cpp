#include "campusevents/service.hpp"

#include <algorithm>

namespace ce {

namespace {

// Validates free text: trimmed, no control characters, 1..max bytes (or 0..max when optional).
std::string cleanText(const std::string& raw, const char* field, size_t maxLen, bool required) {
    std::string s = trim(raw);
    if (hasControlChars(s)) throw AppError::invalid(std::string("'") + field + "' must not contain control characters");
    if (required && s.empty()) throw AppError::invalid(std::string("'") + field + "' is required");
    if (s.size() > maxLen) {
        throw AppError::invalid(std::string("'") + field + "' is too long (max " + std::to_string(maxLen) + " characters)");
    }
    return s;
}

int checkCapacity(int cap, const char* field) {
    if (cap < 1 || cap > EventService::kMaxCapacity) {
        throw AppError::invalid(std::string("'") + field + "' must be between 1 and " +
                                std::to_string(EventService::kMaxCapacity));
    }
    return cap;
}

json eventBrief(const Event& e) {
    return {{"id", e.id}, {"title", e.title}, {"start", formatDateTime(e.start)}, {"end", formatDateTime(e.end)}};
}

}  // namespace

std::string EventService::normaliseStudentId(const std::string& raw) {
    std::string id = toUpper(trim(raw));
    if (id.size() < 3 || id.size() > 20) throw AppError::invalid("'student_id' must be 3 to 20 characters");
    for (char c : id) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) throw AppError::invalid("'student_id' may only contain letters, digits, '-' and '_'");
    }
    return id;
}

// ------------------------------------------------------------------ venues

json EventService::createVenue(const NewVenue& in) {
    const std::string name = cleanText(in.name, "name", 60, true);
    const std::string location = cleanText(in.location, "location", kMaxLocation, false);
    checkCapacity(in.capacity, "capacity");
    std::lock_guard<std::mutex> lock(mu_);
    Transaction tx(db_);
    if (db_.scalar("SELECT COUNT(*) FROM venues WHERE name = ?", {name}) > 0) {
        throw AppError::conflict("duplicate_venue", "a venue named '" + name + "' already exists");
    }
    Venue v = repo_.addVenue(name, in.capacity, location);
    tx.commit();
    return v.toJson();
}

json EventService::listVenues() {
    std::lock_guard<std::mutex> lock(mu_);
    json out = json::array();
    for (const Venue& v : repo_.venues()) out.push_back(v.toJson());
    return out;
}

// ------------------------------------------------------------------ events

json EventService::createEvent(const NewEvent& in) {
    Event e;
    e.title = cleanText(in.title, "title", kMaxTitle, true);
    e.category = cleanText(in.category, "category", kMaxCategory, false);
    if (e.category.empty()) e.category = "General";
    e.description = cleanText(in.description, "description", kMaxDescription, false);
    e.venueId = in.venueId;
    e.capacity = checkCapacity(in.capacity, "capacity");
    if (!parseDateTime(trim(in.start), e.start)) throw AppError::invalid("'start' must look like 2026-11-02 10:00");
    if (!parseDateTime(trim(in.end), e.end)) throw AppError::invalid("'end' must look like 2026-11-02 12:00");
    if (e.end <= e.start) throw AppError::invalid("'end' must be after 'start'");
    const int64_t minutes = (e.end - e.start) / 60;
    if (minutes < kMinEventMinutes) {
        throw AppError::invalid("an event must last at least " + std::to_string(kMinEventMinutes) + " minutes");
    }
    if (minutes > kMaxEventMinutes) {
        throw AppError::invalid("an event can last at most " + std::to_string(kMaxEventMinutes / 60) + " hours");
    }

    std::lock_guard<std::mutex> lock(mu_);
    Transaction tx(db_);
    std::optional<Venue> venue = repo_.venue(e.venueId);
    if (!venue) throw AppError::notFound("venue " + std::to_string(e.venueId) + " does not exist");

    // Rule 1: the venue must be big enough.
    if (e.capacity > venue->capacity) {
        throw AppError::invalid("event capacity " + std::to_string(e.capacity) + " exceeds the capacity of '" +
                                venue->name + "' (" + std::to_string(venue->capacity) + ")");
    }
    // Rule 2: the venue must be free for the whole time (interval overlap).
    if (std::optional<Event> clash = repo_.findVenueClash(e.venueId, e.interval())) {
        throw AppError::conflict("venue_clash",
                                 "'" + venue->name + "' is already booked for '" + clash->title + "' (" +
                                     clash->timeText() + ")",
                                 {{"clashes_with", eventBrief(*clash)}});
    }
    Event saved = repo_.addEvent(e);
    tx.commit();
    return repo_.eventSummary(saved.id);
}

json EventService::cancelEvent(int64_t eventId) {
    std::lock_guard<std::mutex> lock(mu_);
    Transaction tx(db_);
    std::optional<Event> e = repo_.event(eventId);
    if (!e) throw AppError::notFound("event " + std::to_string(eventId) + " does not exist");
    if (e->cancelled) throw AppError::conflict("already_cancelled", "this event is already cancelled");
    repo_.setCancelled(eventId);
    tx.commit();
    return repo_.eventSummary(eventId);
}

json EventService::getEvent(int64_t eventId) {
    std::lock_guard<std::mutex> lock(mu_);
    json s = repo_.eventSummary(eventId);
    if (s.is_null()) throw AppError::notFound("event " + std::to_string(eventId) + " does not exist");
    return s;
}

json EventService::searchEvents(const EventQuery& q) {
    EventFilter f;
    f.text = cleanText(q.text, "q", 100, false);
    f.category = cleanText(q.category, "category", kMaxCategory, false);
    f.venueId = q.venueId;
    if (q.status != "active" && q.status != "cancelled" && q.status != "all") {
        throw AppError::invalid("'status' must be active, cancelled or all");
    }
    f.status = q.status;
    if (q.sort != "start" && q.sort != "title" && q.sort != "popularity" && q.sort != "seats") {
        throw AppError::invalid("'sort' must be start, title, popularity or seats");
    }
    f.sort = q.sort;
    if (q.order != "asc" && q.order != "desc") throw AppError::invalid("'order' must be asc or desc");
    f.descending = q.order == "desc";
    if (q.limit < 1 || q.limit > 200) throw AppError::invalid("'limit' must be between 1 and 200");
    if (q.offset < 0) throw AppError::invalid("'offset' must not be negative");
    f.limit = q.limit;
    f.offset = q.offset;
    int64_t t = 0;
    if (!q.from.empty()) {
        if (!parseDate(q.from, t)) throw AppError::invalid("'from' must look like 2026-11-02");
        f.from = t;
    }
    if (!q.to.empty()) {
        if (!parseDate(q.to, t)) throw AppError::invalid("'to' must look like 2026-11-02");
        f.toExclusive = t + 86400;  // 'to' is inclusive of that whole day
    }
    if (f.from && f.toExclusive && *f.from >= *f.toExclusive) throw AppError::invalid("'from' must not be after 'to'");
    std::lock_guard<std::mutex> lock(mu_);
    return repo_.searchEvents(f);
}

json EventService::categories() {
    std::lock_guard<std::mutex> lock(mu_);
    return json(repo_.categories());
}

// ------------------------------------------------------------------ registrations

json EventService::registerStudent(int64_t eventId, const std::string& rawId, const std::string& rawName) {
    const std::string studentId = normaliseStudentId(rawId);
    const std::string name = cleanText(rawName, "name", 60, true);

    std::lock_guard<std::mutex> lock(mu_);
    Transaction tx(db_);
    std::optional<Event> e = repo_.event(eventId);
    if (!e) throw AppError::notFound("event " + std::to_string(eventId) + " does not exist");
    if (e->cancelled) throw AppError::conflict("event_cancelled", "this event has been cancelled");

    if (std::optional<Registration> existing = repo_.registration(eventId, studentId)) {
        throw AppError::conflict("already_registered",
                                 std::string("already registered for this event (status: ") + toString(existing->status) + ")",
                                 {{"status", toString(existing->status)}});
    }
    // A student cannot be in two places at once: refuse if another sign-up overlaps in time.
    if (std::optional<Event> clash = repo_.findStudentClash(studentId, e->interval(), eventId)) {
        throw AppError::conflict("student_clash",
                                 "you are already signed up for '" + clash->title + "' (" + clash->timeText() +
                                     ") which overlaps with this event",
                                 {{"clashes_with", eventBrief(*clash)}});
    }

    // Seats left -> confirmed, otherwise the student joins the back of the FIFO waiting list.
    const bool seatFree = repo_.countWithStatus(eventId, RegStatus::Confirmed) < e->capacity;
    repo_.addStudentIfMissing(studentId, name);
    Registration reg = repo_.addRegistration(eventId, studentId, seatFree ? RegStatus::Confirmed : RegStatus::Waiting);
    json out = {{"status", toString(reg.status)}, {"student_id", studentId}, {"name", reg.name}};
    out["waiting_position"] = seatFree ? json(nullptr) : json(repo_.waitingPosition(eventId, reg.id));
    out["message"] = seatFree ? "You are registered." : "The event is full - you are on the waiting list.";
    tx.commit();
    out["event"] = repo_.eventSummary(eventId);
    return out;
}

json EventService::cancelRegistration(int64_t eventId, const std::string& rawId) {
    const std::string studentId = normaliseStudentId(rawId);
    std::lock_guard<std::mutex> lock(mu_);
    Transaction tx(db_);
    std::optional<Event> e = repo_.event(eventId);
    if (!e) throw AppError::notFound("event " + std::to_string(eventId) + " does not exist");
    std::optional<Registration> reg = repo_.registration(eventId, studentId);
    if (!reg) throw AppError::notFound("no registration for " + studentId + " on this event");

    const RegStatus was = reg->status;
    repo_.removeRegistration(reg->id);

    // A confirmed seat just became free: hand it to the longest-waiting student (first in, first out).
    json promoted = nullptr;
    if (was == RegStatus::Confirmed && !e->cancelled) {
        if (std::optional<Registration> next = repo_.firstWaiting(eventId)) {
            repo_.setRegistrationStatus(next->id, RegStatus::Confirmed);
            promoted = {{"student_id", next->studentId}, {"name", next->name}};
        }
    }
    tx.commit();
    return {{"cancelled", true}, {"was", toString(was)}, {"promoted", promoted}, {"event", repo_.eventSummary(eventId)}};
}

json EventService::checkIn(int64_t eventId, const std::string& rawId) {
    const std::string studentId = normaliseStudentId(rawId);
    std::lock_guard<std::mutex> lock(mu_);
    Transaction tx(db_);
    std::optional<Event> e = repo_.event(eventId);
    if (!e) throw AppError::notFound("event " + std::to_string(eventId) + " does not exist");
    if (e->cancelled) throw AppError::conflict("event_cancelled", "this event has been cancelled");
    std::optional<Registration> reg = repo_.registration(eventId, studentId);
    if (!reg) throw AppError::notFound("no registration for " + studentId + " on this event");
    if (reg->status != RegStatus::Confirmed) {
        throw AppError::conflict("not_confirmed", "this student is still on the waiting list");
    }
    const bool already = reg->checkedIn;
    if (!already) repo_.markCheckedIn(reg->id);
    tx.commit();
    return {{"student_id", studentId}, {"name", reg->name}, {"checked_in", true}, {"already_checked_in", already}};
}

json EventService::attendees(int64_t eventId) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!repo_.event(eventId)) throw AppError::notFound("event " + std::to_string(eventId) + " does not exist");
    return repo_.attendees(eventId);
}

std::string EventService::attendeesCsv(int64_t eventId) {
    const json rows = attendees(eventId);
    std::string out = "student_id,name,status,waiting_position,checked_in\r\n";
    for (const json& r : rows) {
        out += csvField(r.at("student_id").get<std::string>()) + ',' + csvField(r.at("name").get<std::string>()) + ',' +
               r.at("status").get<std::string>() + ',';
        if (!r.at("waiting_position").is_null()) out += std::to_string(r.at("waiting_position").get<int64_t>());
        out += ',';
        out += r.at("checked_in").get<int64_t>() ? "yes" : "no";
        out += "\r\n";
    }
    return out;
}

json EventService::studentRegistrations(const std::string& rawId) {
    const std::string studentId = normaliseStudentId(rawId);
    std::lock_guard<std::mutex> lock(mu_);
    return repo_.studentRegistrations(studentId);
}

// ------------------------------------------------------------------ free slots

json EventService::freeSlots(int64_t venueId, const std::string& date, const std::string& dayStart,
                             const std::string& dayEnd, int minMinutes) {
    int64_t day = 0;
    int fromMin = 0, toMin = 0;
    if (!parseDate(date, day)) throw AppError::invalid("'date' must look like 2026-11-02");
    if (!parseTimeOfDay(dayStart, fromMin)) throw AppError::invalid("'day_start' must look like 08:00");
    if (!parseTimeOfDay(dayEnd, toMin)) throw AppError::invalid("'day_end' must look like 20:00");
    if (fromMin >= toMin) throw AppError::invalid("'day_start' must be before 'day_end'");
    if (minMinutes < 1 || minMinutes > 24 * 60) throw AppError::invalid("'min_minutes' must be between 1 and 1440");

    std::lock_guard<std::mutex> lock(mu_);
    std::optional<Venue> venue = repo_.venue(venueId);
    if (!venue) throw AppError::notFound("venue " + std::to_string(venueId) + " does not exist");

    const Interval window{day + fromMin * 60LL, day + toMin * 60LL};
    std::vector<Event> events = repo_.venueEventsIn(venueId, window);
    std::vector<Interval> busy;
    json busyJson = json::array();
    for (const Event& e : events) {
        busy.push_back(e.interval());
        busyJson.push_back(eventBrief(e));
    }
    json slots = json::array();
    for (const Interval& g : freeGaps(busy, window, minMinutes * 60LL)) {
        slots.push_back({{"start", formatDateTime(g.start)},
                         {"end", formatDateTime(g.end)},
                         {"minutes", (g.end - g.start) / 60}});
    }
    return {{"venue", venue->toJson()}, {"date", date}, {"day_start", dayStart}, {"day_end", dayEnd},
            {"min_minutes", minMinutes}, {"free_slots", slots}, {"booked", busyJson}};
}

// ------------------------------------------------------------------ reports

json EventService::attendanceReport(bool includeCancelled) {
    std::lock_guard<std::mutex> lock(mu_);
    return repo_.attendanceReport(includeCancelled);
}
json EventService::popularityReport(int limit) {
    if (limit < 1 || limit > 100) throw AppError::invalid("'limit' must be between 1 and 100");
    std::lock_guard<std::mutex> lock(mu_);
    return repo_.popularityReport(limit);
}
json EventService::venueUsageReport() {
    std::lock_guard<std::mutex> lock(mu_);
    return repo_.venueUsageReport();
}
json EventService::waitlistReport() {
    std::lock_guard<std::mutex> lock(mu_);
    return repo_.waitlistReport();
}
json EventService::stats() {
    std::lock_guard<std::mutex> lock(mu_);
    return repo_.stats();
}

}  // namespace ce
