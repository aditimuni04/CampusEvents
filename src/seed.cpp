#include "campusevents/seed.hpp"

#include <algorithm>
#include <cstdio>
#include <random>
#include <vector>

namespace ce {

namespace {

struct VenueSpec { const char* name; int capacity; const char* location; };
const VenueSpec kVenues[] = {
    {"Main Auditorium", 500, "Block A, ground floor"},
    {"Seminar Hall", 120, "Block B, first floor"},
    {"Computer Lab 1", 40, "Block C, second floor"},
    {"Open Air Theatre", 800, "Campus grounds"},
    {"Conference Room", 30, "Administration building"},
    {"Sports Complex", 300, "Sports block"},
};

struct EventSpec { const char* title; const char* category; const char* description; };
const EventSpec kEvents[] = {
    {"Intro to Competitive Programming", "Tech", "Problem solving with arrays, strings and greedy ideas."},
    {"Git and GitHub Workshop", "Workshop", "Branches, pull requests and open-source etiquette."},
    {"Resume Building Clinic", "Career", "Hands-on resume review with placement mentors."},
    {"Mock Interview Day", "Career", "One-to-one technical and HR practice rounds."},
    {"Guided Meditation Session", "Wellness", "A calm 45-minute session for exam stress."},
    {"Yoga and Breathwork", "Wellness", "Beginner-friendly breathing and posture practice."},
    {"Inter-College Quiz", "Academic", "Teams of three; general knowledge and science."},
    {"Robotics Demo Day", "Tech", "Student robots, line followers and drones."},
    {"Open Mic Night", "Cultural", "Music, poetry and stand-up from campus talent."},
    {"Dance Showcase", "Cultural", "Classical and contemporary performances."},
    {"Cricket Friendly", "Sports", "Department vs department friendly match."},
    {"Badminton Tournament", "Sports", "Singles and doubles knockout."},
    {"Cloud Computing Talk", "Tech", "Industry speaker on cloud careers."},
    {"Cybersecurity Basics", "Workshop", "Passwords, phishing and safe habits."},
    {"Entrepreneurship Panel", "Career", "Founders share what worked and what failed."},
    {"Photography Walk", "Cultural", "Campus photo walk with a pro photographer."},
    {"Placement Preparation Talk", "Career", "Aptitude, coding rounds and group discussions."},
    {"Embedded Systems Workshop", "Workshop", "Microcontrollers and sensors, build and test."},
    {"Science Exhibition", "Academic", "Student projects on display."},
    {"Debate Championship", "Academic", "Moot-style debate on technology and society."},
    {"Hackathon Kick-off", "Tech", "Problem statements and team formation."},
    {"Alumni Meet", "Career", "Alumni answer questions about life after college."},
    {"Wellness Fest Planning Meet", "Wellness", "Volunteer briefing for the wellness fest."},
    {"Python for Data Workshop", "Workshop", "Analysing a small dataset with Python."},
};

const char* kFirst[] = {"Aarav", "Aditi", "Aisha", "Akash", "Ananya", "Arjun", "Diya", "Farhan", "Gauri", "Harsh",
                        "Isha", "Kabir", "Kavya", "Meera", "Neel", "Nisha", "Omkar", "Pooja", "Rahul", "Riya",
                        "Rohan", "Sana", "Shreya", "Siddharth", "Sneha", "Tanvi", "Uday", "Vedant", "Yash", "Zoya"};
const char* kLast[] = {"Patil", "Sharma", "Deshmukh", "Khan", "Iyer", "Naik", "Joshi", "Mehta", "Shah", "Kulkarni",
                       "Gupta", "Nair", "Rao", "Singh", "Verma"};

}  // namespace

SeedSummary seedDemoData(EventService& svc, unsigned randomSeed) {
    SeedSummary out;
    std::mt19937 rng(randomSeed);
    auto uni = [&rng](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
    auto chance = [&rng](double p) { return std::uniform_real_distribution<double>(0.0, 1.0)(rng) < p; };

    // --- venues
    std::vector<int64_t> venueIds;
    std::vector<int> venueCaps;
    for (const VenueSpec& v : kVenues) {
        json created = svc.createVenue({v.name, v.capacity, v.location});
        venueIds.push_back(created.at("id").get<int64_t>());
        venueCaps.push_back(v.capacity);
        ++out.venues;
    }

    // --- students (400 synthetic people; IDs like PCE26-0001)
    struct Student { std::string id, name; };
    std::vector<Student> students;
    for (int i = 1; i <= 400; ++i) {
        char id[24];
        std::snprintf(id, sizeof id, "PCE26-%04d", i);
        students.push_back({id, std::string(kFirst[uni(0, 29)]) + " " + kLast[uni(0, 14)]});
    }

    // --- events: 12 Oct .. 15 Nov 2026, Monday-Saturday, up to two attempts per venue per day
    int64_t firstDay = 0;
    parseDate("2026-10-12", firstDay);
    const int hours[] = {9, 11, 13, 15, 17};
    struct Created { int64_t id; int capacity; int64_t end; };
    std::vector<Created> created;
    int specIndex = 0;
    const int64_t today = firstDay + 16 * 86400;  // "now" for the demo is 28 Oct 2026: earlier events count as past
    for (int d = 0; d < 35; ++d) {
        const int64_t day = firstDay + d * 86400LL;
        if (((day / 86400) + 4) % 7 == 0) continue;  // skip Sundays (1970-01-01 was a Thursday)
        for (size_t v = 0; v < venueIds.size(); ++v) {
            const int attempts = chance(0.30) ? (chance(0.15) ? 3 : 2) : 0;
            for (int a = 0; a < attempts; ++a) {
                const EventSpec& spec = kEvents[specIndex++ % (sizeof kEvents / sizeof kEvents[0])];
                const int startHour = hours[uni(0, 4)];
                const int durationHalfHours = uni(2, 6);  // 1 to 3 hours
                const int64_t start = day + startHour * 3600LL;
                const int64_t end = start + durationHalfHours * 1800LL;
                static const int kCaps[] = {20, 30, 40, 60, 80, 120, 200, 300};
                int cap = std::min(kCaps[uni(0, 7)], venueCaps[v]);
                NewEvent ne{spec.title, spec.category, spec.description, venueIds[v], formatDateTime(start),
                            formatDateTime(end), cap};
                try {
                    json e = svc.createEvent(ne);
                    created.push_back({e.at("id").get<int64_t>(), cap, end});
                    ++out.eventsCreated;
                } catch (const AppError& err) {
                    if (err.code() == "venue_clash") ++out.eventsRefusedClash;
                    else throw;
                }
            }
        }
    }

    // --- registrations: a mix of under-subscribed and over-subscribed events
    for (const Created& c : created) {
        const double factor = 0.25 + (uni(0, 115) / 100.0);  // 25% .. 140% of capacity
        const int demand = std::min<int>(static_cast<int>(c.capacity * factor), static_cast<int>(students.size()));
        std::vector<int> order(students.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
        std::shuffle(order.begin(), order.end(), rng);
        for (int k = 0; k < demand; ++k) {
            const Student& s = students[static_cast<size_t>(order[static_cast<size_t>(k)])];
            try {
                json r = svc.registerStudent(c.id, s.id, s.name);
                if (r.at("status") == "confirmed") ++out.registrationsConfirmed;
                else ++out.registrationsWaiting;
            } catch (const AppError& err) {
                if (err.code() == "student_clash") ++out.registrationsRefusedStudentClash;
                else throw;
            }
        }
    }

    // --- check-ins for events that are already over (about 85% of confirmed students show up)
    for (const Created& c : created) {
        if (c.end > today) continue;
        for (const json& a : svc.attendees(c.id)) {
            if (a.at("status") == "confirmed" && chance(0.85)) {
                svc.checkIn(c.id, a.at("student_id").get<std::string>());
                ++out.checkIns;
            }
        }
    }

    // --- a few cancellations on full upcoming events, to show waiting-list promotion
    int budget = 12;
    for (const Created& c : created) {
        if (budget == 0) break;
        if (c.end <= today) continue;
        json att = svc.attendees(c.id);
        bool hasWaiting = false;
        std::string firstConfirmed;
        for (const json& a : att) {
            if (a.at("status") == "waiting") hasWaiting = true;
            else if (firstConfirmed.empty()) firstConfirmed = a.at("student_id").get<std::string>();
        }
        if (hasWaiting && !firstConfirmed.empty()) {
            json r = svc.cancelRegistration(c.id, firstConfirmed);
            if (!r.at("promoted").is_null()) ++out.promotions;
            --budget;
        }
    }
    return out;
}

}  // namespace ce
