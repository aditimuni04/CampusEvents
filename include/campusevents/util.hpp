#pragma once
// Small, dependency-free helpers: time intervals, date parsing/formatting, text hygiene.
#include <cstdint>
#include <string>
#include <vector>

namespace ce {

// Half-open time interval [start, end) in seconds since the Unix epoch.
// Half-open means an event ending at 12:00 does NOT clash with one starting at 12:00.
struct Interval {
    int64_t start = 0;
    int64_t end = 0;
};

// The classic overlap test: two intervals overlap iff each starts before the other ends.
bool overlaps(const Interval& a, const Interval& b);

// Sorts and merges overlapping or touching intervals (empty / inverted ones are dropped).
std::vector<Interval> mergeIntervals(std::vector<Interval> v);

// Free gaps inside `window` that are not covered by `busy` and are at least `minLength` seconds long.
std::vector<Interval> freeGaps(const std::vector<Interval>& busy, Interval window, int64_t minLength);

// Strict parsers. All times are "wall clock" times (the system has no time-zone logic) and are
// stored as seconds since 1970-01-01 00:00 of that same wall clock.
bool parseDateTime(const std::string& text, int64_t& out);   // "YYYY-MM-DD HH:MM" or "YYYY-MM-DDTHH:MM"
bool parseDate(const std::string& text, int64_t& dayStart);  // "YYYY-MM-DD" -> 00:00 of that day
bool parseTimeOfDay(const std::string& text, int& minutes);  // "HH:MM" (also "24:00") -> minutes after midnight

std::string formatDateTime(int64_t ts);  // "YYYY-MM-DD HH:MM"
std::string formatDate(int64_t ts);      // "YYYY-MM-DD"

std::string trim(const std::string& s);
std::string toLower(std::string s);
std::string toUpper(std::string s);
bool hasControlChars(const std::string& s);  // true for ASCII control characters (incl. newline, tab, DEL)

// CSV field escaping that also neutralises spreadsheet formula injection (=, +, -, @ at the start).
std::string csvField(const std::string& s);

}  // namespace ce
