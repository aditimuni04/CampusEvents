#!/usr/bin/env bash
# Live demo of the main rules against a running server (see run_demo.sh).
#   bash data/demo_walkthrough.sh            # defaults: http://127.0.0.1:8080, key demo-admin
BASE="${BASE_URL:-http://127.0.0.1:8080}"
KEY="${CAMPUS_ADMIN_KEY:-demo-admin}"
J='Content-Type: application/json'
A="X-Admin-Key: $KEY"

say() { printf '\n\033[1m%s\033[0m\n' "$*"; }
# call METHOD PATH [BODY] [FIELDS] [extra curl args...]  - prints the HTTP status and only the interesting JSON fields
call() {
  local method="$1" path="$2" data="$3" fields="${4:-.*}" out code body
  shift 4 2>/dev/null || shift $#
  if [ -n "$data" ]; then out=$(curl -s -w '\n%{http_code}' -H "$J" "$@" -X "$method" "$BASE$path" -d "$data")
  else out=$(curl -s -w '\n%{http_code}' "$@" -X "$method" "$BASE$path"); fi
  code="${out##*$'\n'}"; body="${out%$'\n'*}"
  printf '  HTTP %s  ' "$code"
  if [ "$fields" = ".*" ]; then echo "$body"; else echo "$body" | grep -oE "$fields" | tr '\n' ' '; echo; fi
}
id_of() { sed -E 's/.*"id":([0-9]+).*/\1/' | head -1; }
EVENT_FIELDS='"(id|title|start|end|capacity)":("[^"]*"|[0-9]+)'
REG_FIELDS='"student_id":"[^"]*"|"status":"(confirmed|waiting)"|"waiting_position":([0-9]+|null)'
STAMP=$(date +%H%M%S)

say "1. Create a small venue (capacity 3)"
V=$(curl -s -H "$J" -H "$A" -X POST "$BASE/api/venues" -d "{\"name\":\"Walkthrough Room $STAMP\",\"capacity\":3}")
echo "  $V"
VID=$(echo "$V" | id_of)

say "2. Refuse an event bigger than the venue (capacity 5 > 3)"
call POST /api/events "{\"title\":\"Too Big\",\"venue_id\":$VID,\"start\":\"2030-05-01 10:00\",\"end\":\"2030-05-01 11:00\",\"capacity\":5}" '"error":"[^"]*"' -H "$A"

say "3. Create an event (capacity 2) from 10:00 to 12:00"
E=$(curl -s -H "$J" -H "$A" -X POST "$BASE/api/events" -d "{\"title\":\"Walkthrough Talk\",\"venue_id\":$VID,\"start\":\"2030-05-01 10:00\",\"end\":\"2030-05-01 12:00\",\"capacity\":2}")
printf '  '; echo "$E" | grep -oE "$EVENT_FIELDS" | tr '\n' ' '; echo
EID=$(echo "$E" | id_of)

say "4. Book the same room 11:00-13:00: refused as a clash, and the answer names the other event"
call POST /api/events "{\"title\":\"Clashing Talk\",\"venue_id\":$VID,\"start\":\"2030-05-01 11:00\",\"end\":\"2030-05-01 13:00\",\"capacity\":2}" '"error":"[^"]*"' -H "$A"

say "5. A back-to-back booking (12:00-13:00) is allowed"
call POST /api/events "{\"title\":\"Next Talk\",\"venue_id\":$VID,\"start\":\"2030-05-01 12:00\",\"end\":\"2030-05-01 13:00\",\"capacity\":2}" "$EVENT_FIELDS" -H "$A"

say "6. Four students register for a 2-seat event: the last two join the waiting list, in order"
for s in "W-ASHA Asha" "W-BELA Bela" "W-CHITRA Chitra" "W-DEEP Deep"; do
  set -- $s
  call POST "/api/events/$EID/register" "{\"student_id\":\"$1\",\"name\":\"$2\"}" "$REG_FIELDS"
done

say "7. Asha cancels: Chitra (first in the queue) is promoted automatically"
call DELETE "/api/events/$EID/registrations/W-ASHA" "" '"(cancelled|was)":("[^"]*"|true)|"promoted":\{[^}]*\}' 

say "8. Free slots in that room on 1 May 2030 (08:00-20:00, at least 60 minutes)"
call GET "/api/venues/$VID/free-slots?date=2030-05-01&min_minutes=60" "" '"free_slots":\[[^]]*\]'

say "9. The attendee list needs the admin key: 401 without it, the full list with it"
call GET "/api/events/$EID/attendees" "" '"error":"[^"]*"'
call GET "/api/events/$EID/attendees" "" '"(student_id|status)":"[^"]*"' -H "$A"
