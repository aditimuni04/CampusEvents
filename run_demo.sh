#!/usr/bin/env bash
# Build, load sample data and open the app on http://127.0.0.1:8080  (demo admin key: demo-admin)
set -euo pipefail
cd "$(dirname "$0")"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
export CAMPUS_ADMIN_KEY="${CAMPUS_ADMIN_KEY:-demo-admin}"
./build/campusevents serve --db campusevents.db --demo
