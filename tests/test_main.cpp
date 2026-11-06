// Self-contained test runner (no framework dependency).
#include <httplib/httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "campusevents/api.hpp"
#include "campusevents/seed.hpp"
#include "campusevents/service.hpp"
#include "campusevents/util.hpp"

using namespace ce;

// ------------------------------------------------------------------ mini framework
struct TestCase {
    const char* name;
    std::function<void()> fn;
};
static std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}
static int g_failures = 0;
static int g_checks = 0;
struct Registrar {
    Registrar(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); }
};
#define TEST(name)                                   \
    static void test_##name();                       \
    static Registrar reg_##name(#name, test_##name); \
    static void test_##name()
#define CHECK(cond)                                                             \
    do {                                                                        \
        ++g_checks;                                                             \
        if (!(cond)) {                                                          \
            ++g_failures;                                                       \
            std::printf("    FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);     \
        }                                                                       \
    } while (0)
#define CHECK_EQ(a, b)                                                                                \
    do {                                                                                              \
        ++g_checks;                                                                                   \
        auto va = (a);                                                                                \
        auto vb = (b);                                                                                \
        if (!(va == vb)) {                                                                            \
            ++g_failures;                                                                             \
            std::ostringstream o;                                                                     \
            o << va << " != " << vb;                                                                  \
            std::printf("    FAIL %s:%d  %s == %s  (%s)\n", __FILE__, __LINE__, #a, #b, o.str().c_str()); \
        }                                                                                             \
    } while (0)
// Expects the expression to throw AppError with this HTTP status and error code.
#define CHECK_APP_ERROR(expr, st, cd)                                                                     \
    do {                                                                                                  \
        ++g_checks;                                                                                       \
        try {                                                                                             \
            (void)(expr);                                                                                 \
            ++g_failures;                                                                                 \
            std::printf("    FAIL %s:%d  expected error %d/%s but nothing was thrown\n", __FILE__, __LINE__, st, cd); \
        } catch (const AppError& e_) {                                                                    \
            if (e_.status() != (st) || e_.code() != (cd)) {                                               \
                ++g_failures;                                                                             \
                std::printf("    FAIL %s:%d  expected %d/%s, got %d/%s (%s)\n", __FILE__, __LINE__, st, cd, \
                            e_.status(), e_.code().c_str(), e_.what());                                   \
            }                                                                                             \
        }                                                                                                 \
    } while (0)

// ------------------------------------------------------------------ fixtures
struct Fx {
    Database db{":memory:"};
    EventService svc{db};
    Fx() { db.initSchema(); }

    int64_t venue(const std::string& name = "Hall", int cap = 100) {
        return svc.createVenue({name, cap, ""}).at("id").get<int64_t>();
    }
    int64_t event(const std::string& title, int64_t venueId, const std::string& start, const std::string& end,
                  int cap = 50, const std::string& cat = "Tech") {
        return svc.createEvent({title, cat, "", venueId, start, end, cap}).at("id").get<int64_t>();
    }
    // Registers "S<n>" style students quickly.
    json reg(int64_t eventId, const std::string& sid) { return svc.registerStudent(eventId, sid, "Student " + sid); }
    json eventInfo(int64_t id) { return svc.getEvent(id); }
};

static int64_t Tm(const char* s) {
    int64_t v = 0;
    parseDateTime(s, v);
    return v;
}

// ================================================================== time intervals
TEST(overlap_rules) {
    const Interval a{100, 200};
    CHECK(overlaps(a, {150, 250}));   // partial overlap, right side
    CHECK(overlaps(a, {50, 150}));    // partial overlap, left side
    CHECK(overlaps(a, {120, 180}));   // b inside a
    CHECK(overlaps(a, {50, 300}));    // a inside b
    CHECK(overlaps(a, {100, 200}));   // identical
    CHECK(overlaps(a, {199, 300}));   // one unit of overlap
    CHECK(!overlaps(a, {200, 300}));  // back to back: end == start is NOT a clash
    CHECK(!overlaps(a, {0, 100}));
    CHECK(!overlaps(a, {300, 400}));
    CHECK(overlaps({150, 250}, a));   // symmetric
}

TEST(merge_intervals) {
    auto m = mergeIntervals({{50, 60}, {10, 20}, {15, 30}, {30, 40}, {5, 5}, {70, 65}});
    CHECK_EQ(m.size(), size_t(2));       // [10,40] (overlap + touching) and [50,60]; empty/inverted dropped
    CHECK_EQ(m[0].start, 10);
    CHECK_EQ(m[0].end, 40);
    CHECK_EQ(m[1].start, 50);
    CHECK_EQ(m[1].end, 60);
    CHECK(mergeIntervals({}).empty());
    auto nested = mergeIntervals({{0, 100}, {10, 20}, {30, 40}});
    CHECK_EQ(nested.size(), size_t(1));
    CHECK_EQ(nested[0].end, 100);
}

TEST(free_gaps) {
    const Interval w{0, 100};
    CHECK_EQ(freeGaps({}, w, 1).size(), size_t(1));          // nothing booked: whole window is free
    CHECK(freeGaps({{0, 100}}, w, 1).empty());                // fully booked
    CHECK(freeGaps({{-50, 500}}, w, 1).empty());              // booking larger than the window
    auto g = freeGaps({{10, 20}, {20, 30}, {60, 70}}, w, 1);  // touching bookings leave no zero-length gap
    CHECK_EQ(g.size(), size_t(3));
    CHECK_EQ(g[0].end, 10);
    CHECK_EQ(g[1].start, 30);
    CHECK_EQ(g[1].end, 60);
    CHECK_EQ(g[2].start, 70);
    CHECK_EQ(freeGaps({{10, 20}, {20, 30}, {60, 70}}, w, 31).size(), size_t(0));  // every gap is shorter than 31
    CHECK_EQ(freeGaps({{10, 20}, {20, 30}, {60, 70}}, w, 30).size(), size_t(2));  // gaps are 10, 30, 30 minutes: the 10 is dropped, exactly-30 is kept
    auto clipped = freeGaps({{-10, 5}, {95, 120}}, w, 1);     // bookings poking out of the window are clipped
    CHECK_EQ(clipped.size(), size_t(1));
    CHECK_EQ(clipped[0].start, 5);
    CHECK_EQ(clipped[0].end, 95);
}

TEST(date_parsing) {
    int64_t t = 0;
    CHECK(parseDateTime("2026-11-02 10:30", t));
    CHECK_EQ(formatDateTime(t), std::string("2026-11-02 10:30"));
    CHECK(parseDateTime("2026-11-02T10:30", t));  // browser datetime-local format
    CHECK(parseDateTime("2028-02-29 00:00", t));  // leap day
    CHECK(!parseDateTime("2027-02-29 00:00", t)); // not a leap year
    CHECK(!parseDateTime("2026-13-01 10:00", t));
    CHECK(!parseDateTime("2026-04-31 10:00", t));
    CHECK(!parseDateTime("2026-11-02 24:00", t)); // 24:00 is not a valid event time
    CHECK(!parseDateTime("2026-11-02 10:60", t));
    CHECK(!parseDateTime("2026-11-02 10:30:00", t));  // seconds not accepted
    CHECK(!parseDateTime("2026-1-2 10:30", t));
    CHECK(!parseDateTime("garbage", t));
    CHECK(!parseDateTime("", t));
    CHECK(!parseDateTime("1999-12-31 10:00", t));     // outside 2000..2100
    CHECK(!parseDate("2026-11-0x", t));
    int minutes = 0;
    CHECK(parseTimeOfDay("24:00", minutes) && minutes == 1440);
    CHECK(parseTimeOfDay("00:00", minutes) && minutes == 0);
    CHECK(!parseTimeOfDay("24:01", minutes));
    CHECK(!parseTimeOfDay("9:00", minutes));
    // day boundaries and consecutive days
    int64_t d1 = 0, d2 = 0;
    CHECK(parseDate("2026-12-31", d1) && parseDate("2027-01-01", d2));
    CHECK_EQ(d2 - d1, int64_t(86400));
    CHECK_EQ(formatDate(d2), std::string("2027-01-01"));
}

TEST(text_helpers) {
    CHECK_EQ(trim("  hi \n"), std::string("hi"));
    CHECK_EQ(trim("   "), std::string(""));
    CHECK(hasControlChars("a\nb"));
    CHECK(hasControlChars(std::string("a\0b", 3)));
    CHECK(!hasControlChars("plain text ünïcode"));
    CHECK_EQ(csvField("plain"), std::string("plain"));
    CHECK_EQ(csvField("a,b"), std::string("\"a,b\""));
    CHECK_EQ(csvField("say \"hi\""), std::string("\"say \"\"hi\"\"\""));
    CHECK_EQ(csvField("=HYPERLINK(\"x\")").substr(0, 2), std::string("\"'"));  // formula neutralised then quoted
    CHECK_EQ(csvField("+1"), std::string("'+1"));
    CHECK_EQ(csvField("@SUM(A1)"), std::string("'@SUM(A1)"));
    CHECK_EQ(csvField("-2"), std::string("'-2"));
    CHECK_EQ(EventService::normaliseStudentId("  pce26-0001 "), std::string("PCE26-0001"));
    CHECK_APP_ERROR(EventService::normaliseStudentId("ab"), 400, "invalid_input");
    CHECK_APP_ERROR(EventService::normaliseStudentId("has space"), 400, "invalid_input");
    CHECK_APP_ERROR(EventService::normaliseStudentId("a;drop"), 400, "invalid_input");
    CHECK_APP_ERROR(EventService::normaliseStudentId(std::string(21, 'A')), 400, "invalid_input");
}

// ================================================================== venues and events
TEST(venue_creation_and_validation) {
    Fx fx;
    json v = fx.svc.createVenue({"  Main Hall ", 200, "Block A"});
    CHECK_EQ(v.at("name").get<std::string>(), std::string("Main Hall"));  // trimmed
    CHECK_APP_ERROR(fx.svc.createVenue({"main hall", 50, ""}), 409, "duplicate_venue");  // case-insensitive
    CHECK_APP_ERROR(fx.svc.createVenue({"", 50, ""}), 400, "invalid_input");
    CHECK_APP_ERROR(fx.svc.createVenue({"   ", 50, ""}), 400, "invalid_input");
    CHECK_APP_ERROR(fx.svc.createVenue({"X", 0, ""}), 400, "invalid_input");
    CHECK_APP_ERROR(fx.svc.createVenue({"X", -5, ""}), 400, "invalid_input");
    CHECK_APP_ERROR(fx.svc.createVenue({"X", 100001, ""}), 400, "invalid_input");
    CHECK_APP_ERROR(fx.svc.createVenue({std::string(61, 'x'), 10, ""}), 400, "invalid_input");
    CHECK_APP_ERROR(fx.svc.createVenue({"Bad\nName", 10, ""}), 400, "invalid_input");
    CHECK_EQ(fx.svc.listVenues().size(), size_t(1));
}

TEST(event_creation_and_validation) {
    Fx fx;
    const int64_t v = fx.venue("Hall", 100);
    json e = fx.svc.createEvent({"  Workshop ", "", "intro", v, "2026-11-02 10:00", "2026-11-02 12:00", 40});
    CHECK_EQ(e.at("title").get<std::string>(), std::string("Workshop"));
    CHECK_EQ(e.at("category").get<std::string>(), std::string("General"));  // default
    CHECK_EQ(e.at("seats_left").get<int>(), 40);
    CHECK_EQ(e.at("start").get<std::string>(), std::string("2026-11-02 10:00"));
    const std::string S = "2026-11-03 10:00";
    CHECK_APP_ERROR(fx.svc.createEvent({"", "", "", v, S, "2026-11-03 11:00", 10}), 400, "invalid_input");
    CHECK_APP_ERROR(fx.svc.createEvent({std::string(101, 'a'), "", "", v, S, "2026-11-03 11:00", 10}), 400, "invalid_input");
    CHECK_APP_ERROR(fx.svc.createEvent({"T", std::string(31, 'c'), "", v, S, "2026-11-03 11:00", 10}), 400, "invalid_input");
    CHECK_APP_ERROR(fx.svc.createEvent({"T", "", std::string(501, 'd'), v, S, "2026-11-03 11:00", 10}), 400, "invalid_input");
    CHECK_APP_ERROR(fx.svc.createEvent({"T", "", "", v, S, "2026-11-03 10:00", 10}), 400, "invalid_input");  // zero length
    CHECK_APP_ERROR(fx.svc.createEvent({"T", "", "", v, S, "2026-11-03 09:00", 10}), 400, "invalid_input");  // ends before start
    CHECK_APP_ERROR(fx.svc.createEvent({"T", "", "", v, S, "2026-11-03 10:10", 10}), 400, "invalid_input");  // under 15 minutes
    CHECK_APP_ERROR(fx.svc.createEvent({"T", "", "", v, S, "2026-11-03 22:30", 10}), 400, "invalid_input");  // over 12 hours
    CHECK_APP_ERROR(fx.svc.createEvent({"T", "", "", v, "nope", "2026-11-03 11:00", 10}), 400, "invalid_input");
    CHECK_APP_ERROR(fx.svc.createEvent({"T", "", "", v, S, "2026-02-30 11:00", 10}), 400, "invalid_input");
    CHECK_APP_ERROR(fx.svc.createEvent({"T", "", "", v, S, "2026-11-03 11:00", 0}), 400, "invalid_input");
    CHECK_APP_ERROR(fx.svc.createEvent({"T", "", "", v, S, "2026-11-03 11:00", -1}), 400, "invalid_input");
    CHECK_APP_ERROR(fx.svc.createEvent({"T", "", "", 999, S, "2026-11-03 11:00", 10}), 404, "not_found");
    CHECK_APP_ERROR(fx.svc.createEvent({"T\tx", "", "", v, S, "2026-11-03 11:00", 10}), 400, "invalid_input");
    // exactly 15 minutes and exactly 12 hours are allowed
    fx.event("Short", v, "2026-11-04 10:00", "2026-11-04 10:15", 10);
    fx.event("Long", v, "2026-11-05 08:00", "2026-11-05 20:00", 10);
    // an event may run past midnight
    fx.event("Overnight", v, "2026-11-06 22:00", "2026-11-07 02:00", 10);
    CHECK_EQ(fx.svc.getEvent(1).at("id").get<int>(), 1);
}

TEST(venue_capacity_check) {
    Fx fx;
    const int64_t v = fx.venue("Small Room", 30);
    CHECK_APP_ERROR(fx.event("Too big", v, "2026-11-02 10:00", "2026-11-02 11:00", 31), 400, "invalid_input");
    fx.event("Exactly full room", v, "2026-11-02 10:00", "2026-11-02 11:00", 30);  // equal capacity is fine
    CHECK_EQ(fx.svc.searchEvents({}).at("total").get<int>(), 1);                   // the refused one was not stored
}

TEST(clash_detection_cases) {
    Fx fx;
    const int64_t v = fx.venue();
    const int64_t base = fx.event("Base", v, "2026-11-02 10:00", "2026-11-02 12:00");
    // partial overlap at the start, partial overlap at the end, inside, around, identical, one minute of overlap
    CHECK_APP_ERROR(fx.event("A", v, "2026-11-02 09:00", "2026-11-02 10:30"), 409, "venue_clash");
    CHECK_APP_ERROR(fx.event("B", v, "2026-11-02 11:30", "2026-11-02 13:00"), 409, "venue_clash");
    CHECK_APP_ERROR(fx.event("C", v, "2026-11-02 10:30", "2026-11-02 11:30"), 409, "venue_clash");
    CHECK_APP_ERROR(fx.event("D", v, "2026-11-02 09:00", "2026-11-02 13:00"), 409, "venue_clash");
    CHECK_APP_ERROR(fx.event("E", v, "2026-11-02 10:00", "2026-11-02 12:00"), 409, "venue_clash");
    CHECK_APP_ERROR(fx.event("F", v, "2026-11-02 11:45", "2026-11-02 12:45"), 409, "venue_clash");
    CHECK_APP_ERROR(fx.event("G", v, "2026-11-02 09:00", "2026-11-02 10:15"), 409, "venue_clash");
    // back-to-back is allowed on both sides
    fx.event("Before", v, "2026-11-02 08:00", "2026-11-02 10:00");
    fx.event("After", v, "2026-11-02 12:00", "2026-11-02 13:00");
    // the refusal names the event in the way
    try {
        fx.event("Clash", v, "2026-11-02 11:00", "2026-11-02 11:30");
        CHECK(false);
    } catch (const AppError& e) {
        CHECK_EQ(e.details().at("clashes_with").at("id").get<int64_t>(), base);
        CHECK(std::string(e.what()).find("Base") != std::string::npos);
        CHECK(std::string(e.what()).find("10:00") != std::string::npos);
    }
    // a different venue, or a different day, is free
    const int64_t v2 = fx.venue("Other", 100);
    fx.event("Other venue", v2, "2026-11-02 10:00", "2026-11-02 12:00");
    fx.event("Next day", v, "2026-11-03 10:00", "2026-11-03 12:00");
}

TEST(clash_across_midnight) {
    Fx fx;
    const int64_t v = fx.venue();
    fx.event("Night", v, "2026-11-02 22:00", "2026-11-03 02:00");
    CHECK_APP_ERROR(fx.event("Early", v, "2026-11-03 01:00", "2026-11-03 03:00"), 409, "venue_clash");
    fx.event("Morning", v, "2026-11-03 02:00", "2026-11-03 03:00");  // starts exactly when "Night" ends
}

TEST(cancelled_event_frees_the_slot) {
    Fx fx;
    const int64_t v = fx.venue();
    const int64_t e = fx.event("Old", v, "2026-11-02 10:00", "2026-11-02 12:00");
    CHECK_APP_ERROR(fx.event("New", v, "2026-11-02 10:00", "2026-11-02 12:00"), 409, "venue_clash");
    json c = fx.svc.cancelEvent(e);
    CHECK_EQ(c.at("status").get<std::string>(), std::string("cancelled"));
    fx.event("New", v, "2026-11-02 10:00", "2026-11-02 12:00");  // now accepted
    CHECK_APP_ERROR(fx.svc.cancelEvent(e), 409, "already_cancelled");
    CHECK_APP_ERROR(fx.svc.cancelEvent(999), 404, "not_found");
}

// ================================================================== registration + waiting list
TEST(registration_fills_then_waits) {
    Fx fx;
    const int64_t e = fx.event("Talk", fx.venue(), "2026-11-02 10:00", "2026-11-02 11:00", 3);
    for (const char* s : {"S001", "S002", "S003"}) {
        json r = fx.reg(e, s);
        CHECK_EQ(r.at("status").get<std::string>(), std::string("confirmed"));
        CHECK(r.at("waiting_position").is_null());
    }
    json w1 = fx.reg(e, "S004");
    json w2 = fx.reg(e, "S005");
    CHECK_EQ(w1.at("status").get<std::string>(), std::string("waiting"));
    CHECK_EQ(w1.at("waiting_position").get<int>(), 1);
    CHECK_EQ(w2.at("waiting_position").get<int>(), 2);
    json info = fx.eventInfo(e);
    CHECK_EQ(info.at("confirmed").get<int>(), 3);
    CHECK_EQ(info.at("waiting").get<int>(), 2);
    CHECK(info.at("full").get<bool>());
    CHECK_EQ(info.at("seats_left").get<int>(), 0);
}

TEST(waiting_list_is_fifo_and_promotes_in_order) {
    Fx fx;
    const int64_t e = fx.event("Talk", fx.venue(), "2026-11-02 10:00", "2026-11-02 11:00", 2);
    fx.reg(e, "AAA");
    fx.reg(e, "BBB");
    fx.reg(e, "CCC");  // waiting #1
    fx.reg(e, "DDD");  // waiting #2
    fx.reg(e, "EEE");  // waiting #3
    json c1 = fx.svc.cancelRegistration(e, "AAA");
    CHECK_EQ(c1.at("was").get<std::string>(), std::string("confirmed"));
    CHECK_EQ(c1.at("promoted").at("student_id").get<std::string>(), std::string("CCC"));  // first in, first promoted
    json c2 = fx.svc.cancelRegistration(e, "BBB");
    CHECK_EQ(c2.at("promoted").at("student_id").get<std::string>(), std::string("DDD"));
    json list = fx.svc.attendees(e);
    CHECK_EQ(list.size(), size_t(3));
    CHECK_EQ(list[2].at("student_id").get<std::string>(), std::string("EEE"));
    CHECK_EQ(list[2].at("waiting_position").get<int>(), 1);  // EEE moved up to the front of the queue
    CHECK_EQ(fx.eventInfo(e).at("confirmed").get<int>(), 2);
    // a later sign-up joins the BACK of the queue, behind EEE
    json f = fx.reg(e, "FFF");
    CHECK_EQ(f.at("waiting_position").get<int>(), 2);
}

TEST(cancelling_a_waiting_student_promotes_nobody) {
    Fx fx;
    const int64_t e = fx.event("Talk", fx.venue(), "2026-11-02 10:00", "2026-11-02 11:00", 1);
    fx.reg(e, "AAA");
    fx.reg(e, "BBB");  // waiting #1
    fx.reg(e, "CCC");  // waiting #2
    json r = fx.svc.cancelRegistration(e, "BBB");
    CHECK_EQ(r.at("was").get<std::string>(), std::string("waiting"));
    CHECK(r.at("promoted").is_null());
    json list = fx.svc.attendees(e);
    CHECK_EQ(list[0].at("student_id").get<std::string>(), std::string("AAA"));
    CHECK_EQ(list[1].at("student_id").get<std::string>(), std::string("CCC"));
    CHECK_EQ(list[1].at("waiting_position").get<int>(), 1);
}

TEST(cancel_with_empty_waiting_list_just_frees_a_seat) {
    Fx fx;
    const int64_t e = fx.event("Talk", fx.venue(), "2026-11-02 10:00", "2026-11-02 11:00", 2);
    fx.reg(e, "AAA");
    fx.reg(e, "BBB");
    json r = fx.svc.cancelRegistration(e, "AAA");
    CHECK(r.at("promoted").is_null());
    CHECK_EQ(fx.eventInfo(e).at("seats_left").get<int>(), 1);
    json again = fx.reg(e, "CCC");  // the freed seat is usable
    CHECK_EQ(again.at("status").get<std::string>(), std::string("confirmed"));
}

TEST(duplicate_and_unknown_registrations) {
    Fx fx;
    const int64_t e = fx.event("Talk", fx.venue(), "2026-11-02 10:00", "2026-11-02 11:00", 1);
    fx.reg(e, "AAA");
    CHECK_APP_ERROR(fx.reg(e, "AAA"), 409, "already_registered");
    CHECK_APP_ERROR(fx.reg(e, "aaa"), 409, "already_registered");  // ids are case-insensitive
    fx.reg(e, "BBB");                                                // waiting
    CHECK_APP_ERROR(fx.reg(e, "BBB"), 409, "already_registered");   // also while waiting
    CHECK_APP_ERROR(fx.reg(999, "CCC"), 404, "not_found");
    CHECK_APP_ERROR(fx.svc.cancelRegistration(e, "ZZZ"), 404, "not_found");
    CHECK_APP_ERROR(fx.svc.cancelRegistration(999, "AAA"), 404, "not_found");
    CHECK_EQ(fx.eventInfo(e).at("waiting").get<int>(), 1);  // failed attempts changed nothing
}

TEST(registration_input_validation) {
    Fx fx;
    const int64_t e = fx.event("Talk", fx.venue(), "2026-11-02 10:00", "2026-11-02 11:00", 5);
    CHECK_APP_ERROR(fx.svc.registerStudent(e, "", "Asha"), 400, "invalid_input");
    CHECK_APP_ERROR(fx.svc.registerStudent(e, "ab", "Asha"), 400, "invalid_input");
    CHECK_APP_ERROR(fx.svc.registerStudent(e, "A B C", "Asha"), 400, "invalid_input");
    CHECK_APP_ERROR(fx.svc.registerStudent(e, "ok-id", ""), 400, "invalid_input");
    CHECK_APP_ERROR(fx.svc.registerStudent(e, "ok-id", "   "), 400, "invalid_input");
    CHECK_APP_ERROR(fx.svc.registerStudent(e, "ok-id", std::string(61, 'n')), 400, "invalid_input");
    CHECK_APP_ERROR(fx.svc.registerStudent(e, "ok-id", "Bad\nName"), 400, "invalid_input");
    json ok = fx.svc.registerStudent(e, "  ok_id-1 ", "  Zoë Nair  ");
    CHECK_EQ(ok.at("student_id").get<std::string>(), std::string("OK_ID-1"));
    CHECK_EQ(ok.at("name").get<std::string>(), std::string("Zoë Nair"));  // trimmed, unicode kept
    CHECK_EQ(fx.eventInfo(e).at("confirmed").get<int>(), 1);
}

TEST(student_cannot_be_in_two_places) {
    Fx fx;
    const int64_t v1 = fx.venue("A", 100), v2 = fx.venue("B", 100);
    const int64_t e1 = fx.event("One", v1, "2026-11-02 10:00", "2026-11-02 12:00");
    const int64_t e2 = fx.event("Two (overlaps)", v2, "2026-11-02 11:00", "2026-11-02 13:00");
    const int64_t e3 = fx.event("Three (back to back)", v1, "2026-11-02 12:00", "2026-11-02 13:00");
    const int64_t e4 = fx.event("Four (other day)", v2, "2026-11-03 11:00", "2026-11-03 13:00");
    fx.reg(e1, "AAA");
    try {
        fx.reg(e2, "AAA");
        CHECK(false);
    } catch (const AppError& e) {
        CHECK_EQ(e.status(), 409);
        CHECK_EQ(e.code(), std::string("student_clash"));
        CHECK_EQ(e.details().at("clashes_with").at("id").get<int64_t>(), e1);
    }
    CHECK_EQ(fx.reg(e3, "AAA").at("status").get<std::string>(), std::string("confirmed"));  // touching is fine
    CHECK_EQ(fx.reg(e4, "AAA").at("status").get<std::string>(), std::string("confirmed"));
    // e2 (11:00-13:00) overlaps both e1 and e3, so it is only allowed once both are cancelled
    fx.svc.cancelRegistration(e1, "AAA");
    CHECK_APP_ERROR(fx.reg(e2, "AAA"), 409, "student_clash");  // still blocked by e3
    fx.svc.cancelRegistration(e3, "AAA");
    CHECK_EQ(fx.reg(e2, "AAA").at("status").get<std::string>(), std::string("confirmed"));
}

TEST(waiting_registration_also_blocks_overlap_and_cancelled_events_do_not) {
    Fx fx;
    const int64_t v1 = fx.venue("A", 100), v2 = fx.venue("B", 100);
    const int64_t full = fx.event("Full", v1, "2026-11-02 10:00", "2026-11-02 12:00", 1);
    const int64_t other = fx.event("Other", v2, "2026-11-02 11:00", "2026-11-02 12:30");
    fx.reg(full, "XXX");
    CHECK_EQ(fx.reg(full, "AAA").at("status").get<std::string>(), std::string("waiting"));
    CHECK_APP_ERROR(fx.reg(other, "AAA"), 409, "student_clash");  // waiting still counts: promotion must never create a clash
    fx.svc.cancelEvent(full);
    CHECK_EQ(fx.reg(other, "AAA").at("status").get<std::string>(), std::string("confirmed"));  // cancelled event no longer blocks
}

TEST(cancelled_event_rules) {
    Fx fx;
    const int64_t e = fx.event("Talk", fx.venue(), "2026-11-02 10:00", "2026-11-02 11:00", 1);
    fx.reg(e, "AAA");
    fx.reg(e, "BBB");  // waiting
    fx.svc.cancelEvent(e);
    CHECK_APP_ERROR(fx.reg(e, "CCC"), 409, "event_cancelled");
    json r = fx.svc.cancelRegistration(e, "AAA");  // allowed (clean-up) but nobody is promoted into a dead event
    CHECK(r.at("promoted").is_null());
    CHECK_APP_ERROR(fx.svc.checkIn(e, "BBB"), 409, "event_cancelled");
    CHECK_EQ(fx.svc.searchEvents({}).at("total").get<int>(), 0);  // hidden from the default (active) listing
}

TEST(check_in_rules) {
    Fx fx;
    const int64_t e = fx.event("Talk", fx.venue(), "2026-11-02 10:00", "2026-11-02 11:00", 1);
    fx.reg(e, "AAA");
    fx.reg(e, "BBB");  // waiting
    json c = fx.svc.checkIn(e, "aaa");
    CHECK(!c.at("already_checked_in").get<bool>());
    CHECK(fx.svc.checkIn(e, "AAA").at("already_checked_in").get<bool>());  // idempotent
    CHECK_APP_ERROR(fx.svc.checkIn(e, "BBB"), 409, "not_confirmed");
    CHECK_APP_ERROR(fx.svc.checkIn(e, "ZZZ"), 404, "not_found");
    CHECK_APP_ERROR(fx.svc.checkIn(999, "AAA"), 404, "not_found");
    CHECK_EQ(fx.eventInfo(e).at("checked_in").get<int>(), 1);
}

TEST(student_registration_list) {
    Fx fx;
    const int64_t v = fx.venue();
    const int64_t e1 = fx.event("First", v, "2026-11-02 10:00", "2026-11-02 11:00", 1);
    const int64_t e2 = fx.event("Second", v, "2026-11-03 10:00", "2026-11-03 11:00", 1);
    fx.reg(e1, "AAA");
    fx.reg(e2, "BBB");
    fx.reg(e2, "AAA");  // waiting
    json rows = fx.svc.studentRegistrations("aaa");
    CHECK_EQ(rows.size(), size_t(2));
    CHECK_EQ(rows[0].at("title").get<std::string>(), std::string("First"));  // ordered by event time
    CHECK_EQ(rows[1].at("status").get<std::string>(), std::string("waiting"));
    CHECK_EQ(rows[1].at("waiting_position").get<int>(), 1);
    CHECK_EQ(fx.svc.studentRegistrations("NOBODY").size(), size_t(0));
}

// ================================================================== free-slot finder
TEST(free_slots_basic) {
    Fx fx;
    const int64_t v = fx.venue();
    fx.event("A", v, "2026-11-02 09:00", "2026-11-02 10:00");
    fx.event("B", v, "2026-11-02 10:00", "2026-11-02 11:30");  // touches A: no zero-length gap between them
    fx.event("C", v, "2026-11-02 13:00", "2026-11-02 14:00");
    json r = fx.svc.freeSlots(v, "2026-11-02", "08:00", "20:00", 60);
    const json& s = r.at("free_slots");
    CHECK_EQ(s.size(), size_t(3));
    CHECK_EQ(s[0].at("start").get<std::string>(), std::string("2026-11-02 08:00"));
    CHECK_EQ(s[0].at("minutes").get<int>(), 60);  // exactly the minimum is kept
    CHECK_EQ(s[1].at("start").get<std::string>(), std::string("2026-11-02 11:30"));
    CHECK_EQ(s[1].at("end").get<std::string>(), std::string("2026-11-02 13:00"));
    CHECK_EQ(s[1].at("minutes").get<int>(), 90);
    CHECK_EQ(s[2].at("minutes").get<int>(), 360);
    CHECK_EQ(r.at("booked").size(), size_t(3));
    CHECK_EQ(fx.svc.freeSlots(v, "2026-11-02", "08:00", "20:00", 61).at("free_slots").size(), size_t(2));
    CHECK_EQ(fx.svc.freeSlots(v, "2026-11-02", "08:00", "20:00", 1).at("free_slots").size(), size_t(3));
    // an empty day is one big slot; other days and other venues do not interfere
    CHECK_EQ(fx.svc.freeSlots(v, "2026-11-03", "08:00", "20:00", 30).at("free_slots").size(), size_t(1));
    const int64_t v2 = fx.venue("Other", 50);
    CHECK_EQ(fx.svc.freeSlots(v2, "2026-11-02", "08:00", "20:00", 30).at("free_slots").size(), size_t(1));
}

TEST(free_slots_edge_cases) {
    Fx fx;
    const int64_t v = fx.venue();
    fx.event("Starts before window", v, "2026-11-02 07:00", "2026-11-02 09:00");
    fx.event("Ends after window", v, "2026-11-02 19:00", "2026-11-02 21:00");
    json r = fx.svc.freeSlots(v, "2026-11-02", "08:00", "20:00", 30);
    CHECK_EQ(r.at("free_slots").size(), size_t(1));  // clipped to 09:00 - 19:00
    CHECK_EQ(r.at("free_slots")[0].at("start").get<std::string>(), std::string("2026-11-02 09:00"));
    CHECK_EQ(r.at("free_slots")[0].at("end").get<std::string>(), std::string("2026-11-02 19:00"));
    // overnight event spills into the next day's early hours
    fx.event("Overnight", v, "2026-11-05 22:00", "2026-11-06 02:00");
    json n = fx.svc.freeSlots(v, "2026-11-06", "00:00", "24:00", 30);
    CHECK_EQ(n.at("free_slots").size(), size_t(1));
    CHECK_EQ(n.at("free_slots")[0].at("start").get<std::string>(), std::string("2026-11-06 02:00"));
    CHECK_EQ(n.at("free_slots")[0].at("end").get<std::string>(), std::string("2026-11-07 00:00"));
    // fully booked window
    fx.event("All day", v, "2026-11-08 08:00", "2026-11-08 20:00");
    CHECK_EQ(fx.svc.freeSlots(v, "2026-11-08", "08:00", "20:00", 1).at("free_slots").size(), size_t(0));
    // cancelled events do not block
    const int64_t c = fx.event("Cancelled", v, "2026-11-09 10:00", "2026-11-09 12:00");
    fx.svc.cancelEvent(c);
    CHECK_EQ(fx.svc.freeSlots(v, "2026-11-09", "08:00", "20:00", 30).at("booked").size(), size_t(0));
}

TEST(free_slots_validation) {
    Fx fx;
    const int64_t v = fx.venue();
    CHECK_APP_ERROR(fx.svc.freeSlots(v, "2026-13-01", "08:00", "20:00", 30), 400, "invalid_input");
    CHECK_APP_ERROR(fx.svc.freeSlots(v, "2026-11-02", "8:00", "20:00", 30), 400, "invalid_input");
    CHECK_APP_ERROR(fx.svc.freeSlots(v, "2026-11-02", "08:00", "25:00", 30), 400, "invalid_input");
    CHECK_APP_ERROR(fx.svc.freeSlots(v, "2026-11-02", "20:00", "08:00", 30), 400, "invalid_input");
    CHECK_APP_ERROR(fx.svc.freeSlots(v, "2026-11-02", "10:00", "10:00", 30), 400, "invalid_input");
    CHECK_APP_ERROR(fx.svc.freeSlots(v, "2026-11-02", "08:00", "20:00", 0), 400, "invalid_input");
    CHECK_APP_ERROR(fx.svc.freeSlots(v, "2026-11-02", "08:00", "20:00", 1441), 400, "invalid_input");
    CHECK_APP_ERROR(fx.svc.freeSlots(999, "2026-11-02", "08:00", "20:00", 30), 404, "not_found");
}

// ================================================================== search and filters
static void searchFixture(Fx& fx, int64_t& hall, int64_t& lab) {
    hall = fx.venue("Main Hall", 200);
    lab = fx.venue("Computer Lab", 40);
    fx.event("Python Workshop", lab, "2026-11-02 10:00", "2026-11-02 12:00", 30, "Workshop");
    fx.event("Cricket Match", hall, "2026-11-03 10:00", "2026-11-03 12:00", 100, "Sports");
    fx.event("100% Fun Run", hall, "2026-11-04 10:00", "2026-11-04 12:00", 100, "Sports");
    fx.event("Open_Mic Night", hall, "2026-11-05 18:00", "2026-11-05 20:00", 150, "Cultural");
}

TEST(search_filters) {
    Fx fx;
    int64_t hall = 0, lab = 0;
    searchFixture(fx, hall, lab);
    auto titles = [](const json& r) {
        std::vector<std::string> t;
        for (const json& i : r.at("items")) t.push_back(i.at("title").get<std::string>());
        return t;
    };
    EventQuery q;
    CHECK_EQ(fx.svc.searchEvents(q).at("total").get<int>(), 4);
    q.text = "python";  // case-insensitive
    CHECK_EQ(titles(fx.svc.searchEvents(q)).size(), size_t(1));
    q = {}; q.text = "COMPUTER LAB";  // matches the venue name
    CHECK_EQ(fx.svc.searchEvents(q).at("total").get<int>(), 1);
    q = {}; q.text = "sports";  // matches the category
    CHECK_EQ(fx.svc.searchEvents(q).at("total").get<int>(), 2);
    q = {}; q.venueId = hall;
    CHECK_EQ(fx.svc.searchEvents(q).at("total").get<int>(), 3);
    q = {}; q.category = "sports";
    CHECK_EQ(fx.svc.searchEvents(q).at("total").get<int>(), 2);
    q = {}; q.from = "2026-11-03"; q.to = "2026-11-04";  // both days inclusive
    auto r = fx.svc.searchEvents(q);
    CHECK_EQ(r.at("total").get<int>(), 2);
    CHECK_EQ(titles(r)[0], std::string("Cricket Match"));
    q = {}; q.from = "2026-11-05";
    CHECK_EQ(fx.svc.searchEvents(q).at("total").get<int>(), 1);
    q = {}; q.to = "2026-11-02";  // the whole last day counts
    CHECK_EQ(fx.svc.searchEvents(q).at("total").get<int>(), 1);
    q = {}; q.venueId = hall; q.category = "Sports"; q.text = "cricket";  // filters combine with AND
    CHECK_EQ(fx.svc.searchEvents(q).at("total").get<int>(), 1);
    q = {}; q.text = "no such thing";
    CHECK_EQ(fx.svc.searchEvents(q).at("total").get<int>(), 0);
}

TEST(search_treats_wildcards_literally) {
    Fx fx;
    int64_t hall = 0, lab = 0;
    searchFixture(fx, hall, lab);
    EventQuery q;
    q.text = "%";
    json r = fx.svc.searchEvents(q);
    CHECK_EQ(r.at("total").get<int>(), 1);  // only "100% Fun Run", not everything
    CHECK_EQ(r.at("items")[0].at("title").get<std::string>(), std::string("100% Fun Run"));
    q.text = "_";
    r = fx.svc.searchEvents(q);
    CHECK_EQ(r.at("total").get<int>(), 1);
    CHECK_EQ(r.at("items")[0].at("title").get<std::string>(), std::string("Open_Mic Night"));
    q.text = "\\";
    CHECK_EQ(fx.svc.searchEvents(q).at("total").get<int>(), 0);
}

TEST(search_sort_and_paging) {
    Fx fx;
    int64_t hall = 0, lab = 0;
    searchFixture(fx, hall, lab);
    const int64_t e1 = 1, e4 = 4;
    for (int i = 0; i < 3; ++i) fx.reg(e4, "ST" + std::to_string(100 + i));  // Open_Mic: 3 registered
    fx.reg(e1, "ST200");                                                   // Python: 1 registered
    EventQuery q;
    q.sort = "popularity"; q.order = "desc";
    CHECK_EQ(fx.svc.searchEvents(q).at("items")[0].at("title").get<std::string>(), std::string("Open_Mic Night"));
    q = {}; q.sort = "title";
    CHECK_EQ(fx.svc.searchEvents(q).at("items")[0].at("title").get<std::string>(), std::string("100% Fun Run"));
    q = {}; q.sort = "seats"; q.order = "asc";  // fewest seats left first: Python (30-1=29) is the smallest
    CHECK_EQ(fx.svc.searchEvents(q).at("items")[0].at("title").get<std::string>(), std::string("Python Workshop"));
    q = {}; q.order = "desc";
    CHECK_EQ(fx.svc.searchEvents(q).at("items")[0].at("title").get<std::string>(), std::string("Open_Mic Night"));
    q = {}; q.limit = 3; q.offset = 3;
    json p = fx.svc.searchEvents(q);
    CHECK_EQ(p.at("total").get<int>(), 4);  // total ignores the page window
    CHECK_EQ(p.at("items").size(), size_t(1));
    q.offset = 99;
    CHECK_EQ(fx.svc.searchEvents(q).at("items").size(), size_t(0));
}

TEST(search_status_filter) {
    Fx fx;
    const int64_t v = fx.venue();
    fx.event("Keep", v, "2026-11-02 10:00", "2026-11-02 11:00");
    const int64_t gone = fx.event("Gone", v, "2026-11-03 10:00", "2026-11-03 11:00");
    fx.svc.cancelEvent(gone);
    EventQuery q;
    CHECK_EQ(fx.svc.searchEvents(q).at("total").get<int>(), 1);
    q.status = "cancelled";
    CHECK_EQ(fx.svc.searchEvents(q).at("total").get<int>(), 1);
    q.status = "all";
    CHECK_EQ(fx.svc.searchEvents(q).at("total").get<int>(), 2);
}

TEST(search_validation) {
    Fx fx;
    EventQuery q;
    q.status = "weird"; CHECK_APP_ERROR(fx.svc.searchEvents(q), 400, "invalid_input");
    q = {}; q.sort = "id; DROP TABLE events"; CHECK_APP_ERROR(fx.svc.searchEvents(q), 400, "invalid_input");
    q = {}; q.order = "sideways"; CHECK_APP_ERROR(fx.svc.searchEvents(q), 400, "invalid_input");
    q = {}; q.limit = 0; CHECK_APP_ERROR(fx.svc.searchEvents(q), 400, "invalid_input");
    q = {}; q.limit = 201; CHECK_APP_ERROR(fx.svc.searchEvents(q), 400, "invalid_input");
    q = {}; q.offset = -1; CHECK_APP_ERROR(fx.svc.searchEvents(q), 400, "invalid_input");
    q = {}; q.from = "yesterday"; CHECK_APP_ERROR(fx.svc.searchEvents(q), 400, "invalid_input");
    q = {}; q.to = "2026-02-30"; CHECK_APP_ERROR(fx.svc.searchEvents(q), 400, "invalid_input");
    q = {}; q.from = "2026-11-05"; q.to = "2026-11-01"; CHECK_APP_ERROR(fx.svc.searchEvents(q), 400, "invalid_input");
    q = {}; q.text = std::string(101, 'x'); CHECK_APP_ERROR(fx.svc.searchEvents(q), 400, "invalid_input");
}

TEST(sql_injection_text_is_inert) {
    Fx fx;
    const int64_t v = fx.venue("O'Brien Hall", 100);
    const std::string evil = "x'); DROP TABLE events; --";
    fx.svc.createEvent({evil, "Tech", "", v, "2026-11-02 10:00", "2026-11-02 11:00", 10});
    const int64_t id = fx.svc.searchEvents({}).at("items")[0].at("id").get<int64_t>();
    json r = fx.svc.registerStudent(id, "SQL-1", "Robert'); DROP TABLE students;--");
    CHECK_EQ(r.at("status").get<std::string>(), std::string("confirmed"));
    EventQuery q;
    q.text = "DROP TABLE";
    CHECK_EQ(fx.svc.searchEvents(q).at("total").get<int>(), 1);  // stored and found as plain text
    CHECK_EQ(fx.svc.searchEvents({}).at("items")[0].at("title").get<std::string>(), evil);
    CHECK_EQ(fx.db.scalar("SELECT COUNT(*) FROM students"), int64_t(1));  // tables are intact
}

// ================================================================== reports
TEST(attendance_report_numbers) {
    Fx fx;
    const int64_t v = fx.venue();
    const int64_t e = fx.event("Talk", v, "2026-11-02 10:00", "2026-11-02 11:00", 2);
    fx.reg(e, "AAA");
    fx.reg(e, "BBB");
    fx.reg(e, "CCC");  // waiting
    fx.svc.checkIn(e, "AAA");
    const int64_t empty = fx.event("Quiet", v, "2026-11-03 10:00", "2026-11-03 11:00", 40);
    (void)empty;
    json rep = fx.svc.attendanceReport(false);
    CHECK_EQ(rep.size(), size_t(2));
    CHECK_EQ(rep[0].at("confirmed").get<int>(), 2);
    CHECK_EQ(rep[0].at("waiting").get<int>(), 1);
    CHECK_EQ(rep[0].at("checked_in").get<int>(), 1);
    CHECK_EQ(rep[0].at("fill_pct").get<double>(), 100.0);
    CHECK_EQ(rep[0].at("show_up_pct").get<double>(), 50.0);
    CHECK_EQ(rep[1].at("confirmed").get<int>(), 0);  // an event nobody joined still appears (LEFT JOIN)
    CHECK_EQ(rep[1].at("fill_pct").get<double>(), 0.0);
    CHECK_EQ(rep[1].at("show_up_pct").get<double>(), 0.0);  // no divide-by-zero
    fx.svc.cancelEvent(empty);
    CHECK_EQ(fx.svc.attendanceReport(false).size(), size_t(1));
    CHECK_EQ(fx.svc.attendanceReport(true).size(), size_t(2));
}

TEST(popularity_report_order) {
    Fx fx;
    const int64_t v = fx.venue();
    const int64_t big = fx.event("Big", v, "2026-11-02 10:00", "2026-11-02 11:00", 2);
    const int64_t mid = fx.event("Mid", v, "2026-11-03 10:00", "2026-11-03 11:00", 10);
    const int64_t tieEarly = fx.event("TieEarly", v, "2026-11-04 10:00", "2026-11-04 11:00", 10);
    const int64_t tieLate = fx.event("TieLate", v, "2026-11-05 10:00", "2026-11-05 11:00", 10);
    for (int i = 0; i < 5; ++i) fx.reg(big, "B" + std::to_string(100 + i));   // 2 confirmed + 3 waiting = demand 5
    for (int i = 0; i < 4; ++i) fx.reg(mid, "M" + std::to_string(100 + i));   // demand 4
    for (int i = 0; i < 3; ++i) fx.reg(tieLate, "L" + std::to_string(100 + i));
    for (int i = 0; i < 3; ++i) fx.reg(tieEarly, "E" + std::to_string(100 + i));
    json rep = fx.svc.popularityReport(10);
    CHECK_EQ(rep.size(), size_t(4));
    CHECK_EQ(rep[0].at("title").get<std::string>(), std::string("Big"));     // waiting-list demand counts
    CHECK_EQ(rep[0].at("demand").get<int>(), 5);
    CHECK_EQ(rep[1].at("title").get<std::string>(), std::string("Mid"));
    CHECK_EQ(rep[2].at("title").get<std::string>(), std::string("TieEarly"));  // equal demand: earlier event first
    CHECK_EQ(fx.svc.popularityReport(2).size(), size_t(2));
    CHECK_APP_ERROR(fx.svc.popularityReport(0), 400, "invalid_input");
    CHECK_APP_ERROR(fx.svc.popularityReport(101), 400, "invalid_input");
}

TEST(venue_usage_report_does_not_multiply_by_attendees) {
    Fx fx;
    const int64_t a = fx.venue("A", 100);
    fx.venue("Unused", 50);
    const int64_t e1 = fx.event("Two hours", a, "2026-11-02 10:00", "2026-11-02 12:00", 10);
    fx.event("Three hours", a, "2026-11-03 10:00", "2026-11-03 13:00", 10);
    for (int i = 0; i < 5; ++i) fx.reg(e1, "U" + std::to_string(100 + i));  // many attendees must not inflate hours
    json rep = fx.svc.venueUsageReport();
    CHECK_EQ(rep.size(), size_t(2));
    CHECK_EQ(rep[0].at("name").get<std::string>(), std::string("A"));
    CHECK_EQ(rep[0].at("events").get<int>(), 2);
    CHECK_EQ(rep[0].at("booked_hours").get<double>(), 5.0);
    CHECK_EQ(rep[0].at("confirmed").get<int>(), 5);
    CHECK_EQ(rep[0].at("avg_fill_pct").get<double>(), 25.0);  // 5 confirmed of 20 seats
    CHECK_EQ(rep[1].at("name").get<std::string>(), std::string("Unused"));  // venues with no events still listed
    CHECK_EQ(rep[1].at("events").get<int>(), 0);
    CHECK_EQ(rep[1].at("booked_hours").get<double>(), 0.0);
    CHECK_EQ(rep[1].at("avg_fill_pct").get<double>(), 0.0);
}

TEST(waitlist_report_and_stats) {
    Fx fx;
    const int64_t v = fx.venue();
    const int64_t full = fx.event("Full", v, "2026-11-02 10:00", "2026-11-02 11:00", 1);
    const int64_t open = fx.event("Open", v, "2026-11-03 10:00", "2026-11-03 11:00", 5);
    fx.reg(full, "AAA"); fx.reg(full, "BBB"); fx.reg(full, "CCC");
    fx.reg(open, "AAA");
    json w = fx.svc.waitlistReport();
    CHECK_EQ(w.size(), size_t(1));
    CHECK_EQ(w[0].at("title").get<std::string>(), std::string("Full"));
    CHECK_EQ(w[0].at("waiting").get<int>(), 2);
    json s = fx.svc.stats();
    CHECK_EQ(s.at("venues").get<int>(), 1);
    CHECK_EQ(s.at("events").get<int>(), 2);
    CHECK_EQ(s.at("students").get<int>(), 3);
    CHECK_EQ(s.at("confirmed").get<int>(), 2);
    CHECK_EQ(s.at("waiting").get<int>(), 2);
    fx.svc.cancelEvent(full);  // cancelled events leave the live counts
    CHECK_EQ(fx.svc.waitlistReport().size(), size_t(0));
    CHECK_EQ(fx.svc.stats().at("waiting").get<int>(), 0);
    CHECK_EQ(fx.svc.stats().at("cancelled_events").get<int>(), 1);
}

TEST(csv_export) {
    Fx fx;
    const int64_t e = fx.event("Talk", fx.venue(), "2026-11-02 10:00", "2026-11-02 11:00", 1);
    fx.svc.registerStudent(e, "AAA", "=cmd|' /C calc'!A0");     // spreadsheet formula injection attempt
    fx.svc.registerStudent(e, "BBB", "Smith, John \"JJ\"");      // comma and quotes
    fx.svc.checkIn(e, "AAA");
    const std::string csv = fx.svc.attendeesCsv(e);
    CHECK(csv.rfind("student_id,name,status,waiting_position,checked_in\r\n", 0) == 0);
    CHECK(csv.find(",'=cmd") != std::string::npos);                 // formula neutralised
    CHECK(csv.find(",=cmd") == std::string::npos);
    CHECK(csv.find("\"Smith, John \"\"JJ\"\"\",waiting,1,no") != std::string::npos);
    CHECK(csv.find("AAA,'=cmd|' /C calc'!A0,confirmed,,yes") != std::string::npos ||
          csv.find("AAA,\"'=cmd|' /C calc'!A0\",confirmed,,yes") != std::string::npos);
    CHECK_APP_ERROR(fx.svc.attendeesCsv(999), 404, "not_found");
}

// ================================================================== database integrity (second line of defence)
TEST(database_triggers_block_bad_writes) {
    Fx fx;
    const int64_t v = fx.venue("Hall", 10);
    const int64_t e = fx.event("Talk", v, "2026-11-02 10:00", "2026-11-02 11:00", 1);
    // overlapping active event written straight to SQL, bypassing the C++ rules
    bool threw = false;
    try { fx.db.exec("INSERT INTO events(title,venue_id,start_ts,end_ts,capacity) VALUES('x'," + std::to_string(v) + "," +
                     std::to_string(Tm("2026-11-02 10:30")) + "," + std::to_string(Tm("2026-11-02 11:30")) + ",1)"); }
    catch (const DbError&) { threw = true; }
    CHECK(threw);
    threw = false;
    try { fx.db.exec("INSERT INTO events(title,venue_id,start_ts,end_ts,capacity) VALUES('big'," + std::to_string(v) + "," +
                     std::to_string(Tm("2026-11-09 10:00")) + "," + std::to_string(Tm("2026-11-09 11:00")) + ",11)"); }
    catch (const DbError&) { threw = true; }
    CHECK(threw);  // bigger than the venue
    threw = false;
    try { fx.db.exec("INSERT INTO events(title,venue_id,start_ts,end_ts,capacity) VALUES('bad'," + std::to_string(v) + "," +
                     std::to_string(Tm("2026-11-10 12:00")) + "," + std::to_string(Tm("2026-11-10 11:00")) + ",1)"); }
    catch (const DbError&) { threw = true; }
    CHECK(threw);  // end before start
    // over-booking straight to SQL
    fx.reg(e, "AAA");
    fx.db.exec("INSERT INTO students(id,name) VALUES('ZZZ','Z')");
    threw = false;
    try { fx.db.exec("INSERT INTO registrations(event_id,student_id,status) VALUES(" + std::to_string(e) + ",'ZZZ','confirmed')"); }
    catch (const DbError&) { threw = true; }
    CHECK(threw);
    fx.db.exec("INSERT INTO registrations(event_id,student_id,status) VALUES(" + std::to_string(e) + ",'ZZZ','waiting')");
    threw = false;
    try { fx.db.exec("UPDATE registrations SET status='confirmed' WHERE student_id='ZZZ'"); }
    catch (const DbError&) { threw = true; }
    CHECK(threw);  // promoting into a full event
    threw = false;
    try { fx.db.exec("INSERT INTO registrations(event_id,student_id,status) VALUES(12345,'ZZZ','waiting')"); }
    catch (const DbError&) { threw = true; }
    CHECK(threw);  // foreign key: no such event
    threw = false;
    try { fx.db.exec("INSERT INTO registrations(event_id,student_id,status) VALUES(" + std::to_string(e) + ",'ZZZ','waiting')"); }
    catch (const DbError&) { threw = true; }
    CHECK(threw);  // unique (event, student)
}

TEST(transaction_rolls_back_on_failure) {
    Fx fx;
    fx.venue("Hall", 10);
    {
        Transaction tx(fx.db);
        fx.db.exec("INSERT INTO venues(name,capacity) VALUES('Temp',5)");
        // leaving the scope without commit() must undo the insert
    }
    CHECK_EQ(fx.db.scalar("SELECT COUNT(*) FROM venues"), int64_t(1));
    {
        Transaction tx(fx.db);
        fx.db.exec("INSERT INTO venues(name,capacity) VALUES('Kept',5)");
        tx.commit();
    }
    CHECK_EQ(fx.db.scalar("SELECT COUNT(*) FROM venues"), int64_t(2));
    // A refused registration or event leaves no half-written rows behind.
    const int64_t v = fx.venue("Other", 50);
    fx.event("A", v, "2026-11-02 10:00", "2026-11-02 11:00");
    const int64_t before = fx.db.scalar("SELECT COUNT(*) FROM events");
    CHECK_APP_ERROR(fx.event("B", v, "2026-11-02 10:30", "2026-11-02 11:30"), 409, "venue_clash");
    CHECK_EQ(fx.db.scalar("SELECT COUNT(*) FROM events"), before);
    CHECK_APP_ERROR(fx.svc.registerStudent(1, "ok1", ""), 400, "invalid_input");
    CHECK_EQ(fx.db.scalar("SELECT COUNT(*) FROM students"), int64_t(0));
}

TEST(database_persists_between_opens) {
    const std::string path = "ce_test_persist.db";
    std::remove(path.c_str());
    {
        Database db(path);
        db.initSchema();
        EventService svc(db);
        const int64_t v = svc.createVenue({"Hall", 50, ""}).at("id").get<int64_t>();
        svc.createEvent({"Saved", "Tech", "", v, "2026-11-02 10:00", "2026-11-02 11:00", 5});
        svc.registerStudent(1, "AAA", "Asha");
    }
    {
        Database db(path);
        db.initSchema();  // running it again on an existing database must be harmless
        EventService svc(db);
        CHECK_EQ(svc.getEvent(1).at("title").get<std::string>(), std::string("Saved"));
        CHECK_EQ(svc.getEvent(1).at("confirmed").get<int>(), 1);
    }
    std::remove(path.c_str());
    std::remove((path + "-wal").c_str());
    std::remove((path + "-shm").c_str());
}

// ================================================================== concurrency + model-based testing
TEST(concurrent_registration_never_overbooks) {
    Fx fx;
    const int64_t e = fx.event("Rush", fx.venue(), "2026-11-02 10:00", "2026-11-02 11:00", 5);
    std::vector<std::thread> threads;
    std::atomic<int> confirmed{0}, waiting{0};
    for (int i = 0; i < 24; ++i) {
        threads.emplace_back([&, i] {
            json r = fx.svc.registerStudent(e, "RUSH" + std::to_string(1000 + i), "Student");
            (r.at("status") == "confirmed" ? confirmed : waiting)++;
        });
    }
    for (auto& t : threads) t.join();
    CHECK_EQ(confirmed.load(), 5);
    CHECK_EQ(waiting.load(), 19);
    json list = fx.svc.attendees(e);
    std::set<int> positions;
    for (const json& a : list) if (a.at("status") == "waiting") positions.insert(a.at("waiting_position").get<int>());
    CHECK_EQ(positions.size(), size_t(19));  // every waiting student has a distinct queue position 1..19
    CHECK_EQ(*positions.begin(), 1);
    CHECK_EQ(*positions.rbegin(), 19);
}

TEST(concurrent_register_and_cancel_keeps_invariants) {
    Fx fx;
    const int64_t e = fx.event("Busy", fx.venue(), "2026-11-02 10:00", "2026-11-02 11:00", 10);
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&, t] {
            std::mt19937 rng(100 + t);
            for (int i = 0; i < 250; ++i) {
                const std::string sid = "C" + std::to_string(100 + static_cast<int>(rng() % 40));
                try {
                    if (rng() % 2) fx.svc.registerStudent(e, sid, "S");
                    else fx.svc.cancelRegistration(e, sid);
                } catch (const AppError&) { /* duplicates / not found are expected under contention */ }
            }
        });
    }
    for (auto& t : threads) t.join();
    json info = fx.svc.getEvent(e);
    CHECK(info.at("confirmed").get<int>() <= 10);
    if (info.at("waiting").get<int>() > 0) CHECK_EQ(info.at("confirmed").get<int>(), 10);  // never an idle seat while people wait
    json list = fx.svc.attendees(e);
    int expectPos = 1;
    for (const json& a : list) {
        if (a.at("status") == "waiting") CHECK_EQ(a.at("waiting_position").get<int>(), expectPos++);
    }
}

TEST(random_operations_match_a_simple_model) {
    // Compare the real system with a tiny in-memory model (a vector + a queue) over thousands of random operations.
    Fx fx;
    const int64_t e = fx.event("Model", fx.venue(), "2026-11-02 10:00", "2026-11-02 11:00", 4);
    std::vector<std::string> confirmed;
    std::deque<std::string> waiting;
    std::mt19937 rng(7);
    for (int step = 0; step < 4000; ++step) {
        const std::string sid = "M" + std::to_string(100 + static_cast<int>(rng() % 12));
        const bool inConfirmed = std::find(confirmed.begin(), confirmed.end(), sid) != confirmed.end();
        const bool inWaiting = std::find(waiting.begin(), waiting.end(), sid) != waiting.end();
        if (rng() % 2) {
            if (inConfirmed || inWaiting) { CHECK_APP_ERROR(fx.reg(e, sid), 409, "already_registered"); continue; }
            json r = fx.reg(e, sid);
            if (confirmed.size() < 4) { confirmed.push_back(sid); CHECK(r.at("status") == "confirmed"); }
            else { waiting.push_back(sid); CHECK(r.at("status") == "waiting"); }
        } else {
            if (!inConfirmed && !inWaiting) { CHECK_APP_ERROR(fx.svc.cancelRegistration(e, sid), 404, "not_found"); continue; }
            json r = fx.svc.cancelRegistration(e, sid);
            if (inWaiting) { waiting.erase(std::find(waiting.begin(), waiting.end(), sid)); CHECK(r.at("promoted").is_null()); }
            else {
                confirmed.erase(std::find(confirmed.begin(), confirmed.end(), sid));
                if (!waiting.empty()) {
                    CHECK_EQ(r.at("promoted").at("student_id").get<std::string>(), waiting.front());
                    confirmed.push_back(waiting.front());
                    waiting.pop_front();
                } else CHECK(r.at("promoted").is_null());
            }
        }
        if (step % 50 == 0) {  // full state comparison now and then
            json list = fx.svc.attendees(e);
            std::vector<std::string> gotConfirmed, gotWaiting;
            for (const json& a : list) (a.at("status") == "confirmed" ? gotConfirmed : gotWaiting).push_back(a.at("student_id").get<std::string>());
            std::sort(gotConfirmed.begin(), gotConfirmed.end());
            std::vector<std::string> want = confirmed;
            std::sort(want.begin(), want.end());
            CHECK(gotConfirmed == want);
            CHECK(gotWaiting == std::vector<std::string>(waiting.begin(), waiting.end()));  // same order = FIFO
        }
    }
}

TEST(seed_data_respects_every_rule) {
    Fx fx;
    SeedSummary s = seedDemoData(fx.svc);
    CHECK(s.eventsCreated > 30);
    CHECK(s.eventsRefusedClash > 0);              // the clash rule was actually exercised
    CHECK(s.registrationsWaiting > 0);            // waiting lists exist
    CHECK(s.registrationsRefusedStudentClash > 0);
    CHECK(s.promotions > 0);
    // 1. no two active events overlap in one venue
    CHECK_EQ(fx.db.scalar("SELECT COUNT(*) FROM events a JOIN events b ON a.venue_id = b.venue_id AND a.id < b.id "
                          "AND a.status='active' AND b.status='active' AND a.start_ts < b.end_ts AND b.start_ts < a.end_ts"), int64_t(0));
    // 2. never more confirmed students than seats
    CHECK_EQ(fx.db.scalar("SELECT COUNT(*) FROM (SELECT e.id FROM events e JOIN registrations r ON r.event_id = e.id "
                          "AND r.status='confirmed' GROUP BY e.id HAVING COUNT(*) > e.capacity)"), int64_t(0));
    // 3. somebody waits only when the event is full
    CHECK_EQ(fx.db.scalar("SELECT COUNT(*) FROM events e WHERE EXISTS (SELECT 1 FROM registrations r WHERE r.event_id=e.id AND r.status='waiting') "
                          "AND (SELECT COUNT(*) FROM registrations r WHERE r.event_id=e.id AND r.status='confirmed') < e.capacity"), int64_t(0));
    // 4. no student is signed up for two overlapping events
    CHECK_EQ(fx.db.scalar("SELECT COUNT(*) FROM registrations r1 JOIN events e1 ON e1.id=r1.event_id "
                          "JOIN registrations r2 ON r2.student_id=r1.student_id AND r2.id > r1.id JOIN events e2 ON e2.id=r2.event_id "
                          "WHERE e1.start_ts < e2.end_ts AND e2.start_ts < e1.end_ts"), int64_t(0));
    // 5. only confirmed students are checked in
    CHECK_EQ(fx.db.scalar("SELECT COUNT(*) FROM registrations WHERE checked_in=1 AND status<>'confirmed'"), int64_t(0));
    // 6. generation is deterministic
    Fx other;
    SeedSummary t = seedDemoData(other.svc);
    CHECK_EQ(s.eventsCreated, t.eventsCreated);
    CHECK_EQ(s.registrationsConfirmed, t.registrationsConfirmed);
    CHECK_EQ(s.registrationsWaiting, t.registrationsWaiting);
}

// ================================================================== REST API over real HTTP
struct TestServer {
    httplib::Server svr;
    std::thread th;
    int port = 0;
    TestServer(EventService& service, const std::string& key) {
        registerApi(svr, service, "", key);
        port = svr.bind_to_any_port("127.0.0.1");
        th = std::thread([this] { svr.listen_after_bind(); });
        svr.wait_until_ready();
    }
    ~TestServer() {
        svr.stop();
        if (th.joinable()) th.join();
    }
    httplib::Client client() const {
        httplib::Client c("127.0.0.1", port);
        c.set_connection_timeout(5, 0);
        c.set_read_timeout(10, 0);
        return c;
    }
};
static const char* JSON_CT = "application/json";
static const httplib::Headers ADMIN = {{"X-Admin-Key", "secret-key"}};
static json body(const httplib::Result& r) { return json::parse(r->body, nullptr, false); }

TEST(api_basics_and_errors) {
    Fx fx;
    TestServer srv(fx.svc, "secret-key");
    auto c = srv.client();
    auto h = c.Get("/api/health");
    CHECK(h && h->status == 200);
    CHECK_EQ(body(h).at("status").get<std::string>(), std::string("ok"));
    CHECK_EQ(h->get_header_value("X-Content-Type-Options"), std::string("nosniff"));
    CHECK_EQ(h->get_header_value("X-Frame-Options"), std::string("DENY"));
    CHECK(h->get_header_value("Content-Security-Policy").find("script-src 'self'") != std::string::npos);
    CHECK(h->get_header_value("Content-Security-Policy").find("unsafe-inline") == std::string::npos);
    auto nf = c.Get("/api/nope");
    CHECK(nf && nf->status == 404);
    CHECK_EQ(body(nf).at("code").get<std::string>(), std::string("not_found"));  // errors are JSON too
    auto bad = c.Get("/api/events/abc");
    CHECK(bad && bad->status == 404);
    auto huge = c.Get("/api/events/99999999999999999999");
    CHECK(huge && huge->status == 404);
    auto missing = c.Get("/api/events/12345");
    CHECK(missing && missing->status == 404);
    auto q = c.Get("/api/events?limit=abc");
    CHECK(q && q->status == 400);
    q = c.Get("/api/events?sort=title;drop");
    CHECK(q && q->status == 400);
    q = c.Get("/api/events?venue_id=-1");
    CHECK(q && q->status == 400);
    auto e = c.Get("/api/events");
    CHECK(e && e->status == 200);
    CHECK_EQ(body(e).at("total").get<int>(), 0);
}

TEST(api_admin_protection_and_content_type) {
    Fx fx;
    TestServer srv(fx.svc, "secret-key");
    auto c = srv.client();
    const std::string v = R"({"name":"Hall","capacity":50})";
    auto r = c.Post("/api/venues", v, JSON_CT);
    CHECK(r && r->status == 401);                                             // no key
    r = c.Post("/api/venues", httplib::Headers{{"X-Admin-Key", "wrong"}}, v, JSON_CT);
    CHECK(r && r->status == 401);                                             // wrong key
    r = c.Post("/api/venues", httplib::Headers{{"X-Admin-Key", "secret-ke"}}, v, JSON_CT);
    CHECK(r && r->status == 401);                                             // prefix of the key is not enough
    r = c.Post("/api/venues", ADMIN, v, "text/plain");
    CHECK(r && r->status == 415);                                             // cross-site form posts are refused
    r = c.Post("/api/venues", ADMIN, v, "application/x-www-form-urlencoded");
    CHECK(r && r->status == 415);
    r = c.Post("/api/venues", ADMIN, v, JSON_CT);
    CHECK(r && r->status == 201);
    CHECK_EQ(fx.svc.listVenues().size(), size_t(1));
    // all admin-only reads
    CHECK(c.Get("/api/events/1/attendees")->status == 401);
    CHECK(c.Get("/api/events/1/attendees.csv")->status == 401);
    CHECK(c.Get("/api/auth/check")->status == 401);
    CHECK(c.Get("/api/auth/check", ADMIN)->status == 200);
    CHECK(c.Post("/api/events/1/cancel", "{}", JSON_CT)->status == 401);
    CHECK(c.Post("/api/events/1/checkin", R"({"student_id":"AAA"})", JSON_CT)->status == 401);
    // a server started without any key rejects every admin call
    Fx fx2;
    TestServer open(fx2.svc, "");
    auto c2 = open.client();
    CHECK(c2.Post("/api/venues", httplib::Headers{{"X-Admin-Key", ""}}, v, JSON_CT)->status == 401);
}

TEST(api_body_validation) {
    Fx fx;
    TestServer srv(fx.svc, "secret-key");
    auto c = srv.client();
    auto post = [&](const std::string& path, const std::string& b) { return c.Post(path, ADMIN, b, JSON_CT); };
    CHECK(post("/api/venues", "{not json")->status == 400);
    CHECK(post("/api/venues", "[1,2,3]")->status == 400);
    CHECK(post("/api/venues", "\"text\"")->status == 400);
    CHECK(post("/api/venues", R"({"capacity":10})")->status == 400);                    // name missing
    CHECK(post("/api/venues", R"({"name":5,"capacity":10})")->status == 400);           // wrong type
    CHECK(post("/api/venues", R"({"name":"A","capacity":"10"})")->status == 400);       // number as string
    CHECK(post("/api/venues", R"({"name":"A","capacity":10.5})")->status == 400);       // not a whole number
    CHECK(post("/api/venues", R"({"name":"A","capacity":true})")->status == 400);
    CHECK(post("/api/venues", R"({"name":"A","capacity":99999999999})")->status == 400); // out of int range
    CHECK(post("/api/venues", R"({"name":"A","capacity":-3})")->status == 400);
    CHECK(post("/api/venues", R"({"name":"A","capacity":10,"location":7})")->status == 400);
    CHECK_EQ(fx.svc.listVenues().size(), size_t(0));  // nothing was created by any bad request
    // payload limit
    auto big = c.Post("/api/venues", ADMIN, std::string(70 * 1024, 'x'), JSON_CT);
    CHECK(big && big->status == 413);
}

TEST(api_full_workflow) {
    Fx fx;
    TestServer srv(fx.svc, "secret-key");
    auto c = srv.client();
    auto pv = c.Post("/api/venues", ADMIN, R"({"name":"Seminar Hall","capacity":60,"location":"Block B"})", JSON_CT);
    CHECK(pv && pv->status == 201);
    const int64_t vid = body(pv).at("id").get<int64_t>();
    const std::string ev = R"({"title":"Intro","category":"Tech","venue_id":)" + std::to_string(vid) +
                           R"(,"start":"2026-11-02T10:00","end":"2026-11-02T12:00","capacity":2,"description":"d"})";
    auto pe = c.Post("/api/events", ADMIN, ev, JSON_CT);
    CHECK(pe && pe->status == 201);
    const std::string eid = std::to_string(body(pe).at("id").get<int64_t>());
    // clash -> 409 with the clashing event named
    auto clash = c.Post("/api/events", ADMIN, R"({"title":"Other","venue_id":)" + std::to_string(vid) +
                        R"(,"start":"2026-11-02 11:00","end":"2026-11-02 13:00","capacity":10})", JSON_CT);
    CHECK(clash && clash->status == 409);
    CHECK_EQ(body(clash).at("code").get<std::string>(), std::string("venue_clash"));
    CHECK_EQ(body(clash).at("details").at("clashes_with").at("title").get<std::string>(), std::string("Intro"));
    // too small venue -> 400
    auto big = c.Post("/api/events", ADMIN, R"({"title":"Big","venue_id":)" + std::to_string(vid) +
                      R"(,"start":"2026-11-03 10:00","end":"2026-11-03 12:00","capacity":61})", JSON_CT);
    CHECK(big && big->status == 400);
    // registration: two seats then a waiting list
    auto r1 = c.Post("/api/events/" + eid + "/register", R"({"student_id":"s1001","name":"Asha"})", JSON_CT);
    auto r2 = c.Post("/api/events/" + eid + "/register", R"({"student_id":"S1002","name":"Bela"})", JSON_CT);
    auto r3 = c.Post("/api/events/" + eid + "/register", R"({"student_id":"S1003","name":"Chitra"})", JSON_CT);
    CHECK(r1 && r1->status == 201 && r2 && r2->status == 201 && r3 && r3->status == 201);
    CHECK_EQ(body(r1).at("status").get<std::string>(), std::string("confirmed"));
    CHECK_EQ(body(r3).at("status").get<std::string>(), std::string("waiting"));
    CHECK_EQ(body(r3).at("waiting_position").get<int>(), 1);
    auto dup = c.Post("/api/events/" + eid + "/register", R"({"student_id":"S1001","name":"Asha"})", JSON_CT);
    CHECK(dup && dup->status == 409);
    auto badId = c.Post("/api/events/" + eid + "/register", R"({"student_id":"a b","name":"X"})", JSON_CT);
    CHECK(badId && badId->status == 400);
    // the student's own view
    auto mine = c.Get("/api/students/S1003/registrations");
    CHECK(mine && mine->status == 200 && body(mine).size() == 1);
    CHECK_EQ(body(mine)[0].at("waiting_position").get<int>(), 1);
    // cancel a confirmed seat -> the waiting student is promoted
    auto del = c.Delete("/api/events/" + eid + "/registrations/S1001");
    CHECK(del && del->status == 200);
    CHECK_EQ(body(del).at("promoted").at("student_id").get<std::string>(), std::string("S1003"));
    auto del2 = c.Delete("/api/events/" + eid + "/registrations/S1001");
    CHECK(del2 && del2->status == 404);
    // admin: attendees, check-in, CSV
    CHECK(c.Get("/api/events/" + eid + "/attendees", ADMIN)->status == 200);
    auto ci = c.Post("/api/events/" + eid + "/checkin", ADMIN, R"({"student_id":"S1002"})", JSON_CT);
    CHECK(ci && ci->status == 200);
    auto csv = c.Get("/api/events/" + eid + "/attendees.csv", ADMIN);
    CHECK(csv && csv->status == 200);
    CHECK(csv->get_header_value("Content-Type").find("text/csv") != std::string::npos);
    CHECK(csv->get_header_value("Content-Disposition").find("attachment") != std::string::npos);
    CHECK(csv->body.find("S1002,Bela,confirmed,,yes") != std::string::npos);
    // free slots + search + reports
    auto fs = c.Get("/api/venues/" + std::to_string(vid) + "/free-slots?date=2026-11-02&min_minutes=60");
    CHECK(fs && fs->status == 200);
    CHECK_EQ(body(fs).at("free_slots").size(), size_t(2));  // 08:00-10:00 and 12:00-20:00
    CHECK(c.Get("/api/venues/" + std::to_string(vid) + "/free-slots")->status == 400);  // date is required
    CHECK(c.Get("/api/venues/" + std::to_string(vid) + "/free-slots?date=2026-11-02&day_start=20:00&day_end=08:00")->status == 400);
    CHECK(c.Get("/api/venues/99/free-slots?date=2026-11-02")->status == 404);
    auto se = c.Get("/api/events?q=intro&category=tech&from=2026-11-02&to=2026-11-02");
    CHECK(se && se->status == 200 && body(se).at("total").get<int>() == 1);
    for (const char* rep : {"attendance", "popularity", "venues", "waitlist"}) {
        auto rr = c.Get(std::string("/api/reports/") + rep);
        CHECK(rr && rr->status == 200 && body(rr).is_array());
    }
    CHECK(c.Get("/api/reports/popularity?limit=0")->status == 400);
    CHECK_EQ(body(c.Get("/api/stats")).at("events").get<int>(), 1);
    // cancel the event -> registrations refused
    auto ce = c.Post("/api/events/" + eid + "/cancel", ADMIN, "{}", JSON_CT);
    CHECK(ce && ce->status == 200);
    auto after = c.Post("/api/events/" + eid + "/register", R"({"student_id":"S9999","name":"Late"})", JSON_CT);
    CHECK(after && after->status == 409);
}

TEST(api_concurrent_registration) {
    Fx fx;
    TestServer srv(fx.svc, "secret-key");
    const int64_t e = fx.event("Rush", fx.venue(), "2026-11-02 10:00", "2026-11-02 11:00", 6);
    std::atomic<int> confirmed{0}, waiting{0}, other{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 20; ++i) {
        threads.emplace_back([&, i] {
            auto c = srv.client();
            auto r = c.Post("/api/events/" + std::to_string(e) + "/register",
                            R"({"student_id":"HTTP)" + std::to_string(1000 + i) + R"(","name":"N"})", JSON_CT);
            if (!r || r->status != 201) {
                std::printf("    request %d failed: %s\n", i, r ? ("status " + std::to_string(r->status) + " " + r->body).c_str() : httplib::to_string(r.error()).c_str());
                other++;
                return;
            }
            (json::parse(r->body).at("status") == "confirmed" ? confirmed : waiting)++;
        });
    }
    for (auto& t : threads) t.join();
    CHECK_EQ(other.load(), 0);
    CHECK_EQ(confirmed.load(), 6);
    CHECK_EQ(waiting.load(), 14);
}

TEST(secure_equals_behaviour) {
    CHECK(secureEquals("abc", "abc"));
    CHECK(!secureEquals("abc", "abd"));
    CHECK(!secureEquals("abc", "abcd"));
    CHECK(!secureEquals("", "x"));
    CHECK(secureEquals("", ""));
}

// ------------------------------------------------------------------ runner
int main(int argc, char** argv) {
    const std::string filter = argc > 1 ? argv[1] : "";
    int ran = 0;
    for (const TestCase& t : registry()) {
        if (!filter.empty() && std::string(t.name).find(filter) == std::string::npos) continue;
        const int before = g_failures;
        try {
            t.fn();
        } catch (const std::exception& e) {
            ++g_failures;
            std::printf("    EXCEPTION: %s\n", e.what());
        }
        ++ran;
        const bool ok = g_failures == before;
        std::printf("[%s] %s\n", ok ? " ok " : "FAIL", t.name);
    }
    std::printf("\n%d tests, %d checks, %d failed check(s)\n", ran, g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
