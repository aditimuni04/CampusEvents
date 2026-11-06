#include "campusevents/api.hpp"

#include <httplib/httplib.h>

#include <atomic>
#include <chrono>
#include <algorithm>
#include <climits>
#include <cstdio>
#include <filesystem>
#include <thread>

namespace ce {

namespace {

std::atomic<bool> g_stop{false};

// ---------------------------------------------------------------- parsing helpers

int64_t parseInt(const std::string& text, const std::string& name, int64_t lo, int64_t hi) {
    if (text.empty() || text.size() > 15) throw AppError::invalid("'" + name + "' must be an integer");
    size_t i = 0;
    bool neg = false;
    if (text[0] == '-') {
        neg = true;
        i = 1;
        if (text.size() == 1) throw AppError::invalid("'" + name + "' must be an integer");
    }
    int64_t v = 0;
    for (; i < text.size(); ++i) {
        if (text[i] < '0' || text[i] > '9') throw AppError::invalid("'" + name + "' must be an integer");
        v = v * 10 + (text[i] - '0');
    }
    if (neg) v = -v;
    if (v < lo || v > hi) {
        throw AppError::invalid("'" + name + "' must be between " + std::to_string(lo) + " and " + std::to_string(hi));
    }
    return v;
}

int intParam(const httplib::Request& req, const char* name, int def, int lo, int hi) {
    if (!req.has_param(name)) return def;
    return static_cast<int>(parseInt(req.get_param_value(name), name, lo, hi));
}

std::string textParam(const httplib::Request& req, const char* name, const std::string& def = "") {
    return req.has_param(name) ? req.get_param_value(name) : def;
}

int64_t pathId(const httplib::Request& req, size_t group = 1) {
    return parseInt(req.matches[group], "id", 1, 999999999999999LL);
}

// ---------------------------------------------------------------- body helpers

json parseBody(const httplib::Request& req) {
    const std::string ct = toLower(req.get_header_value("Content-Type"));
    // Requiring JSON blocks cross-site HTML forms (they cannot send this content type without a CORS preflight).
    if (ct.rfind("application/json", 0) != 0) {
        throw AppError(415, "unsupported_media_type", "send the request body as application/json");
    }
    if (req.body.empty()) return json::object();
    json body = json::parse(req.body, nullptr, /*allow_exceptions=*/false);
    if (body.is_discarded() || !body.is_object()) throw AppError::invalid("the body must be a JSON object");
    return body;
}

void requireJsonContentType(const httplib::Request& req) { (void)parseBody(req); }

std::string bodyString(const json& b, const char* key, bool required) {
    auto it = b.find(key);
    if (it == b.end() || it->is_null()) {
        if (required) throw AppError::invalid(std::string("'") + key + "' is required");
        return "";
    }
    if (!it->is_string()) throw AppError::invalid(std::string("'") + key + "' must be a string");
    return it->get<std::string>();
}

int64_t bodyInt(const json& b, const char* key) {
    auto it = b.find(key);
    if (it == b.end() || it->is_null()) throw AppError::invalid(std::string("'") + key + "' is required");
    if (!it->is_number_integer()) throw AppError::invalid(std::string("'") + key + "' must be a whole number");
    return it->get<int64_t>();
}

int bodyIntRange(const json& b, const char* key) {
    const int64_t v = bodyInt(b, key);
    if (v < INT_MIN || v > INT_MAX) throw AppError::invalid(std::string("'") + key + "' is out of range");
    return static_cast<int>(v);
}

// ---------------------------------------------------------------- responses

void sendJson(httplib::Response& res, const json& body, int status = 200) {
    res.status = status;
    res.set_content(body.dump(), "application/json; charset=utf-8");
}

void sendError(httplib::Response& res, int status, const std::string& code, const std::string& message,
               const json& details = nullptr) {
    json body = {{"error", message}, {"code", code}};
    if (!details.is_null()) body["details"] = details;
    sendJson(res, body, status);
}

// Wraps a handler so every failure becomes a clean JSON error instead of a crash.
template <typename Fn>
httplib::Server::Handler wrap(Fn fn) {
    return [fn](const httplib::Request& req, httplib::Response& res) {
        try {
            fn(req, res);
        } catch (const AppError& e) {
            sendError(res, e.status(), e.code(), e.what(), e.details());
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[api] %s %s failed: %s\n", req.method.c_str(), req.path.c_str(), e.what());
            sendError(res, 500, "internal_error", "internal server error");
        }
    };
}

}  // namespace

bool secureEquals(const std::string& a, const std::string& b) {
    unsigned char diff = static_cast<unsigned char>(a.size() != b.size());
    const size_t n = std::max(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const unsigned char x = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
        const unsigned char y = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
        diff |= static_cast<unsigned char>(x ^ y);
    }
    return diff == 0;
}

// ---------------------------------------------------------------- routes

void registerApi(httplib::Server& svr, EventService& svc, const std::string& webDir, const std::string& adminKey) {
    // Admin endpoints need the key. An empty configured key means "nobody is admin" (fail closed).
    auto requireAdmin = [adminKey](const httplib::Request& req) {
        const std::string given = req.get_header_value("X-Admin-Key");
        if (adminKey.empty() || given.empty() || !secureEquals(given, adminKey)) {
            throw AppError::unauthorized("admin key missing or wrong (send it in the X-Admin-Key header)");
        }
    };
    const std::string kId = R"((\d{1,15}))";
    const std::string kSid = R"(([A-Za-z0-9_\-]{1,40}))";

    svr.Get("/api/health", wrap([](const httplib::Request&, httplib::Response& res) { sendJson(res, {{"status", "ok"}}); }));
    svr.Get("/api/auth/check", wrap([requireAdmin](const httplib::Request& req, httplib::Response& res) {
                requireAdmin(req);
                sendJson(res, {{"admin", true}});
            }));
    svr.Get("/api/stats", wrap([&svc](const httplib::Request&, httplib::Response& res) { sendJson(res, svc.stats()); }));
    svr.Get("/api/categories",
            wrap([&svc](const httplib::Request&, httplib::Response& res) { sendJson(res, svc.categories()); }));

    // ---- venues
    svr.Get("/api/venues", wrap([&svc](const httplib::Request&, httplib::Response& res) { sendJson(res, svc.listVenues()); }));
    svr.Post("/api/venues", wrap([&svc, requireAdmin](const httplib::Request& req, httplib::Response& res) {
                 requireAdmin(req);
                 const json b = parseBody(req);
                 NewVenue v;
                 v.name = bodyString(b, "name", true);
                 v.location = bodyString(b, "location", false);
                 v.capacity = bodyIntRange(b, "capacity");
                 sendJson(res, svc.createVenue(v), 201);
             }));
    svr.Get("/api/venues/" + kId + "/free-slots", wrap([&svc](const httplib::Request& req, httplib::Response& res) {
                if (!req.has_param("date")) throw AppError::invalid("'date' is required (YYYY-MM-DD)");
                sendJson(res, svc.freeSlots(pathId(req), req.get_param_value("date"), textParam(req, "day_start", "08:00"),
                                            textParam(req, "day_end", "20:00"), intParam(req, "min_minutes", 30, 1, 1440)));
            }));

    // ---- events
    svr.Get("/api/events", wrap([&svc](const httplib::Request& req, httplib::Response& res) {
                EventQuery q;
                q.text = textParam(req, "q");
                q.category = textParam(req, "category");
                q.status = textParam(req, "status", "active");
                q.sort = textParam(req, "sort", "start");
                q.order = toLower(textParam(req, "order", "asc"));
                q.venueId = req.has_param("venue_id") ? parseInt(req.get_param_value("venue_id"), "venue_id", 1, 999999999999999LL) : 0;
                q.from = textParam(req, "from");
                q.to = textParam(req, "to");
                q.limit = intParam(req, "limit", 50, 1, 200);
                q.offset = intParam(req, "offset", 0, 0, 10000000);
                sendJson(res, svc.searchEvents(q));
            }));
    svr.Post("/api/events", wrap([&svc, requireAdmin](const httplib::Request& req, httplib::Response& res) {
                 requireAdmin(req);
                 const json b = parseBody(req);
                 NewEvent e;
                 e.title = bodyString(b, "title", true);
                 e.category = bodyString(b, "category", false);
                 e.description = bodyString(b, "description", false);
                 e.venueId = bodyInt(b, "venue_id");
                 e.start = bodyString(b, "start", true);
                 e.end = bodyString(b, "end", true);
                 e.capacity = bodyIntRange(b, "capacity");
                 sendJson(res, svc.createEvent(e), 201);
             }));
    svr.Get("/api/events/" + kId, wrap([&svc](const httplib::Request& req, httplib::Response& res) {
                sendJson(res, svc.getEvent(pathId(req)));
            }));
    svr.Post("/api/events/" + kId + "/cancel", wrap([&svc, requireAdmin](const httplib::Request& req, httplib::Response& res) {
                 requireAdmin(req);
                 requireJsonContentType(req);
                 sendJson(res, svc.cancelEvent(pathId(req)));
             }));

    // ---- registrations
    svr.Post("/api/events/" + kId + "/register", wrap([&svc](const httplib::Request& req, httplib::Response& res) {
                 const json b = parseBody(req);
                 sendJson(res, svc.registerStudent(pathId(req), bodyString(b, "student_id", true), bodyString(b, "name", true)), 201);
             }));
    svr.Delete("/api/events/" + kId + "/registrations/" + kSid, wrap([&svc](const httplib::Request& req, httplib::Response& res) {
                   sendJson(res, svc.cancelRegistration(pathId(req), req.matches[2]));
               }));
    svr.Post("/api/events/" + kId + "/checkin", wrap([&svc, requireAdmin](const httplib::Request& req, httplib::Response& res) {
                 requireAdmin(req);
                 const json b = parseBody(req);
                 sendJson(res, svc.checkIn(pathId(req), bodyString(b, "student_id", true)));
             }));
    svr.Get("/api/events/" + kId + "/attendees", wrap([&svc, requireAdmin](const httplib::Request& req, httplib::Response& res) {
                requireAdmin(req);
                sendJson(res, svc.attendees(pathId(req)));
            }));
    svr.Get("/api/events/" + kId + R"(/attendees\.csv)", wrap([&svc, requireAdmin](const httplib::Request& req, httplib::Response& res) {
                requireAdmin(req);
                const int64_t id = pathId(req);
                res.status = 200;
                res.set_content(svc.attendeesCsv(id), "text/csv; charset=utf-8");
                res.set_header("Content-Disposition", "attachment; filename=\"event-" + std::to_string(id) + "-attendees.csv\"");
            }));
    svr.Get("/api/students/" + kSid + "/registrations", wrap([&svc](const httplib::Request& req, httplib::Response& res) {
                sendJson(res, svc.studentRegistrations(req.matches[1]));
            }));

    // ---- reports
    svr.Get("/api/reports/attendance", wrap([&svc](const httplib::Request& req, httplib::Response& res) {
                sendJson(res, svc.attendanceReport(textParam(req, "include_cancelled") == "true"));
            }));
    svr.Get("/api/reports/popularity", wrap([&svc](const httplib::Request& req, httplib::Response& res) {
                sendJson(res, svc.popularityReport(intParam(req, "limit", 10, 1, 100)));
            }));
    svr.Get("/api/reports/venues", wrap([&svc](const httplib::Request&, httplib::Response& res) { sendJson(res, svc.venueUsageReport()); }));
    svr.Get("/api/reports/waitlist", wrap([&svc](const httplib::Request&, httplib::Response& res) { sendJson(res, svc.waitlistReport()); }));

    svr.set_error_handler([](const httplib::Request& req, httplib::Response& res) {
        if (!res.body.empty()) return;
        if (req.path.rfind("/api/", 0) == 0) {
            sendError(res, res.status, res.status == 404 ? "not_found" : "request_failed",
                      res.status == 404 ? "no such endpoint" : "request failed");
        } else {
            res.set_content("Not found", "text/plain; charset=utf-8");
        }
    });
    svr.set_exception_handler([](const httplib::Request&, httplib::Response& res, std::exception_ptr) {
        sendError(res, 500, "internal_error", "internal server error");
    });
    svr.set_post_routing_handler([](const httplib::Request& req, httplib::Response& res) {
        res.set_header("X-Content-Type-Options", "nosniff");
        res.set_header("X-Frame-Options", "DENY");
        res.set_header("Referrer-Policy", "no-referrer");
        // The page is fully self-contained: no external scripts, styles, fonts or frames.
        res.set_header("Content-Security-Policy",
                       "default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self' data:; "
                       "connect-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'");
        res.set_header("Cache-Control", req.path.rfind("/api/", 0) == 0 ? "no-store" : "no-cache");
    });
    svr.new_task_queue = [] { return new httplib::ThreadPool(16); };  // fixed worker pool: bounded resource use
    svr.set_payload_max_length(64 * 1024);
    svr.set_read_timeout(10, 0);
    svr.set_write_timeout(10, 0);

    if (!webDir.empty()) svr.set_mount_point("/", webDir);
}

std::string findWebDir(const std::string& explicitPath, const std::string& exePath) {
    namespace fs = std::filesystem;
    std::vector<fs::path> candidates;
    if (!explicitPath.empty()) {
        candidates.emplace_back(explicitPath);
    } else {
        candidates.emplace_back("web");
        candidates.emplace_back("../web");
        std::error_code ec;
        fs::path exeDir = fs::absolute(fs::path(exePath), ec).parent_path();
        if (!ec) {
            candidates.push_back(exeDir / "web");
            candidates.push_back(exeDir / ".." / "web");
            candidates.push_back(exeDir / ".." / ".." / "web");
        }
    }
    for (const fs::path& c : candidates) {
        std::error_code ec;
        if (fs::exists(c / "index.html", ec)) {
            fs::path canon = fs::weakly_canonical(c, ec);
            return ec ? c.string() : canon.string();
        }
    }
    return "";
}

void requestStop() { g_stop.store(true); }

int runServer(EventService& service, const ServerOptions& opt) {
    g_stop.store(false);
    httplib::Server svr;
    registerApi(svr, service, opt.webDir, opt.adminKey);

    if (!svr.bind_to_port(opt.host, opt.port)) {
        std::fprintf(stderr, "error: cannot listen on %s:%d (port already in use? try --port)\n", opt.host.c_str(), opt.port);
        return 1;
    }
    std::printf("CampusEvents: http://%s:%d/   (Ctrl+C to stop)\n", opt.host == "0.0.0.0" ? "localhost" : opt.host.c_str(), opt.port);
    if (opt.host == "0.0.0.0") {
        std::printf("warning: listening on all interfaces - students can register without logging in\n");
    }
    std::fflush(stdout);

    std::atomic<bool> finished{false};
    std::thread server([&svr, &finished] {
        svr.listen_after_bind();
        finished.store(true);
    });
    while (!g_stop.load() && !finished.load()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const bool stoppedOnRequest = g_stop.load();
    while (!finished.load()) {
        svr.stop();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    server.join();
    if (!stoppedOnRequest) {
        std::fprintf(stderr, "error: the server stopped unexpectedly\n");
        return 1;
    }
    std::printf("\nServer stopped.\n");
    return 0;
}

}  // namespace ce
