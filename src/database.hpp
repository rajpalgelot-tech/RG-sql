// RajSQL: database.hpp
// The public engine API. Ties together the parser, catalog, B+tree tables,
// the pager and the WAL into an executable SQL surface.
//
// C API JSON contract (used by the WASM bindings, the web playground and
// the CLI pretty-printer; keep in sync):
//   exec(sql) → JSON ARRAY, one object per statement:
//     { "ok": true,
//       "columns": ["id", "name"],            // SELECT only
//       "rows": [[1, "raj"], [2, null]],      // SELECT only
//       "message": "3 rows inserted",         // non-SELECT success note
//       "rowsAffected": 3,                    // INSERT/UPDATE/DELETE
//       "ms": 0.42 }
//     { "ok": false, "error": "no such table: foo" }
//   schema() → { "tables": [ { "name": "students",
//                              "columns": [{"name":"id","type":"INTEGER"}],
//                              "rows": 12 } ] }
//   crash_demo() → { "steps": ["..."], "rowsBeforeCrash": 500,
//                    "rowsAfterRecovery": 500, "recovered": true }

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "value.hpp"

namespace rajsql {

struct Result {
    bool ok = true;
    std::string error;
    std::string message;
    std::vector<std::string> columns;
    std::vector<std::vector<Value>> rows;
    size_t rows_affected = 0;
    double elapsed_ms = 0.0;

    std::string to_json() const;
};

class Database {
public:
    // Opens (creating if needed) the database file and its WAL, running
    // crash recovery if the log holds a committed transaction.
    explicit Database(const std::string& path);
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // Parses and executes every statement in `sql` (split on ';').
    // One Result per statement; each runs in its own implicit transaction
    // unless wrapped in BEGIN/COMMIT.
    std::vector<Result> exec(const std::string& sql);

    // JSON description of all tables (see contract above).
    std::string schema_json() const;

    // Runs the durability demo against a scratch table: insert rows, commit
    // the WAL, "lose power" before the checkpoint, reopen and recover.
    std::string crash_demo_json();

    // True when there are uncommitted changes (BEGIN without COMMIT).
    bool in_transaction() const;

    // Pimpl, exposed at file scope so the TU-local executor can hold a
    // reference to it (defined in database.cpp).
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace rajsql
