#pragma once
// Thin RAII wrapper around one SQLite connection. All values are passed as bound parameters.
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "campusevents/types.hpp"

struct sqlite3;

namespace ce {

using Param = std::variant<int64_t, std::string>;

class Database {
public:
    explicit Database(const std::string& path);  // ":memory:" works too
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    void exec(const std::string& sql);  // trusted, parameter-free SQL (schema, pragmas, BEGIN/COMMIT)
    json query(const std::string& sql, const std::vector<Param>& params = {});  // array of row objects
    int64_t scalar(const std::string& sql, const std::vector<Param>& params = {});
    void run(const std::string& sql, const std::vector<Param>& params = {});  // INSERT / UPDATE / DELETE
    int64_t lastInsertId() const;
    int changes() const;

    void initSchema();  // creates tables, indexes and integrity triggers if they do not exist yet

private:
    sqlite3* db_ = nullptr;
};

// BEGIN IMMEDIATE ... COMMIT. If it goes out of scope without commit() (an exception, an early
// return) it rolls back, so a half-finished multi-step change can never be left in the database.
class Transaction {
public:
    explicit Transaction(Database& db);
    ~Transaction();
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    void commit();

private:
    Database& db_;
    bool done_ = false;
};

}  // namespace ce
