#include "campusevents/repository.hpp"

#include <algorithm>
#include <cmath>

namespace ce {

namespace {

int64_t i64(const json& j, const char* k) { return j.at(k).get<int64_t>(); }
std::string str(const json& j, const char* k) { return j.at(k).get<std::string>(); }

Venue toVenue(const json& r) {
    Venue v;
    v.id = i64(r, "id");
    v.name = str(r, "name");
    v.capacity = static_cast<int>(i64(r, "capacity"));
    v.location = str(r, "location");
    return v;
}

Event toEvent(const json& r) {
    Event e;
    e.id = i64(r, "id");
    e.title = str(r, "title");
    e.category = str(r, "category");
    e.description = str(r, "description");
    e.venueId = i64(r, "venue_id");
    e.start = i64(r, "start_ts");
    e.end = i64(r, "end_ts");
    e.capacity = static_cast<int>(i64(r, "capacity"));
    e.cancelled = str(r, "status") == "cancelled";
    return e;
}

Registration toRegistration(const json& r) {
    Registration g;
    g.id = i64(r, "id");
    g.eventId = i64(r, "event_id");
    g.studentId = str(r, "student_id");
    g.name = str(r, "name");
    g.status = str(r, "status") == "confirmed" ? RegStatus::Confirmed : RegStatus::Waiting;
    g.checkedIn = i64(r, "checked_in") != 0;
    g.createdAt = i64(r, "created_at");
    return g;
}

const char* kEventColumns = "e.id, e.title, e.category, e.description, e.venue_id, e.start_ts, e.end_ts, e.capacity, e.status";

// Escapes %, _ and \ so user text is matched literally by LIKE ... ESCAPE '\'.
std::string likePattern(const std::string& text) {
    std::string out = "%";
    for (char c : text) {
        if (c == '%' || c == '_' || c == '\\') out += '\\';
        out += c;
    }
    out += '%';
    return out;
}

double round1(double v) { return std::round(v * 10.0) / 10.0; }

// Adds the human-readable fields every event row shown to clients carries.
void decorateEvent(json& r) {
    const int64_t cap = r.at("capacity").get<int64_t>();
    const int64_t confirmed = r.at("confirmed").get<int64_t>();
    r["start"] = formatDateTime(r.at("start_ts").get<int64_t>());
    r["end"] = formatDateTime(r.at("end_ts").get<int64_t>());
    r["seats_left"] = std::max<int64_t>(cap - confirmed, 0);
    r["full"] = confirmed >= cap;
    r["cancelled"] = r.at("status").get<std::string>() == "cancelled";
}

// Event + venue name + live counts, via JOIN and GROUP BY. Callers append WHERE / ORDER / LIMIT.
const char* kSummarySelect =
    "SELECT e.id, e.title, e.category, e.description, e.venue_id, v.name AS venue_name, v.capacity AS venue_capacity, "
    "       e.start_ts, e.end_ts, e.capacity, e.status, "
    "       COALESCE(SUM(r.status = 'confirmed'), 0) AS confirmed, "
    "       COALESCE(SUM(r.status = 'waiting'), 0)   AS waiting, "
    "       COALESCE(SUM(r.checked_in), 0)           AS checked_in "
    "FROM events e "
    "JOIN venues v ON v.id = e.venue_id "
    "LEFT JOIN registrations r ON r.event_id = e.id ";

}  // namespace

// ------------------------------------------------------------------ venues

Venue Repository::addVenue(const std::string& name, int capacity, const std::string& location) {
    db_.run("INSERT INTO venues(name, capacity, location) VALUES(?, ?, ?)",
            {name, static_cast<int64_t>(capacity), location});
    return *venue(db_.lastInsertId());
}

std::optional<Venue> Repository::venue(int64_t id) {
    json rows = db_.query("SELECT id, name, capacity, location FROM venues WHERE id = ?", {id});
    if (rows.empty()) return std::nullopt;
    return toVenue(rows[0]);
}

std::vector<Venue> Repository::venues() {
    std::vector<Venue> out;
    for (const json& r : db_.query("SELECT id, name, capacity, location FROM venues ORDER BY name COLLATE NOCASE")) {
        out.push_back(toVenue(r));
    }
    return out;
}

// ------------------------------------------------------------------ events

Event Repository::addEvent(const Event& e) {
    db_.run("INSERT INTO events(title, category, description, venue_id, start_ts, end_ts, capacity, status) "
            "VALUES(?, ?, ?, ?, ?, ?, ?, 'active')",
            {e.title, e.category, e.description, e.venueId, e.start, e.end, static_cast<int64_t>(e.capacity)});
    return *event(db_.lastInsertId());
}

std::optional<Event> Repository::event(int64_t id) {
    json rows = db_.query(std::string("SELECT ") + kEventColumns + " FROM events e WHERE e.id = ?", {id});
    if (rows.empty()) return std::nullopt;
    return toEvent(rows[0]);
}

void Repository::setCancelled(int64_t id) { db_.run("UPDATE events SET status = 'cancelled' WHERE id = ?", {id}); }

std::optional<Event> Repository::findVenueClash(int64_t venueId, Interval iv, int64_t ignoreEventId) {
    // Overlap test in SQL: existing.start < new.end AND new.start < existing.end
    json rows = db_.query(std::string("SELECT ") + kEventColumns +
                              " FROM events e WHERE e.venue_id = ? AND e.status = 'active' AND e.id <> ? "
                              "AND e.start_ts < ? AND ? < e.end_ts ORDER BY e.start_ts LIMIT 1",
                          {venueId, ignoreEventId, iv.end, iv.start});
    if (rows.empty()) return std::nullopt;
    return toEvent(rows[0]);
}

std::vector<Event> Repository::venueEventsIn(int64_t venueId, Interval window) {
    std::vector<Event> out;
    for (const json& r : db_.query(std::string("SELECT ") + kEventColumns +
                                       " FROM events e WHERE e.venue_id = ? AND e.status = 'active' "
                                       "AND e.start_ts < ? AND ? < e.end_ts ORDER BY e.start_ts",
                                   {venueId, window.end, window.start})) {
        out.push_back(toEvent(r));
    }
    return out;
}

json Repository::eventSummary(int64_t id) {
    json rows = db_.query(std::string(kSummarySelect) + "WHERE e.id = ? GROUP BY e.id", {id});
    if (rows.empty()) return nullptr;
    decorateEvent(rows[0]);
    return rows[0];
}

json Repository::searchEvents(const EventFilter& f) {
    std::string where = " WHERE 1=1";
    std::vector<Param> params;
    if (f.status == "active" || f.status == "cancelled") {
        where += " AND e.status = ?";
        params.emplace_back(f.status);
    }
    if (!f.text.empty()) {
        const std::string like = likePattern(f.text);
        where += " AND (e.title LIKE ? ESCAPE '\\' OR e.description LIKE ? ESCAPE '\\' "
                 "OR e.category LIKE ? ESCAPE '\\' OR v.name LIKE ? ESCAPE '\\')";
        for (int i = 0; i < 4; ++i) params.emplace_back(like);
    }
    if (f.venueId > 0) {
        where += " AND e.venue_id = ?";
        params.emplace_back(f.venueId);
    }
    if (!f.category.empty()) {
        where += " AND e.category = ? COLLATE NOCASE";
        params.emplace_back(f.category);
    }
    if (f.from) {
        where += " AND e.start_ts >= ?";
        params.emplace_back(*f.from);
    }
    if (f.toExclusive) {
        where += " AND e.start_ts < ?";
        params.emplace_back(*f.toExclusive);
    }

    // The ORDER BY column comes from a fixed whitelist - never from user text.
    std::string orderCol = "e.start_ts";
    if (f.sort == "title") orderCol = "e.title COLLATE NOCASE";
    else if (f.sort == "popularity") orderCol = "(confirmed + waiting)";
    else if (f.sort == "seats") orderCol = "(e.capacity - confirmed)";

    json out;
    out["total"] = db_.scalar("SELECT COUNT(*) FROM events e JOIN venues v ON v.id = e.venue_id" + where, params);
    std::vector<Param> p2 = params;
    p2.emplace_back(static_cast<int64_t>(f.limit));
    p2.emplace_back(static_cast<int64_t>(f.offset));
    json rows = db_.query(std::string(kSummarySelect) + where + " GROUP BY e.id ORDER BY " + orderCol +
                              (f.descending ? " DESC" : " ASC") + ", e.id ASC LIMIT ? OFFSET ?",
                          p2);
    for (json& r : rows) decorateEvent(r);
    out["items"] = std::move(rows);
    return out;
}

std::vector<std::string> Repository::categories() {
    std::vector<std::string> out;
    for (const json& r : db_.query("SELECT DISTINCT category FROM events ORDER BY category COLLATE NOCASE")) {
        out.push_back(str(r, "category"));
    }
    return out;
}

// ------------------------------------------------------------------ registrations

void Repository::addStudentIfMissing(const std::string& id, const std::string& name) {
    db_.run("INSERT OR IGNORE INTO students(id, name) VALUES(?, ?)", {id, name});
}

std::optional<Registration> Repository::registration(int64_t eventId, const std::string& studentId) {
    json rows = db_.query("SELECT r.id, r.event_id, r.student_id, s.name, r.status, r.checked_in, r.created_at "
                          "FROM registrations r JOIN students s ON s.id = r.student_id "
                          "WHERE r.event_id = ? AND r.student_id = ?",
                          {eventId, studentId});
    if (rows.empty()) return std::nullopt;
    return toRegistration(rows[0]);
}

Registration Repository::addRegistration(int64_t eventId, const std::string& studentId, RegStatus status) {
    db_.run("INSERT INTO registrations(event_id, student_id, status) VALUES(?, ?, ?)",
            {eventId, studentId, std::string(toString(status))});
    return *registration(eventId, studentId);
}

void Repository::removeRegistration(int64_t regId) { db_.run("DELETE FROM registrations WHERE id = ?", {regId}); }

void Repository::setRegistrationStatus(int64_t regId, RegStatus status) {
    db_.run("UPDATE registrations SET status = ? WHERE id = ?", {std::string(toString(status)), regId});
}

void Repository::markCheckedIn(int64_t regId) { db_.run("UPDATE registrations SET checked_in = 1 WHERE id = ?", {regId}); }

int64_t Repository::countWithStatus(int64_t eventId, RegStatus status) {
    return db_.scalar("SELECT COUNT(*) FROM registrations WHERE event_id = ? AND status = ?",
                      {eventId, std::string(toString(status))});
}

std::optional<Registration> Repository::firstWaiting(int64_t eventId) {
    json rows = db_.query("SELECT r.id, r.event_id, r.student_id, s.name, r.status, r.checked_in, r.created_at "
                          "FROM registrations r JOIN students s ON s.id = r.student_id "
                          "WHERE r.event_id = ? AND r.status = 'waiting' ORDER BY r.id ASC LIMIT 1",
                          {eventId});
    if (rows.empty()) return std::nullopt;
    return toRegistration(rows[0]);
}

int64_t Repository::waitingPosition(int64_t eventId, int64_t regId) {
    return db_.scalar("SELECT COUNT(*) FROM registrations WHERE event_id = ? AND status = 'waiting' AND id <= ?",
                      {eventId, regId});
}

std::optional<Event> Repository::findStudentClash(const std::string& studentId, Interval iv, int64_t ignoreEventId) {
    json rows = db_.query(std::string("SELECT ") + kEventColumns +
                              " FROM registrations r JOIN events e ON e.id = r.event_id "
                              "WHERE r.student_id = ? AND e.status = 'active' AND e.id <> ? "
                              "AND e.start_ts < ? AND ? < e.end_ts ORDER BY e.start_ts LIMIT 1",
                          {studentId, ignoreEventId, iv.end, iv.start});
    if (rows.empty()) return std::nullopt;
    return toEvent(rows[0]);
}

json Repository::attendees(int64_t eventId) {
    json rows = db_.query(
        "SELECT r.student_id, s.name, r.status, r.checked_in, r.created_at, "
        "       CASE WHEN r.status = 'waiting' THEN "
        "            (SELECT COUNT(*) FROM registrations w WHERE w.event_id = r.event_id AND w.status = 'waiting' AND w.id <= r.id) "
        "            ELSE NULL END AS waiting_position "
        "FROM registrations r JOIN students s ON s.id = r.student_id "
        "WHERE r.event_id = ? ORDER BY (r.status = 'waiting'), r.id",
        {eventId});
    return rows;
}

json Repository::studentRegistrations(const std::string& studentId) {
    json rows = db_.query(
        "SELECT e.id AS event_id, e.title, e.category, v.name AS venue_name, e.start_ts, e.end_ts, e.status AS event_status, "
        "       r.status, r.checked_in, "
        "       CASE WHEN r.status = 'waiting' THEN "
        "            (SELECT COUNT(*) FROM registrations w WHERE w.event_id = r.event_id AND w.status = 'waiting' AND w.id <= r.id) "
        "            ELSE NULL END AS waiting_position "
        "FROM registrations r JOIN events e ON e.id = r.event_id JOIN venues v ON v.id = e.venue_id "
        "WHERE r.student_id = ? ORDER BY e.start_ts, e.id",
        {studentId});
    for (json& r : rows) {
        r["start"] = formatDateTime(r.at("start_ts").get<int64_t>());
        r["end"] = formatDateTime(r.at("end_ts").get<int64_t>());
    }
    return rows;
}

// ------------------------------------------------------------------ reports

json Repository::attendanceReport(bool includeCancelled) {
    json rows = db_.query(
        std::string("SELECT e.id, e.title, e.category, v.name AS venue_name, e.start_ts, e.capacity, e.status, "
                    "       COALESCE(SUM(r.status = 'confirmed'), 0) AS confirmed, "
                    "       COALESCE(SUM(r.status = 'waiting'), 0)   AS waiting, "
                    "       COALESCE(SUM(r.checked_in), 0)           AS checked_in "
                    "FROM events e JOIN venues v ON v.id = e.venue_id "
                    "LEFT JOIN registrations r ON r.event_id = e.id ") +
        (includeCancelled ? "" : "WHERE e.status = 'active' ") + "GROUP BY e.id ORDER BY e.start_ts, e.id");
    for (json& r : rows) {
        const double cap = static_cast<double>(r.at("capacity").get<int64_t>());
        const double confirmed = static_cast<double>(r.at("confirmed").get<int64_t>());
        const double in = static_cast<double>(r.at("checked_in").get<int64_t>());
        r["start"] = formatDateTime(r.at("start_ts").get<int64_t>());
        r["fill_pct"] = round1(100.0 * confirmed / cap);
        r["show_up_pct"] = confirmed > 0 ? round1(100.0 * in / confirmed) : 0.0;
    }
    return rows;
}

json Repository::popularityReport(int limit) {
    json rows = db_.query(
        "SELECT e.id, e.title, e.category, v.name AS venue_name, e.start_ts, e.capacity, "
        "       COALESCE(SUM(r.status = 'confirmed'), 0) AS confirmed, "
        "       COALESCE(SUM(r.status = 'waiting'), 0)   AS waiting, "
        "       COUNT(r.id)                              AS demand "
        "FROM events e JOIN venues v ON v.id = e.venue_id "
        "LEFT JOIN registrations r ON r.event_id = e.id "
        "WHERE e.status = 'active' GROUP BY e.id "
        "ORDER BY demand DESC, confirmed DESC, e.start_ts, e.id LIMIT ?",
        {static_cast<int64_t>(limit)});
    for (json& r : rows) r["start"] = formatDateTime(r.at("start_ts").get<int64_t>());
    return rows;
}

json Repository::venueUsageReport() {
    // Two levels: first one row per event (with its confirmed count), then group those rows per venue.
    // Joining registrations straight onto venues would multiply the event hours by the attendee count.
    json rows = db_.query(
        "WITH per_event AS ("
        "  SELECT e.id, e.venue_id, e.capacity, e.end_ts - e.start_ts AS seconds, "
        "         COALESCE(SUM(r.status = 'confirmed'), 0) AS confirmed "
        "  FROM events e LEFT JOIN registrations r ON r.event_id = e.id "
        "  WHERE e.status = 'active' GROUP BY e.id) "
        "SELECT v.id, v.name, v.capacity, v.location, "
        "       COUNT(p.id) AS events, "
        "       COALESCE(SUM(p.seconds), 0) / 3600.0 AS booked_hours, "
        "       COALESCE(SUM(p.confirmed), 0) AS confirmed, "
        "       COALESCE(100.0 * SUM(p.confirmed) / NULLIF(SUM(p.capacity), 0), 0) AS avg_fill_pct "
        "FROM venues v LEFT JOIN per_event p ON p.venue_id = v.id "
        "GROUP BY v.id ORDER BY booked_hours DESC, v.name COLLATE NOCASE");
    for (json& r : rows) {
        r["booked_hours"] = round1(r.at("booked_hours").get<double>());
        r["avg_fill_pct"] = round1(r.at("avg_fill_pct").get<double>());
    }
    return rows;
}

json Repository::waitlistReport() {
    json rows = db_.query(
        "SELECT e.id, e.title, v.name AS venue_name, e.start_ts, e.capacity, COUNT(r.id) AS waiting, "
        "       MIN(r.created_at) AS oldest_request "
        "FROM events e JOIN venues v ON v.id = e.venue_id "
        "JOIN registrations r ON r.event_id = e.id AND r.status = 'waiting' "
        "WHERE e.status = 'active' GROUP BY e.id ORDER BY waiting DESC, e.start_ts, e.id");
    for (json& r : rows) r["start"] = formatDateTime(r.at("start_ts").get<int64_t>());
    return rows;
}

json Repository::stats() {
    json s;
    s["venues"] = db_.scalar("SELECT COUNT(*) FROM venues");
    s["events"] = db_.scalar("SELECT COUNT(*) FROM events WHERE status = 'active'");
    s["cancelled_events"] = db_.scalar("SELECT COUNT(*) FROM events WHERE status = 'cancelled'");
    s["students"] = db_.scalar("SELECT COUNT(*) FROM students");
    s["confirmed"] = db_.scalar("SELECT COUNT(*) FROM registrations r JOIN events e ON e.id = r.event_id "
                                "WHERE r.status = 'confirmed' AND e.status = 'active'");
    s["waiting"] = db_.scalar("SELECT COUNT(*) FROM registrations r JOIN events e ON e.id = r.event_id "
                              "WHERE r.status = 'waiting' AND e.status = 'active'");
    return s;
}

}  // namespace ce
