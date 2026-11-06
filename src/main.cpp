// CampusEvents - campus event management: venues, events, registration, waiting list, reports.

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "campusevents/api.hpp"
#include "campusevents/seed.hpp"

#ifndef CE_VERSION
#define CE_VERSION "1.0.0"
#endif

using namespace ce;

namespace {

void usage() {
    std::printf(
        "CampusEvents %s - event management with clash detection and waiting lists\n"
        "\n"
        "Usage: campusevents <command> [options]\n"
        "\n"
        "Commands\n"
        "  serve    Start the web page and REST API\n"
        "             [--db FILE (campusevents.db)]  [--port N (8080)]  [--host H (127.0.0.1)]\n"
        "             [--web DIR]  [--demo]   (--demo loads sample data when the database is empty)\n"
        "  seed     Fill an empty database with sample data      [--db FILE]\n"
        "  version | help\n"
        "\n"
        "Admin key: set the CAMPUS_ADMIN_KEY environment variable. If it is not set, a random key\n"
        "is generated and printed when the server starts.\n",
        CE_VERSION);
}

struct Args {
    std::string command;
    std::map<std::string, std::string> opts;
    std::set<std::string> flags;
    std::string bad;  // first problem found while parsing, "" if none
    std::string get(const std::string& k, const std::string& def = "") const {
        auto it = opts.find(k);
        return it == opts.end() ? def : it->second;
    }
};

Args parseArgs(int argc, char** argv) {
    Args a;
    if (argc > 1) a.command = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string s = argv[i];
        if (s == "--demo") {
            a.flags.insert("demo");
        } else if (s.rfind("--", 0) == 0 && s.size() > 2) {
            if (i + 1 >= argc) {
                a.bad = "option " + s + " needs a value";
                break;
            }
            a.opts[s.substr(2)] = argv[++i];
        } else {
            a.bad = "unexpected argument '" + s + "'";
            break;
        }
    }
    return a;
}

bool parsePort(const std::string& text, int& port) {
    if (text.empty() || text.size() > 5) return false;
    int v = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    if (v < 1 || v > 65535) return false;
    port = v;
    return true;
}

std::string randomKey() {
    std::random_device rd;
    static const char* hex = "0123456789abcdef";
    std::string key;
    for (int i = 0; i < 16; ++i) {
        const unsigned v = rd() & 0xff;
        key += hex[v >> 4];
        key += hex[v & 15];
    }
    return key;
}

void printSeed(const SeedSummary& s) {
    std::printf("Sample data loaded: %d venues, %d events (%d bookings refused for clashing), %d confirmed + %d waiting "
                "registrations (%d refused: student already busy), %d check-ins, %d waiting-list promotions.\n",
                s.venues, s.eventsCreated, s.eventsRefusedClash, s.registrationsConfirmed, s.registrationsWaiting,
                s.registrationsRefusedStudentClash, s.checkIns, s.promotions);
}

void onSignal(int) { requestStop(); }

double msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// Creates many events (plus deliberate clashes), registrations, searches and free-slot lookups on a
// temporary file database and prints how long each phase took.
int bench(const Args& args) {
    const int events = std::atoi(args.get("events", "20000").c_str());
    const int regs = std::atoi(args.get("registrations", "100000").c_str());
    if (events < 100 || events > 2000000 || regs < 1 || regs > 20000000) {
        std::fprintf(stderr, "error: --events must be 100..2000000 and --registrations 1..20000000\n");
        return 2;
    }
    const std::string path = "campusevents_bench.db";
    for (const char* suffix : {"", "-wal", "-shm"}) std::remove((path + suffix).c_str());
    {
        Database db(path);
        db.initSchema();
        EventService svc(db);
        std::mt19937 rng(1);
        const int venues = 50, students = 5000;
        std::vector<int64_t> venueIds;
        for (int v = 0; v < venues; ++v) venueIds.push_back(svc.createVenue({"Venue " + std::to_string(v), 500, ""}).at("id").get<int64_t>());
        int64_t base = 0;
        parseDate("2027-01-04", base);

        auto t0 = std::chrono::steady_clock::now();
        std::vector<int64_t> ids;
        int clashes = 0;
        for (int i = 0; i < events; ++i) {
            const int64_t start = base + (i / venues) * 2 * 3600LL;  // one event per venue every 2 hours
            NewEvent e{"Event " + std::to_string(i), "Tech", "", venueIds[static_cast<size_t>(i % venues)], formatDateTime(start), formatDateTime(start + 3600), 100};
            ids.push_back(svc.createEvent(e).at("id").get<int64_t>());
            if (i % 10 == 0) {  // every 10th creation, also try a booking that overlaps and must be refused
                NewEvent c = e;
                c.start = formatDateTime(start + 1800);
                c.end = formatDateTime(start + 5400);
                try { svc.createEvent(c); } catch (const AppError& err) { if (err.code() == "venue_clash") ++clashes; }
            }
        }
        const double createMs = msSince(t0);
        std::printf("create events      : %d created + %d clash checks refused in %.0f ms (%.0f bookings/s incl. checks)\n", events, clashes,
                    createMs, (events + clashes) * 1000.0 / createMs);

        t0 = std::chrono::steady_clock::now();
        int confirmed = 0, waiting = 0, refused = 0;
        for (int r = 0; r < regs; ++r) {
            const int64_t ev = ids[rng() % ids.size()];
            try {
                json out = svc.registerStudent(ev, "S" + std::to_string(10000 + static_cast<int>(rng() % students)), "Student");
                (out.at("status") == "confirmed" ? confirmed : waiting)++;
            } catch (const AppError&) { ++refused; }
        }
        const double regMs = msSince(t0);
        std::printf("register students  : %d attempts (%d confirmed, %d waiting, %d refused) in %.0f ms (%.0f/s)\n", regs, confirmed,
                    waiting, refused, regMs, regs * 1000.0 / regMs);

        t0 = std::chrono::steady_clock::now();
        const int searches = 2000;
        for (int i = 0; i < searches; ++i) {
            EventQuery q;
            q.text = "Event " + std::to_string(rng() % static_cast<unsigned>(events));
            q.limit = 20;
            svc.searchEvents(q);
        }
        std::printf("search events      : %d searches over %d events in %.0f ms (%.2f ms each)\n", searches, events, msSince(t0), msSince(t0) / searches);

        t0 = std::chrono::steady_clock::now();
        const int lookups = 2000;
        for (int i = 0; i < lookups; ++i) {
            svc.freeSlots(venueIds[rng() % venueIds.size()], formatDate(base + static_cast<int64_t>(rng() % 200) * 86400), "08:00", "20:00", 30);
        }
        std::printf("free-slot lookups  : %d lookups in %.0f ms (%.2f ms each)\n", lookups, msSince(t0), msSince(t0) / lookups);

        t0 = std::chrono::steady_clock::now();
        svc.venueUsageReport();
        svc.popularityReport(10);
        svc.attendanceReport(false);
        std::printf("3 reports          : %.0f ms for venue usage + popularity + attendance over %d events\n", msSince(t0), events);
    }
    for (const char* suffix : {"", "-wal", "-shm"}) std::remove((path + suffix).c_str());
    return 0;
}

int run(int argc, char** argv) {
    Args args = parseArgs(argc, argv);
    const std::string& cmd = args.command;
    if (cmd.empty() || cmd == "help" || cmd == "--help" || cmd == "-h") {
        usage();
        return cmd.empty() ? 2 : 0;
    }
    if (cmd == "version" || cmd == "--version") {
        std::printf("campusevents %s\n", CE_VERSION);
        return 0;
    }
    if (cmd == "bench") {
        if (!args.bad.empty()) { std::fprintf(stderr, "error: %s\n", args.bad.c_str()); return 2; }
        return bench(args);
    }
    if (cmd != "serve" && cmd != "seed") {
        std::fprintf(stderr, "error: unknown command '%s' (try 'campusevents help')\n", cmd.c_str());
        return 2;
    }
    if (!args.bad.empty()) {
        std::fprintf(stderr, "error: %s\n", args.bad.c_str());
        return 2;
    }

    Database db(args.get("db", "campusevents.db"));
    db.initSchema();
    EventService service(db);

    if (cmd == "seed") {
        if (service.stats().at("venues").get<int>() > 0) {
            std::fprintf(stderr, "error: this database already contains data. Use a new --db file for sample data.\n");
            return 1;
        }
        printSeed(seedDemoData(service));
        return 0;
    }

    // ---- serve
    ServerOptions opt;
    opt.host = args.get("host", "127.0.0.1");
    if (!parsePort(args.get("port", "8080"), opt.port)) {
        std::fprintf(stderr, "error: --port must be a number between 1 and 65535\n");
        return 2;
    }
    opt.webDir = findWebDir(args.get("web"), argv[0]);
    if (opt.webDir.empty()) {
        std::fprintf(stderr, "warning: web folder not found - serving the API only (use --web DIR)\n");
    }
    if (args.flags.count("demo") && service.stats().at("venues").get<int>() == 0) printSeed(seedDemoData(service));

    const char* envKey = std::getenv("CAMPUS_ADMIN_KEY");
    opt.adminKey = envKey ? envKey : "";
    if (opt.adminKey.empty()) {
        opt.adminKey = randomKey();
        std::printf("Admin key (generated for this run): %s\n", opt.adminKey.c_str());
    } else {
        std::printf("Admin key: taken from CAMPUS_ADMIN_KEY\n");
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    return runServer(service, opt);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
