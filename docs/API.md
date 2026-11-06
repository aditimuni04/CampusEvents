# CampusEvents REST API

Base URL `http://127.0.0.1:8080`. Requests and responses are JSON (`Content-Type: application/json` is required on every `POST`). Admin endpoints need the header `X-Admin-Key: <key>`.

Times are wall-clock strings: `2026-11-02 10:00` (a `T` instead of the space is accepted). Dates are `2026-11-02`.

## Errors

```json
{"error": "'Seminar Hall' is already booked for 'Python Workshop' (2026-11-03 11:00 - 12:30)",
 "code": "venue_clash",
 "details": {"clashes_with": {"id": 17, "title": "Python Workshop", "start": "2026-11-03 11:00", "end": "2026-11-03 12:30"}}}
```

| Status | `code` | Meaning |
| --- | --- | --- |
| 400 | `invalid_input` | A field is missing, has the wrong type, or breaks a limit |
| 401 | `unauthorized` | Admin key missing or wrong |
| 404 | `not_found` | Unknown event, venue or registration, or unknown route |
| 409 | `venue_clash`, `student_clash`, `already_registered`, `event_cancelled`, `already_cancelled`, `not_confirmed`, `duplicate_venue` | The request is valid but conflicts with existing data |
| 413 | | Body larger than 64 KB |
| 415 | `unsupported_media_type` | `POST` without `application/json` |
| 500 | `internal_error` | Unexpected failure (details are logged, not returned) |

## Events

### `GET /api/events`

Query parameters (all optional): `q` (text), `venue_id`, `category`, `from`, `to` (dates; `to` includes the whole day), `status` (`active` default, `cancelled`, `all`), `sort` (`start` default, `title`, `popularity`, `seats`), `order` (`asc`/`desc`), `limit` (1–200, default 50), `offset`.

```bash
curl 'http://127.0.0.1:8080/api/events?q=workshop&from=2026-11-01&sort=popularity&order=desc&limit=5'
```
```json
{"total": 7, "items": [{"id": 12, "title": "Git and GitHub Workshop", "category": "Workshop", "venue_id": 1, "venue_name": "Main Auditorium",
  "start": "2026-11-02 09:00", "end": "2026-11-02 11:30", "capacity": 80, "confirmed": 80, "waiting": 6, "checked_in": 0,
  "seats_left": 0, "full": true, "cancelled": false, "status": "active", "description": "…"}]}
```

### `POST /api/events` (admin)

```bash
curl -X POST http://127.0.0.1:8080/api/events -H 'Content-Type: application/json' -H 'X-Admin-Key: demo-admin' \
  -d '{"title":"Intro to C++","category":"Tech","venue_id":2,"start":"2026-11-10 10:00","end":"2026-11-10 12:00","capacity":60,"description":"Optional"}'
```

`201` with the created event, `409 venue_clash` if the room is taken, `400` if `capacity` is larger than the venue or the times are invalid.

### `POST /api/events/{id}/cancel` (admin)

Marks the event cancelled and frees its time slot. `409 already_cancelled` if it already was.

## Registration

### `POST /api/events/{id}/register`

```bash
curl -X POST http://127.0.0.1:8080/api/events/12/register -H 'Content-Type: application/json' -d '{"student_id":"PCE26-0001","name":"Asha Patil"}'
```
```json
{"status": "waiting", "waiting_position": 7, "student_id": "PCE26-0001", "name": "Asha Patil",
 "message": "The event is full - you are on the waiting list.", "event": {"...": "..."}}
```

`student_id`: 3–20 letters, digits, `-` or `_` (stored upper-case). `status` is `confirmed` or `waiting`. `409` for a duplicate sign-up, a cancelled event, or a student clash (the student is already signed up for an overlapping event; `details.clashes_with` names it).

### `DELETE /api/events/{id}/registrations/{studentId}`

```json
{"cancelled": true, "was": "confirmed", "promoted": {"student_id": "PCE26-0042", "name": "Bela Shah"}, "event": {"...": "..."}}
```

`promoted` is `null` when nobody was waiting, or when a waiting student cancelled.

### `POST /api/events/{id}/checkin` (admin)

Body `{"student_id": "…"}`. Only confirmed students can be checked in (`409 not_confirmed` otherwise). Repeating the call is safe (`already_checked_in: true`).

### `GET /api/events/{id}/attendees` and `/attendees.csv` (admin)

Confirmed students first, then the waiting list in queue order, each with `waiting_position`.

### `GET /api/students/{studentId}/registrations`

All of a student's registrations ordered by event time, with `status`, `waiting_position` and `event_status`.

## Venues

* `GET /api/venues` – list.
* `POST /api/venues` (admin) – `{"name": "...", "capacity": 120, "location": "..."}`; `409 duplicate_venue` (names are case-insensitive).
* `GET /api/venues/{id}/free-slots?date=2026-11-02&day_start=08:00&day_end=20:00&min_minutes=60`: `date` is required, the rest default to `08:00`, `20:00`, `30`. `day_end` may be `24:00`.

```json
{"venue": {"id": 2, "name": "Seminar Hall", "capacity": 120, "location": "Block B, first floor"}, "date": "2026-11-02",
 "free_slots": [{"start": "2026-11-02 08:00", "end": "2026-11-02 09:00", "minutes": 60}],
 "booked": [{"id": 9, "title": "Resume Clinic", "start": "2026-11-02 09:00", "end": "2026-11-02 10:30"}]}
```

## Reports

| Endpoint | Rows |
| --- | --- |
| `GET /api/reports/attendance[?include_cancelled=true]` | per event: `capacity`, `confirmed`, `waiting`, `checked_in`, `fill_pct`, `show_up_pct` |
| `GET /api/reports/popularity?limit=10` | events ranked by `demand` (confirmed + waiting) |
| `GET /api/reports/venues` | per venue: `events`, `booked_hours`, `confirmed`, `avg_fill_pct` (venues with no events included) |
| `GET /api/reports/waitlist` | events with students waiting, with `oldest_request` |
| `GET /api/stats` | totals for the page header |
