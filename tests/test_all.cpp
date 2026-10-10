// RajSQL: tests/test_all.cpp
// Self-contained test runner (no framework). Each group uses its own
// database file, removed (along with its WAL) before the group starts.
//
// Build/link with the engine sources; run with no arguments.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "../src/database.hpp"
#include "../src/value.hpp"

// ── Check bookkeeping ──────────────────────────────────────────────────────

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (cond) {                                                            \
            ++g_pass;                                                          \
        } else {                                                               \
            ++g_fail;                                                          \
            std::cout << "  FAIL: " << msg << "  (line " << __LINE__ << ")\n"; \
        }                                                                      \
    } while (0)

static void group(const char* name) {
    std::cout << "== " << name << " ==\n";
}

// Removes the .db and .db.wal files for a fresh group.
static void fresh(const std::string& path) {
    std::remove(path.c_str());
    std::remove((path + ".wal").c_str());
}

// ── Small helpers ──────────────────────────────────────────────────────────

// True when exec(sql) yields exactly one Result and it succeeded.
static bool ok1(rajsql::Database& db, const std::string& sql) {
    auto rs = db.exec(sql);
    return rs.size() == 1 && rs[0].ok;
}

// True when exec(sql) yields exactly one failed Result (never throws out).
static bool fails1(rajsql::Database& db, const std::string& sql) {
    try {
        auto rs = db.exec(sql);
        return rs.size() == 1 && !rs[0].ok && !rs[0].error.empty();
    } catch (const rajsql::RajSQLError& e) {
        // Contract says errors come back as Results; a throw should not
        // escape, but if it does at least require a message.
        return !e.message.empty();
    } catch (const std::exception&) {
        return true;
    }
}

// First cell of the first row of a successful SELECT as int64, or `dflt`.
static int64_t cell_int(rajsql::Database& db, const std::string& sql, int64_t dflt) {
    auto rs = db.exec(sql);
    if (rs.size() != 1 || !rs[0].ok || rs[0].rows.empty() || rs[0].rows[0].empty()) return dflt;
    const auto& v = rs[0].rows[0][0];
    if (v.is_null()) return dflt;
    return v.tag == rajsql::Value::Tag::Real ? (int64_t)v.real_v : v.int_v;
}

// First cell of the first row (e.g. COUNT / SUM results).
static rajsql::Value cell_value(rajsql::Database& db, const std::string& sql) {
    auto rs = db.exec(sql);
    if (rs.size() != 1 || !rs[0].ok || rs[0].rows.empty() || rs[0].rows[0].empty())
        return rajsql::Value::null();
    return rs[0].rows[0][0];
}

static bool is_int(const rajsql::Value& v, int64_t expect) {
    return v.tag == rajsql::Value::Tag::Int && v.int_v == expect;
}

static bool is_real(const rajsql::Value& v, double expect) {
    return v.tag == rajsql::Value::Tag::Real && std::fabs(v.real_v - expect) < 1e-9;
}

static bool is_text(const rajsql::Value& v, const std::string& expect) {
    return v.tag == rajsql::Value::Tag::Text && v.text_v == expect;
}

// Number of rows in the (single) result of a SELECT.
static size_t row_count(rajsql::Database& db, const std::string& sql) {
    auto rs = db.exec(sql);
    if (rs.size() != 1 || !rs[0].ok) return (size_t)-1;
    return rs[0].rows.size();
}

// ── 1. CRUD roundtrip + reopen ─────────────────────────────────────────────

static void test_crud() {
    group("1. CRUD roundtrip + reopen");
    const std::string path = "t_crud.db";
    fresh(path);

    {
        rajsql::Database db(path);
        CHECK(ok1(db, "CREATE TABLE students (id INTEGER, name TEXT, gpa REAL)"),
              "create table students");
        CHECK(ok1(db, "INSERT INTO students VALUES (1, 'Raj', 3.8), (2, 'Asha', 3.9)"),
              "multi-tuple insert");

        auto rs = db.exec("SELECT * FROM students");
        CHECK(rs.size() == 1 && rs[0].ok, "select * succeeds");
        if (rs.size() == 1 && rs[0].ok) {
            CHECK(rs[0].columns.size() == 3 && rs[0].columns[0] == "id" &&
                      rs[0].columns[1] == "name" && rs[0].columns[2] == "gpa",
                  "column names id/name/gpa");
            CHECK(rs[0].rows.size() == 2, "two rows");
            if (rs[0].rows.size() == 2) {
                CHECK(is_int(rs[0].rows[0][0], 1) && is_text(rs[0].rows[0][1], "Raj") &&
                          is_real(rs[0].rows[0][2], 3.8),
                      "row 1 exact values");
                CHECK(is_int(rs[0].rows[1][0], 2) && is_text(rs[0].rows[1][1], "Asha") &&
                          is_real(rs[0].rows[1][2], 3.9),
                      "row 2 exact values");
            }
        }
    }  // destroy: the pager must have persisted by now

    rajsql::Database db2(path);  // reopen the same file
    CHECK(row_count(db2, "SELECT * FROM students") == 2, "data survives reopen");
    CHECK(is_int(cell_value(db2, "SELECT id FROM students WHERE name = 'Asha'"), 2),
          "value survives reopen");
}

// ── 2. B+tree stress ───────────────────────────────────────────────────────

static void test_bptree() {
    group("2. B+tree stress (1500 rows)");
    const std::string path = "t_pager.db";
    fresh(path);

    rajsql::Database db(path);
    CHECK(ok1(db, "CREATE TABLE nums (id INTEGER, val INTEGER)"), "create nums");
    for (int64_t i = 1; i <= 1500; ++i) {
        auto rs = db.exec("INSERT INTO nums VALUES (" + std::to_string(i) + ", " +
                          std::to_string(i * i) + ")");
        if (rs.size() != 1 || !rs[0].ok) {
            CHECK(false, "single-row insert");
            return;
        }
    }
    CHECK(true, "1500 single-row inserts");

    CHECK(cell_int(db, "SELECT COUNT(*) FROM nums", -1) == 1500, "COUNT(*) == 1500");
    CHECK(cell_int(db, "SELECT val FROM nums WHERE id = 1", -1) == 1, "point lookup id=1");
    CHECK(cell_int(db, "SELECT val FROM nums WHERE id = 750", -1) == 562500,
          "point lookup id=750");
    CHECK(cell_int(db, "SELECT val FROM nums WHERE id = 1500", -1) == 2250000,
          "point lookup id=1500");

    auto rs = db.exec("SELECT val FROM nums ORDER BY val DESC LIMIT 5");
    CHECK(rs.size() == 1 && rs[0].ok && rs[0].rows.size() == 5, "top-5 query returns 5 rows");
    if (rs.size() == 1 && rs[0].ok && rs[0].rows.size() == 5) {
        int64_t expect[5] = {2250000, 2247001, 2244004, 2241009, 2238016};  // 1500^2 .. 1496^2
        bool all = true;
        for (int i = 0; i < 5; ++i)
            if (!is_int(rs[0].rows[i][0], expect[i])) all = false;
        CHECK(all, "top-5 vals are the five largest squares DESC");
    }
}

// ── 3. WHERE operators and expressions ─────────────────────────────────────

static void test_where() {
    group("3. WHERE operators");
    const std::string path = "t_where.db";
    fresh(path);

    rajsql::Database db(path);
    CHECK(ok1(db, "CREATE TABLE w (a INTEGER, b TEXT, g REAL)"), "create w");
    CHECK(ok1(db, "INSERT INTO w VALUES (1, 'x', 1.5), (2, 'y', 2.5), (3, 'x', 3.5)"),
          "seed rows");

    auto n = [&](const std::string& sql) { return row_count(db, sql); };
    CHECK(n("SELECT * FROM w WHERE a = 2") == 1, "a = 2");
    CHECK(n("SELECT * FROM w WHERE a != 1") == 2, "a != 1");
    CHECK(n("SELECT * FROM w WHERE a <> 1") == 2, "a <> 1");
    CHECK(n("SELECT * FROM w WHERE a < 3") == 2, "a < 3");
    CHECK(n("SELECT * FROM w WHERE a <= 2") == 2, "a <= 2");
    CHECK(n("SELECT * FROM w WHERE a > 1") == 2, "a > 1");
    CHECK(n("SELECT * FROM w WHERE a >= 2") == 2, "a >= 2");
    CHECK(n("SELECT * FROM w WHERE a = 1 AND b = 'x'") == 1, "AND");
    CHECK(n("SELECT * FROM w WHERE a = 1 OR a = 3") == 2, "OR");
    CHECK(n("SELECT * FROM w WHERE NOT (a = 1)") == 2, "NOT");
    CHECK(n("SELECT * FROM w WHERE (a = 1 OR a = 2) AND g > 2.0") == 1, "parentheses");
    CHECK(n("SELECT * FROM w WHERE b = 'x'") == 2, "text equality");

    auto rs = db.exec("SELECT a * 10 FROM w WHERE a = 2");
    CHECK(rs.size() == 1 && rs[0].ok && rs[0].rows.size() == 1 &&
              is_int(rs[0].rows[0][0], 20),
          "arithmetic in SELECT item (a * 10 = 20)");

    auto rs2 = db.exec("SELECT -a FROM w WHERE a = 3");
    CHECK(rs2.size() == 1 && rs2[0].ok && rs2[0].rows.size() == 1 &&
              is_int(rs2[0].rows[0][0], -3),
          "unary minus in SELECT item");
}

// ── 4. Aggregates ──────────────────────────────────────────────────────────

static void test_aggregates() {
    group("4. Aggregates");
    const std::string path = "t_agg.db";
    fresh(path);

    rajsql::Database db(path);
    CHECK(ok1(db, "CREATE TABLE ag (n INTEGER, r REAL, s TEXT)"), "create ag");
    CHECK(ok1(db, "INSERT INTO ag VALUES (1, 1.5, 'b'), (2, 2.5, 'a'), (NULL, NULL, NULL)"),
          "seed rows incl. NULLs");

    CHECK(is_int(cell_value(db, "SELECT COUNT(*) FROM ag"), 3), "COUNT(*) = 3");
    CHECK(is_int(cell_value(db, "SELECT COUNT(n) FROM ag"), 2), "COUNT(n) skips NULL");
    CHECK(is_int(cell_value(db, "SELECT SUM(n) FROM ag"), 3), "SUM(n) = 3");
    CHECK(is_real(cell_value(db, "SELECT AVG(r) FROM ag"), 2.0), "AVG(r) = 2.0");
    CHECK(is_int(cell_value(db, "SELECT MIN(n) FROM ag"), 1), "MIN(n) = 1");
    CHECK(is_int(cell_value(db, "SELECT MAX(n) FROM ag"), 2), "MAX(n) = 2");
    CHECK(is_text(cell_value(db, "SELECT MIN(s) FROM ag"), "a"), "MIN on TEXT lexicographic");
    CHECK(is_text(cell_value(db, "SELECT MAX(s) FROM ag"), "b"), "MAX on TEXT lexicographic");

    CHECK(ok1(db, "CREATE TABLE empty (x INTEGER, y TEXT)"), "create empty");
    CHECK(is_int(cell_value(db, "SELECT COUNT(*) FROM empty"), 0), "empty COUNT(*) = 0");
    CHECK(cell_value(db, "SELECT SUM(x) FROM empty").is_null(), "empty SUM = NULL");
    CHECK(cell_value(db, "SELECT AVG(x) FROM empty").is_null(), "empty AVG = NULL");
    CHECK(cell_value(db, "SELECT MIN(y) FROM empty").is_null(), "empty MIN = NULL");
    CHECK(cell_value(db, "SELECT MAX(y) FROM empty").is_null(), "empty MAX = NULL");
}

// ── 5. JOIN ────────────────────────────────────────────────────────────────

static void test_join() {
    group("5. JOIN");
    const std::string path = "t_join.db";
    fresh(path);

    rajsql::Database db(path);
    CHECK(ok1(db, "CREATE TABLE customers (id INTEGER, name TEXT)"), "create customers");
    CHECK(ok1(db, "CREATE TABLE orders (id INTEGER, cust INTEGER, amt REAL)"), "create orders");
    CHECK(ok1(db, "INSERT INTO customers VALUES (1, 'Raj'), (2, 'Asha'), (3, 'No Orders')"),
          "seed customers");
    CHECK(ok1(db, "INSERT INTO orders VALUES (10, 1, 5.0), (11, 1, 7.5), (12, 2, 9.0)"),
          "seed orders");

    auto rs = db.exec(
        "SELECT customers.name, orders.amt FROM customers JOIN orders "
        "ON customers.id = orders.cust WHERE orders.amt > 6.0");
    CHECK(rs.size() == 1 && rs[0].ok, "join query succeeds");
    if (rs.size() == 1 && rs[0].ok) {
        CHECK(rs[0].rows.size() == 2, "join with WHERE returns 2 rows");
        CHECK(rs[0].columns.size() == 2, "two select items");
        // Qualified names in select items and WHERE: verify one row precisely.
        bool saw_asha_9 = false;
        for (const auto& row : rs[0].rows)
            if (is_text(row[0], "Asha") && is_real(row[1], 9.0)) saw_asha_9 = true;
        CHECK(saw_asha_9, "qualified t.col values correct");
    }

    CHECK(row_count(db, "SELECT * FROM customers JOIN orders ON customers.id = orders.cust") == 3,
          "star join row count = 3");
}

// ── 6. UPDATE / DELETE ─────────────────────────────────────────────────────

static void test_update_delete() {
    group("6. UPDATE / DELETE");
    const std::string path = "t_upd.db";
    fresh(path);

    rajsql::Database db(path);
    CHECK(ok1(db, "CREATE TABLE t (id INTEGER, name TEXT, score INTEGER)"), "create t");
    CHECK(ok1(db, "INSERT INTO t VALUES (1, 'a', 10), (2, 'b', 20), (3, 'c', 30)"), "seed rows");

    auto rs = db.exec("UPDATE t SET score = 99 WHERE id = 2");
    CHECK(rs.size() == 1 && rs[0].ok && rs[0].rows_affected == 1, "UPDATE affects 1 row");
    CHECK(cell_int(db, "SELECT score FROM t WHERE id = 2", -1) == 99, "value actually changed");

    auto rs2 = db.exec("UPDATE t SET score = 0");  // no WHERE: all rows
    CHECK(rs2.size() == 1 && rs2[0].ok && rs2[0].rows_affected == 3, "WHERE-less UPDATE hits 3");
    CHECK(row_count(db, "SELECT * FROM t WHERE score = 0") == 3, "all scores now 0");

    auto rs3 = db.exec("UPDATE t SET name = 'z' WHERE id = 999");
    CHECK(rs3.size() == 1 && rs3[0].ok && rs3[0].rows_affected == 0, "UPDATE miss = 0 rows");

    auto rs4 = db.exec("DELETE FROM t WHERE id = 1");
    CHECK(rs4.size() == 1 && rs4[0].ok && rs4[0].rows_affected == 1, "DELETE affects 1 row");
    CHECK(row_count(db, "SELECT * FROM t WHERE id = 1") == 0, "row gone");

    auto rs5 = db.exec("DELETE FROM t WHERE id = 999");
    CHECK(rs5.size() == 1 && rs5[0].ok && rs5[0].rows_affected == 0, "DELETE miss = 0 rows");
    CHECK(cell_int(db, "SELECT COUNT(*) FROM t", -1) == 2, "two rows remain");
}

// ── 7. Transactions ────────────────────────────────────────────────────────

static void test_transactions() {
    group("7. Transactions");
    const std::string path = "t_txn.db";
    fresh(path);

    rajsql::Database db(path);
    CHECK(ok1(db, "CREATE TABLE tx (id INTEGER, v TEXT)"), "create tx");
    CHECK(ok1(db, "INSERT INTO tx VALUES (1, 'base')"), "seed row");

    CHECK(ok1(db, "BEGIN"), "BEGIN");
    CHECK(db.in_transaction(), "in_transaction() true after BEGIN");
    CHECK(ok1(db, "INSERT INTO tx VALUES (2, 'rolled')"), "insert inside txn");
    CHECK(ok1(db, "ROLLBACK"), "ROLLBACK");
    CHECK(!db.in_transaction(), "in_transaction() false after ROLLBACK");
    CHECK(row_count(db, "SELECT * FROM tx WHERE id = 2") == 0, "rolled-back row absent");

    CHECK(ok1(db, "BEGIN"), "BEGIN 2");
    CHECK(ok1(db, "INSERT INTO tx VALUES (3, 'kept')"), "insert inside txn 2");
    CHECK(ok1(db, "COMMIT"), "COMMIT");
    CHECK(row_count(db, "SELECT * FROM tx WHERE id = 3") == 1, "committed row visible");
    CHECK(cell_int(db, "SELECT COUNT(*) FROM tx", -1) == 2, "base + committed rows");

    {  // committed data must survive constructing a fresh Database on the path
        rajsql::Database db2(path);
        CHECK(row_count(db2, "SELECT * FROM tx WHERE id = 3") == 1,
              "committed row survives fresh open");
    }
}

// ── 8. WAL crash recovery ──────────────────────────────────────────────────

static void test_wal() {
    group("8. WAL crash recovery");
    const std::string path = "t_wal.db";
    fresh(path);

    // crash_demo_json(): insert rows, commit to WAL, simulate power loss.
    {
        rajsql::Database db(path);
        std::string json = db.crash_demo_json();
        CHECK(json.find("\"recovered\"") != std::string::npos &&
                  json.find("true") != std::string::npos,
              "crash_demo reports recovered: true");

        // Extract rowsBeforeCrash / rowsAfterRecovery numbers and compare.
        auto num_after = [&](const char* key) -> long long {
            std::string k = std::string("\"") + key + "\"";
            size_t p = json.find(k);
            if (p == std::string::npos) return -1;
            p = json.find(':', p + k.size());
            if (p == std::string::npos) return -1;
            return std::strtoll(json.c_str() + p + 1, nullptr, 10);
        };
        long long before = num_after("rowsBeforeCrash");
        long long after = num_after("rowsAfterRecovery");
        CHECK(before > 0, "rowsBeforeCrash present and positive");
        CHECK(before == after, "rowsAfterRecovery == rowsBeforeCrash");
    }

    // Two Database instances open on the same path at once: the second must
    // see the data the first wrote (via file/WAL state).
    {
        fresh(path);
        auto db1 = std::make_unique<rajsql::Database>(path);
        CHECK(ok1(*db1, "CREATE TABLE shared (id INTEGER, tag TEXT)"), "create shared (db1)");
        CHECK(ok1(*db1, "INSERT INTO shared VALUES (1, 'one')"), "insert (db1)");

        rajsql::Database db2(path);  // second instance while db1 still open
        CHECK(row_count(db2, "SELECT * FROM shared WHERE id = 1") == 1,
              "second open instance sees committed data");
        CHECK(is_text(cell_value(db2, "SELECT tag FROM shared WHERE id = 1"), "one"),
              "value correct via second instance");
    }
}

// ── 9. Error handling ──────────────────────────────────────────────────────

static void test_errors() {
    group("9. Error handling");
    const std::string path = "t_err.db";
    fresh(path);

    rajsql::Database db(path);
    CHECK(ok1(db, "CREATE TABLE good (a INTEGER, b TEXT)"), "create good");

    CHECK(fails1(db, "SELECT * FROM missing_table"), "SELECT from missing table -> ok=false");
    CHECK(fails1(db, "INSERT INTO good VALUES (1)"), "wrong column count -> ok=false");
    CHECK(fails1(db, "INSERT INTO good VALUES ('not-a-number', 'x')"),
          "TEXT literal into INTEGER column -> ok=false");
    CHECK(fails1(db, "SELEC * FROM good"), "syntax error -> ok=false");
    CHECK(fails1(db, "CREATE TABLE good (a INTEGER)"), "duplicate CREATE TABLE -> ok=false");
    CHECK(cell_int(db, "SELECT COUNT(*) FROM good", -1) == 0,
          "engine still works after all errors");
}

// ── 10. NULL handling ──────────────────────────────────────────────────────

static void test_nulls() {
    group("10. NULL handling");
    const std::string path = "t_null.db";
    fresh(path);

    rajsql::Database db(path);
    CHECK(ok1(db, "CREATE TABLE nn (i INTEGER, r REAL, t TEXT)"), "create nn");
    CHECK(ok1(db, "INSERT INTO nn VALUES (NULL, NULL, NULL)"), "insert bare NULLs");
    CHECK(ok1(db, "INSERT INTO nn VALUES (7, 7.5, 'seven')"), "insert non-NULL row");

    auto rs = db.exec("SELECT i, r, t FROM nn WHERE i = 7");
    CHECK(rs.size() == 1 && rs[0].ok && rs[0].rows.size() == 1, "non-NULL row found");
    if (rs.size() == 1 && rs[0].ok && rs[0].rows.size() == 1) {
        CHECK(is_int(rs[0].rows[0][0], 7) && is_real(rs[0].rows[0][1], 7.5) &&
                  is_text(rs[0].rows[0][2], "seven"),
              "typed values read back intact");
    }

    // NULL roundtrip: COUNT(*) counts the row, COUNT(col) must skip it,
    // which proves the NULLs were stored and read back as NULL.
    CHECK(cell_int(db, "SELECT COUNT(*) FROM nn", -1) == 2, "both rows stored");
    CHECK(cell_int(db, "SELECT COUNT(i) FROM nn", -1) == 1, "NULL roundtrips: COUNT(col) skips");
    CHECK(is_int(cell_value(db, "SELECT MIN(i) FROM nn"), 7), "NULL not treated as 0");

    // NULL comparisons match nothing (three-valued logic).
    // NOTE: the parser (parser.hpp) has no IS operator, so IS NULL / IS NOT
    // so IS NULL cannot be tested; only `= NULL` matching zero rows is verified.
    CHECK(row_count(db, "SELECT * FROM nn WHERE i = NULL") == 0, "i = NULL matches nothing");
    CHECK(row_count(db, "SELECT * FROM nn WHERE r = NULL") == 0, "r = NULL matches nothing");
    CHECK(row_count(db, "SELECT * FROM nn WHERE t = NULL") == 0, "t = NULL matches nothing");
}

// ── 11. Empty / whitespace input ───────────────────────────────────────────

static void test_empty_input() {
    group("11. Empty / whitespace input");
    const std::string path = "t_empty.db";
    fresh(path);

    rajsql::Database db(path);
    auto rs = db.exec("   ;  ");
    CHECK(rs.empty(), "whitespace-only exec returns empty vector");
}

int main() {
    test_crud();
    test_bptree();
    test_where();
    test_aggregates();
    test_join();
    test_update_delete();
    test_transactions();
    test_wal();
    test_errors();
    test_nulls();
    test_empty_input();

    std::cout << g_pass << " checks passed, " << g_fail << " failed\n";
    return g_fail > 0 ? 1 : 0;
}
