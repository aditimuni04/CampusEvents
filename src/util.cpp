#include "campusevents/util.hpp"

#include <algorithm>
#include <cstdio>

namespace ce {

bool overlaps(const Interval& a, const Interval& b) { return a.start < b.end && b.start < a.end; }

std::vector<Interval> mergeIntervals(std::vector<Interval> v) {
    v.erase(std::remove_if(v.begin(), v.end(), [](const Interval& i) { return i.end <= i.start; }), v.end());
    std::sort(v.begin(), v.end(), [](const Interval& a, const Interval& b) {
        return a.start != b.start ? a.start < b.start : a.end < b.end;
    });
    std::vector<Interval> out;
    for (const Interval& iv : v) {
        if (!out.empty() && iv.start <= out.back().end) {
            out.back().end = std::max(out.back().end, iv.end);  // overlapping or touching: extend
        } else {
            out.push_back(iv);
        }
    }
    return out;
}

std::vector<Interval> freeGaps(const std::vector<Interval>& busy, Interval window, int64_t minLength) {
    std::vector<Interval> clipped;
    for (Interval iv : busy) {
        iv.start = std::max(iv.start, window.start);
        iv.end = std::min(iv.end, window.end);
        if (iv.start < iv.end) clipped.push_back(iv);
    }
    std::vector<Interval> gaps;
    int64_t cursor = window.start;
    auto addGap = [&](int64_t s, int64_t e) {
        if (e > s && e - s >= minLength) gaps.push_back({s, e});
    };
    for (const Interval& m : mergeIntervals(std::move(clipped))) {
        if (m.start > cursor) addGap(cursor, m.start);
        cursor = std::max(cursor, m.end);
    }
    if (window.end > cursor) addGap(cursor, window.end);
    return gaps;
}

// ------------------------------------------------------------------ calendar maths
// Howard Hinnant's public-domain civil-calendar algorithms (days since 1970-01-01).

namespace {

int64_t daysFromCivil(int64_t y, int64_t m, int64_t d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void civilFromDays(int64_t z, int64_t& y, int64_t& m, int64_t& d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = yoe + era * 400;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
}

bool isLeap(int64_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

int daysInMonth(int64_t y, int64_t m) {
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && isLeap(y) ? 29 : kDays[m - 1];
}

// Reads exactly `n` ASCII digits starting at `pos`.
bool digits(const std::string& s, size_t pos, size_t n, int64_t& out) {
    if (pos + n > s.size()) return false;
    int64_t v = 0;
    for (size_t i = 0; i < n; ++i) {
        const char c = s[pos + i];
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    out = v;
    return true;
}

}  // namespace

bool parseDate(const std::string& text, int64_t& dayStart) {
    int64_t y = 0, m = 0, d = 0;
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') return false;
    if (!digits(text, 0, 4, y) || !digits(text, 5, 2, m) || !digits(text, 8, 2, d)) return false;
    if (y < 2000 || y > 2100 || m < 1 || m > 12 || d < 1 || d > daysInMonth(y, m)) return false;
    dayStart = daysFromCivil(y, m, d) * 86400;
    return true;
}

bool parseTimeOfDay(const std::string& text, int& minutes) {
    int64_t h = 0, mi = 0;
    if (text.size() != 5 || text[2] != ':') return false;
    if (!digits(text, 0, 2, h) || !digits(text, 3, 2, mi)) return false;
    if (mi > 59 || h > 24 || (h == 24 && mi != 0)) return false;
    minutes = static_cast<int>(h * 60 + mi);
    return true;
}

bool parseDateTime(const std::string& text, int64_t& out) {
    if (text.size() != 16 || (text[10] != ' ' && text[10] != 'T')) return false;
    int64_t day = 0;
    int minutes = 0;
    if (!parseDate(text.substr(0, 10), day) || !parseTimeOfDay(text.substr(11), minutes)) return false;
    if (minutes >= 24 * 60) return false;  // "24:00" is only meaningful as a day end, not as an event time
    out = day + static_cast<int64_t>(minutes) * 60;
    return true;
}

std::string formatDate(int64_t ts) {
    int64_t days = ts >= 0 ? ts / 86400 : -((-ts + 86399) / 86400);
    int64_t y = 0, m = 0, d = 0;
    civilFromDays(days, y, m, d);
    char buf[96];
    std::snprintf(buf, sizeof buf, "%04lld-%02lld-%02lld", static_cast<long long>(y), static_cast<long long>(m),
                  static_cast<long long>(d));
    return buf;
}

std::string formatDateTime(int64_t ts) {
    int64_t days = ts >= 0 ? ts / 86400 : -((-ts + 86399) / 86400);
    const int64_t secs = ts - days * 86400;
    char buf[64];
    std::snprintf(buf, sizeof buf, " %02lld:%02lld", static_cast<long long>(secs / 3600),
                  static_cast<long long>((secs % 3600) / 60));
    return formatDate(ts) + buf;
}

// ------------------------------------------------------------------ text

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

std::string toLower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::string toUpper(std::string s) {
    for (char& c : s) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return s;
}

bool hasControlChars(const std::string& s) {
    for (unsigned char c : s) if (c < 0x20 || c == 0x7f) return true;
    return false;
}

std::string csvField(const std::string& raw) {
    std::string s = raw;
    if (!s.empty() && (s[0] == '=' || s[0] == '+' || s[0] == '-' || s[0] == '@' || s[0] == '\t' || s[0] == '\r')) {
        s.insert(s.begin(), '\'');  // stop Excel / Sheets from evaluating the cell as a formula
    }
    if (s.find_first_of(",\"\n\r") == std::string::npos) return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += '"';
        out += c;
    }
    out += '"';
    return out;
}

}  // namespace ce
