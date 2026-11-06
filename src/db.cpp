#include "campusevents/db.hpp"

#include <sqlite3.h>

namespace ce {

namespace {

class Stmt {
public:
    Stmt(sqlite3* db, const std::string& sql) : db_(db) {
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &s_, nullptr) != SQLITE_OK) {
            throw DbError(std::string("SQL prepare failed: ") + sqlite3_errmsg(db));
        }
    }
    ~Stmt() { sqlite3_finalize(s_); }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    void bindAll(const std::vector<Param>& params) {
        int idx = 1;
        for (const Param& p : params) {
            int rc;
            if (std::holds_alternative<int64_t>(p)) {
                rc = sqlite3_bind_int64(s_, idx, std::get<int64_t>(p));
            } else {
                const std::string& v = std::get<std::string>(p);
                rc = sqlite3_bind_text(s_, idx, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
            }
            if (rc != SQLITE_OK) throw DbError(std::string("SQL bind failed: ") + sqlite3_errmsg(db_));
            ++idx;
        }
    }
    bool step() {
        const int rc = sqlite3_step(s_);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        throw DbError(std::string("SQL step failed: ") + sqlite3_errmsg(db_));
    }
    sqlite3_stmt* raw() { return s_; }

private:
    sqlite3* db_;
    sqlite3_stmt* s_ = nullptr;
};

// Integrity rules enforced by the database itself, as a second line of defence behind the C++ checks.
const char* kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS venues (
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    name      TEXT    NOT NULL UNIQUE COLLATE NOCASE,
    capacity  INTEGER NOT NULL CHECK (capacity > 0),
    location  TEXT    NOT NULL DEFAULT ''
);
CREATE TABLE IF NOT EXISTS events (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    title       TEXT    NOT NULL,
    category    TEXT    NOT NULL DEFAULT 'General',
    description TEXT    NOT NULL DEFAULT '',
    venue_id    INTEGER NOT NULL REFERENCES venues(id),
    start_ts    INTEGER NOT NULL,
    end_ts      INTEGER NOT NULL,
    capacity    INTEGER NOT NULL CHECK (capacity > 0),
    status      TEXT    NOT NULL DEFAULT 'active' CHECK (status IN ('active', 'cancelled')),
    CHECK (end_ts > start_ts)
);
CREATE INDEX IF NOT EXISTS idx_events_venue_time ON events(venue_id, start_ts, end_ts);
CREATE INDEX IF NOT EXISTS idx_events_start ON events(start_ts);
CREATE TABLE IF NOT EXISTS students (
    id    TEXT PRIMARY KEY,
    name  TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS registrations (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    event_id    INTEGER NOT NULL REFERENCES events(id) ON DELETE CASCADE,
    student_id  TEXT    NOT NULL REFERENCES students(id),
    status      TEXT    NOT NULL CHECK (status IN ('confirmed', 'waiting')),
    checked_in  INTEGER NOT NULL DEFAULT 0 CHECK (checked_in IN (0, 1)),
    created_at  INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
    UNIQUE (event_id, student_id)
);
CREATE INDEX IF NOT EXISTS idx_reg_event_status ON registrations(event_id, status, id);
CREATE INDEX IF NOT EXISTS idx_reg_student ON registrations(student_id);

CREATE TRIGGER IF NOT EXISTS trg_no_venue_clash
BEFORE INSERT ON events
WHEN NEW.status = 'active' AND EXISTS (
    SELECT 1 FROM events x
    WHERE x.venue_id = NEW.venue_id AND x.status = 'active'
      AND x.start_ts < NEW.end_ts AND NEW.start_ts < x.end_ts)
BEGIN SELECT RAISE(ABORT, 'venue clash'); END;

CREATE TRIGGER IF NOT EXISTS trg_event_fits_venue
BEFORE INSERT ON events
WHEN NEW.capacity > (SELECT capacity FROM venues WHERE id = NEW.venue_id)
BEGIN SELECT RAISE(ABORT, 'event larger than venue'); END;

CREATE TRIGGER IF NOT EXISTS trg_capacity_insert
BEFORE INSERT ON registrations
WHEN NEW.status = 'confirmed' AND
     (SELECT COUNT(*) FROM registrations WHERE event_id = NEW.event_id AND status = 'confirmed')
     >= (SELECT capacity FROM events WHERE id = NEW.event_id)
BEGIN SELECT RAISE(ABORT, 'event full'); END;

CREATE TRIGGER IF NOT EXISTS trg_capacity_update
BEFORE UPDATE OF status ON registrations
WHEN NEW.status = 'confirmed' AND OLD.status <> 'confirmed' AND
     (SELECT COUNT(*) FROM registrations WHERE event_id = NEW.event_id AND status = 'confirmed')
     >= (SELECT capacity FROM events WHERE id = NEW.event_id)
BEGIN SELECT RAISE(ABORT, 'event full'); END;
)SQL";

}  // namespace

Database::Database(const std::string& path) {
    if (sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        std::string msg = db_ ? sqlite3_errmsg(db_) : "out of memory";
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        throw DbError("cannot open database '" + path + "': " + msg);
    }
    sqlite3_busy_timeout(db_, 5000);
    exec("PRAGMA foreign_keys = ON;");
    if (path != ":memory:") exec("PRAGMA journal_mode = WAL; PRAGMA synchronous = NORMAL;");
}

Database::~Database() {
    if (db_) sqlite3_close(db_);
}

void Database::initSchema() { exec(kSchema); }

void Database::exec(const std::string& sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "unknown error";
        sqlite3_free(err);
        throw DbError("SQL error: " + msg);
    }
}

json Database::query(const std::string& sql, const std::vector<Param>& params) {
    Stmt st(db_, sql);
    st.bindAll(params);
    json rows = json::array();
    sqlite3_stmt* s = st.raw();
    const int cols = sqlite3_column_count(s);
    while (st.step()) {
        json row = json::object();
        for (int c = 0; c < cols; ++c) {
            const char* name = sqlite3_column_name(s, c);
            switch (sqlite3_column_type(s, c)) {
                case SQLITE_INTEGER: row[name] = static_cast<int64_t>(sqlite3_column_int64(s, c)); break;
                case SQLITE_FLOAT: row[name] = sqlite3_column_double(s, c); break;
                case SQLITE_TEXT:
                    row[name] = std::string(reinterpret_cast<const char*>(sqlite3_column_text(s, c)),
                                            static_cast<size_t>(sqlite3_column_bytes(s, c)));
                    break;
                default: row[name] = nullptr; break;
            }
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

int64_t Database::scalar(const std::string& sql, const std::vector<Param>& params) {
    const json rows = query(sql, params);
    if (rows.empty() || rows[0].empty()) return 0;
    const json v = rows[0].begin().value();
    return v.is_number() ? v.get<int64_t>() : 0;
}

void Database::run(const std::string& sql, const std::vector<Param>& params) {
    Stmt st(db_, sql);
    st.bindAll(params);
    while (st.step()) {}
}

int64_t Database::lastInsertId() const { return static_cast<int64_t>(sqlite3_last_insert_rowid(db_)); }
int Database::changes() const { return sqlite3_changes(db_); }

Transaction::Transaction(Database& db) : db_(db) { db_.exec("BEGIN IMMEDIATE"); }

Transaction::~Transaction() {
    if (!done_) {
        try {
            db_.exec("ROLLBACK");
        } catch (...) {
            // nothing sensible to do while unwinding
        }
    }
}

void Transaction::commit() {
    db_.exec("COMMIT");
    done_ = true;
}

}  // namespace ce
