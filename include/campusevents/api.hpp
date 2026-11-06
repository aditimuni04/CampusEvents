#pragma once
// REST API (JSON) plus static hosting of the web page.
#include <string>

#include "campusevents/service.hpp"

namespace httplib { class Server; }

namespace ce {

struct ServerOptions {
    std::string host = "127.0.0.1";  // loopback only by default
    int port = 8080;
    std::string webDir;              // folder containing index.html ("" = API only)
    std::string adminKey;            // required in the X-Admin-Key header for admin endpoints
};

// Registers all routes and security headers on `svr`. Used by the real server and by the tests.
void registerApi(httplib::Server& svr, EventService& service, const std::string& webDir, const std::string& adminKey);

// Looks for a "web" folder next to the working directory or the executable.
std::string findWebDir(const std::string& explicitPath, const std::string& exePath);

// Binds, serves until requestStop() (Ctrl+C) is called. Returns a process exit code.
int runServer(EventService& service, const ServerOptions& opt);
void requestStop();

// Constant-time string comparison (does not leak how many leading characters matched).
bool secureEquals(const std::string& a, const std::string& b);

}  // namespace ce
