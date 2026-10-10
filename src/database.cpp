// RajSQL: database.cpp
// Catalog management, the statement executor, expression evaluation and
// the crash-recovery demo.

#include "database.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>
#include <utility>

#include "bptree.hpp"
#include "pager.hpp"
#include "parser.hpp"

namespace rajsql {

namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string type_name(ColumnType t) {
    switch (t) {
        case ColumnType::Integer: return "INTEGER";
        case ColumnType::Real: return "REAL";
        case ColumnType::Text: return "TEXT";
    }
    return "?";
}

std::string json_escape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (unsigned char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

// ── table record (stored in the catalog B+tree) ────────────────────────────
// [nameLen:u16][name][colCount:u16][per col: type:u8][nameLen:u16][name]
// [rootPage:u32][nextRowid:i64]

struct TableRecord {
    std::string name;
    std::vector<std::pair<std::string, ColumnType>> columns;
    uint32_t root = 0;
    int64_t next_rowid = 1;
};

std::vector<uint8_t> encode_table(const TableRecord& rec) {
    std::vector<uint8_t> out;
    auto push_u16 = [&out](uint16_t v) {
        out.push_back(static_cast<uint8_t>(v & 0xff));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
    };
    auto push_u32 = [&out](uint32_t v) {
        for (int i = 0; i < 4; ++i)
            out.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xff));
    };
    auto push_i64 = [&out](int64_t v) {
        auto u = static_cast<uint64_t>(v);
        for (int i = 0; i < 8; ++i)
            out.push_back(static_cast<uint8_t>((u >> (i * 8)) & 0xff));
    };
    push_u16(static_cast<uint16_t>(rec.name.size()));
    out.insert(out.end(), rec.name.begin(), rec.name.end());
    push_u16(static_cast<uint16_t>(rec.columns.size()));
    for (const auto& [cname, ctype] : rec.columns) {
        out.push_back(ctype == ColumnType::Integer ? 0 : ctype == ColumnType::Real ? 1 : 2);
        push_u16(static_cast<uint16_t>(cname.size()));
        out.insert(out.end(), cname.begin(), cname.end());
    }
    push_u32(rec.root);
    push_i64(rec.next_rowid);
    return out;
}

TableRecord decode_table(const std::vector<uint8_t>& data) {
    TableRecord rec;
    size_t pos = 0;
    auto read_u16 = [&]() {
        uint16_t v = static_cast<uint16_t>(data[pos] | (data[pos + 1] << 8));
        pos += 2;
        return v;
    };
    auto read_u32 = [&]() {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(data[pos + i]) << (i * 8);
        pos += 4;
        return v;
    };
    auto read_i64 = [&]() {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(data[pos + i]) << (i * 8);
        pos += 8;
        return static_cast<int64_t>(v);
    };
    uint16_t name_len = read_u16();
    rec.name.assign(reinterpret_cast<const char*>(data.data()) + pos, name_len);
    pos += name_len;
    uint16_t col_count = read_u16();
    for (uint16_t i = 0; i < col_count; ++i) {
        uint8_t t = data[pos++];
        ColumnType type =
            t == 0 ? ColumnType::Integer : t == 1 ? ColumnType::Real : ColumnType::Text;
        uint16_t len = read_u16();
        std::string cname(reinterpret_cast<const char*>(data.data()) + pos, len);
        pos += len;
        rec.columns.emplace_back(std::move(cname), type);
    }
    rec.root = read_u32();
    rec.next_rowid = read_i64();
    return rec;
}

// ── expression evaluation ──────────────────────────────────────────────────

struct ColRef {
    std::string table;  // as written in the query ("" when unqualified)
    std::string name;
};

// Resolves a column reference against the query schema.
// Returns the row index; throws on ambiguity / not found.
size_t resolve_column(const Expr& e, const std::vector<ColRef>& schema) {
    std::string col = lower(e.column);
    std::string tab = lower(e.table);
    size_t found = SIZE_MAX;
    bool ambiguous = false;
    for (size_t i = 0; i < schema.size(); ++i) {
        if (lower(schema[i].name) != col) continue;
        if (!tab.empty() && lower(schema[i].table) != tab) continue;
        if (found != SIZE_MAX) {
            ambiguous = true;
            break;
        }
        found = i;
    }
    if (ambiguous) throw RajSQLError("ambiguous column '" + e.column + "'");
    if (found == SIZE_MAX)
        throw RajSQLError("no such column: " +
                          (e.table.empty() ? "" : e.table + ".") + e.column);
    return found;
}

bool is_true(const Value& v) { return v.tag == Value::Tag::Int && v.int_v != 0; }

Value eval_expr(const Expr& e, const std::vector<ColRef>& schema,
                const std::vector<Value>& row) {
    switch (e.kind) {
        case Expr::Kind::Literal:
            return e.literal;
        case Expr::Kind::Column:
            return row[resolve_column(e, schema)];
        case Expr::Kind::UnaryNeg: {
            Value v = eval_expr(*e.left, schema, row);
            if (v.is_null()) return Value::null();
            if (v.tag == Value::Tag::Int) return Value::make_int(-v.int_v);
            return Value::make_real(-v.real_v);
        }
        case Expr::Kind::UnaryNot: {
            Value v = eval_expr(*e.left, schema, row);
            if (v.is_null()) return Value::null();
            return Value::make_int(is_true(v) ? 0 : 1);
        }
        case Expr::Kind::Func:
            // aggregates are handled by the SELECT executor
            throw RajSQLError("aggregate function used outside aggregation");
        case Expr::Kind::Binary: {
            using Op = Expr::Op;
            if (e.op == Op::And || e.op == Op::Or) {
                Value l = eval_expr(*e.left, schema, row);
                if (e.op == Op::And && !l.is_null() && !is_true(l))
                    return Value::make_int(0);  // false short-circuits AND
                if (e.op == Op::Or && is_true(l)) return Value::make_int(1);
                Value r = eval_expr(*e.right, schema, row);
                bool rt = !r.is_null() && is_true(r);
                if (e.op == Op::And) {
                    if (!rt) return Value::make_int(0);
                    if (l.is_null() || r.is_null()) return Value::null();
                    return Value::make_int(1);
                }
                if (rt) return Value::make_int(1);
                if (l.is_null() || r.is_null()) return Value::null();
                return Value::make_int(0);
            }

            Value l = eval_expr(*e.left, schema, row);
            Value r = eval_expr(*e.right, schema, row);
            if (l.is_null() || r.is_null()) return Value::null();

            if (e.op == Op::Eq || e.op == Op::Ne || e.op == Op::Lt || e.op == Op::Le ||
                e.op == Op::Gt || e.op == Op::Ge) {
                int c = l.compare(r);
                bool result = false;
                switch (e.op) {
                    case Op::Eq: result = c == 0; break;
                    case Op::Ne: result = c != 0; break;
                    case Op::Lt: result = c < 0; break;
                    case Op::Le: result = c <= 0; break;
                    case Op::Gt: result = c > 0; break;
                    case Op::Ge: result = c >= 0; break;
                    default: break;
                }
                return Value::make_int(result ? 1 : 0);
            }

            // arithmetic (Int/Int stays integer; any Real promotes)
            if (!l.is_numeric() || !r.is_numeric())
                throw RajSQLError("arithmetic requires numeric operands (got " +
                                  l.to_display_string() + " and " +
                                  r.to_display_string() + ")");
            bool bothInt = l.tag == Value::Tag::Int && r.tag == Value::Tag::Int;
            auto as_d = [](const Value& v) {
                return v.tag == Value::Tag::Int ? static_cast<double>(v.int_v) : v.real_v;
            };
            switch (e.op) {
                case Op::Add:
                    if (bothInt) return Value::make_int(l.int_v + r.int_v);
                    return Value::make_real(as_d(l) + as_d(r));
                case Op::Sub:
                    if (bothInt) return Value::make_int(l.int_v - r.int_v);
                    return Value::make_real(as_d(l) - as_d(r));
                case Op::Mul:
                    if (bothInt) return Value::make_int(l.int_v * r.int_v);
                    return Value::make_real(as_d(l) * as_d(r));
                case Op::Div: {
                    if (bothInt) {
                        if (r.int_v == 0) throw RajSQLError("division by zero");
                        return Value::make_int(l.int_v / r.int_v);
                    }
                    double rd = as_d(r);
                    if (rd == 0.0) throw RajSQLError("division by zero");
                    return Value::make_real(as_d(l) / rd);
                }
                default: break;
            }
            throw RajSQLError("unknown binary operator");
        }
    }
    return Value::null();
}

std::string expr_display_name(const Expr& e) {
    switch (e.kind) {
        case Expr::Kind::Literal: return e.literal.to_display_string();
        case Expr::Kind::Column:
            return e.table.empty() ? e.column : e.table + "." + e.column;
        case Expr::Kind::Func: {
            if (e.agg == Expr::FuncKind::CountStar) return "COUNT(*)";
            const char* names[] = {"", "COUNT", "SUM", "AVG", "MIN", "MAX"};
            return std::string(names[static_cast<int>(e.agg)]) + "(" + e.column + ")";
        }
        case Expr::Kind::UnaryNeg: return "-" + expr_display_name(*e.left);
        case Expr::Kind::UnaryNot: return "NOT " + expr_display_name(*e.left);
        case Expr::Kind::Binary: return "(expr)";
    }
    return "(expr)";
}

}  // namespace

// ── Result JSON ────────────────────────────────────────────────────────────

std::string Result::to_json() const {
    std::string out = "{\"ok\":";
    out += ok ? "true" : "false";
    if (!ok) {
        out += ",\"error\":\"" + json_escape(error) + "\"}";
        return out;
    }
    if (!message.empty()) out += ",\"message\":\"" + json_escape(message) + "\"";
    if (rows_affected) out += ",\"rowsAffected\":" + std::to_string(rows_affected);
    if (!columns.empty()) {
        out += ",\"columns\":[";
        for (size_t i = 0; i < columns.size(); ++i) {
            if (i) out += ',';
            out += '"' + json_escape(columns[i]) + '"';
        }
        out += "],\"rows\":[";
        for (size_t i = 0; i < rows.size(); ++i) {
            if (i) out += ',';
            out += '[';
            for (size_t j = 0; j < rows[i].size(); ++j) {
                if (j) out += ',';
                out += rows[i][j].to_json();
            }
            out += ']';
        }
        out += ']';
    }
    out += ",\"ms\":" + std::to_string(elapsed_ms) + "}";
    return out;
}

// ── Database internals ─────────────────────────────────────────────────────

struct Database::Impl {
    Pager pager;
    std::unique_ptr<BPTree> catalog;
    std::map<std::string, TableRecord> tables;   // key: lowercase name
    std::map<std::string, uint32_t> table_ids;   // lowercase name → catalog key
    bool txn_active = false;                     // BEGIN seen, no COMMIT yet

    explicit Impl(const std::string& path)
        : pager(path), catalog(std::make_unique<BPTree>(pager, pager.catalog_root())) {
        reload_catalog();
    }

    void reload_catalog() {
        tables.clear();
        table_ids.clear();
        catalog = std::make_unique<BPTree>(pager, pager.catalog_root());
        if (pager.catalog_root() == 0) return;
        std::vector<std::pair<int64_t, std::vector<uint8_t>>> rows;
        catalog->scan(rows);
        for (auto& [id, bytes] : rows) {
            TableRecord rec = decode_table(bytes);
            std::string key = lower(rec.name);
            table_ids[key] = static_cast<uint32_t>(id);
            tables[key] = std::move(rec);
        }
    }

    TableRecord& must_get_table(const std::string& name) {
        auto it = tables.find(lower(name));
        if (it == tables.end()) throw RajSQLError("no such table: " + name);
        return it->second;
    }

    void persist_catalog_record(uint32_t id, const TableRecord& rec) {
        if (pager.catalog_root() == 0)  // first table ever: create the catalog tree
            catalog = std::make_unique<BPTree>(pager, BPTree::create(pager));
        catalog->upsert(static_cast<int64_t>(id), encode_table(rec));
        pager.set_catalog_root(catalog->root());
    }
};

Database::Database(const std::string& path) : impl_(std::make_unique<Impl>(path)) {}
Database::~Database() = default;

bool Database::in_transaction() const { return impl_->txn_active; }

std::string Database::schema_json() const {
    std::string out = "{\"tables\":[";
    bool first = true;
    for (const auto& [key, rec] : impl_->tables) {
        if (!first) out += ',';
        first = false;
        out += "{\"name\":\"" + json_escape(rec.name) + "\",\"columns\":[";
        for (size_t i = 0; i < rec.columns.size(); ++i) {
            if (i) out += ',';
            out += "{\"name\":\"" + json_escape(rec.columns[i].first) +
                   "\",\"type\":\"" + type_name(rec.columns[i].second) + "\"}";
        }
        out += "],\"rows\":" +
               std::to_string(BPTree(impl_->pager, rec.root).count_live()) + "}";
    }
    out += "]}";
    return out;
}

// ── executor ───────────────────────────────────────────────────────────────

namespace {

class Executor {
public:
    // When crash_after_wal is true, the commit protocol stops right after
    // the WAL commit, simulating a power cut before checkpointing.
    Executor(Database::Impl& impl, bool crash_after_wal)
        : impl_(impl), crash_(crash_after_wal) {}

    // When true, statements do NOT auto-commit (BEGIN/COMMIT manage that,
    // or the crash demo commits manually).
    bool in_txn_scope = false;

    Result run(const Statement& stmt) {
        auto start = std::chrono::steady_clock::now();
        Result res;
        try {
            dispatch(stmt, res);
        } catch (const RajSQLError& e) {
            res = Result{};
            res.ok = false;
            res.error = e.message;
        }
        if (res.ok && !in_txn_scope && !impl_.txn_active) {
            try {
                impl_.pager.commit(crash_);
            } catch (const RajSQLError& e) {
                res.ok = false;
                res.error = e.message;
            }
        }
        res.elapsed_ms = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - start)
                             .count();
        return res;
    }

private:
    Database::Impl& impl_;
    bool crash_;

    BPTree tree_of(const TableRecord& rec) const {
        return BPTree(impl_.pager, rec.root);
    }

    std::vector<std::vector<Value>> load_rows(const TableRecord& rec) const {
        std::vector<std::pair<int64_t, std::vector<uint8_t>>> raw;
        tree_of(rec).scan(raw);
        std::vector<std::vector<Value>> rows;
        rows.reserve(raw.size());
        for (auto& [id, bytes] : raw) rows.push_back(deserialize_row(bytes));
        return rows;
    }

    void dispatch(const Statement& stmt, Result& res) {
        switch (stmt.type) {
            case Statement::Type::CreateTable: run_create(stmt, res); break;
            case Statement::Type::DropTable: run_drop(stmt, res); break;
            case Statement::Type::Insert: run_insert(stmt, res); break;
            case Statement::Type::Select: run_select(stmt, res); break;
            case Statement::Type::Update: run_update(stmt, res); break;
            case Statement::Type::Delete: run_delete(stmt, res); break;
            case Statement::Type::Begin:
                if (impl_.txn_active)
                    throw RajSQLError("transaction already in progress");
                impl_.txn_active = true;
                res.message = "Transaction started";
                break;
            case Statement::Type::Commit:
                if (!impl_.txn_active)
                    throw RajSQLError("no transaction in progress");
                impl_.pager.commit(crash_);
                impl_.txn_active = false;
                res.message = "Transaction committed";
                break;
            case Statement::Type::Rollback:
                if (!impl_.txn_active)
                    throw RajSQLError("no transaction in progress");
                impl_.pager.rollback();
                impl_.reload_catalog();
                impl_.txn_active = false;
                res.message = "Transaction rolled back";
                break;
        }
    }

    // ── DDL ────────────────────────────────────────────────────────────
    void run_create(const Statement& stmt, Result& res) {
        std::string key = lower(stmt.table_name);
        if (impl_.tables.count(key))
            throw RajSQLError("table '" + stmt.table_name + "' already exists");
        TableRecord rec;
        rec.name = stmt.table_name;
        for (const auto& col : stmt.columns)
            rec.columns.emplace_back(col.name, col.sql_type);
        rec.root = BPTree::create(impl_.pager);
        rec.next_rowid = 1;
        uint32_t id = impl_.pager.next_table_id();
        impl_.pager.set_next_table_id(id + 1);
        impl_.persist_catalog_record(id, rec);
        impl_.table_ids[key] = id;
        impl_.tables[key] = std::move(rec);
        res.message = "Table '" + stmt.table_name + "' created";
    }

    void run_drop(const Statement& stmt, Result& res) {
        std::string key = lower(stmt.table_name);
        auto it = impl_.tables.find(key);
        if (it == impl_.tables.end())
            throw RajSQLError("no such table: " + stmt.table_name);
        // v1 limitation: the dropped table's pages are not reclaimed.
        impl_.catalog->erase(static_cast<int64_t>(impl_.table_ids[key]));
        impl_.pager.set_catalog_root(impl_.catalog->root());
        impl_.tables.erase(it);
        impl_.table_ids.erase(key);
        res.message = "Table '" + stmt.table_name + "' dropped";
    }

    // ── INSERT ─────────────────────────────────────────────────────────
    void run_insert(const Statement& stmt, Result& res) {
        TableRecord& rec = impl_.must_get_table(stmt.table_name);
        BPTree tree = tree_of(rec);

        std::vector<size_t> targets;
        if (stmt.insert_columns.empty()) {
            for (size_t i = 0; i < rec.columns.size(); ++i) targets.push_back(i);
        } else {
            if (stmt.insert_columns.size() > rec.columns.size())
                throw RajSQLError("INSERT lists more columns than table has");
            std::vector<bool> seen(rec.columns.size(), false);
            for (const auto& cname : stmt.insert_columns) {
                size_t idx = SIZE_MAX;
                for (size_t i = 0; i < rec.columns.size(); ++i) {
                    if (lower(rec.columns[i].first) == lower(cname)) {
                        idx = i;
                        break;
                    }
                }
                if (idx == SIZE_MAX) throw RajSQLError("no such column: " + cname);
                if (seen[idx])
                    throw RajSQLError("duplicate column in INSERT: " + cname);
                seen[idx] = true;
                targets.push_back(idx);
            }
        }

        for (const auto& exprs : stmt.insert_rows) {
            if (exprs.size() != targets.size())
                throw RajSQLError("INSERT expects " + std::to_string(targets.size()) +
                                  " values, got " + std::to_string(exprs.size()));
            std::vector<Value> row(rec.columns.size(), Value::null());
            for (size_t i = 0; i < exprs.size(); ++i) {
                const Expr& e = *exprs[i];
                if (e.kind != Expr::Kind::Literal)
                    throw RajSQLError("only literal values are supported in INSERT");
                const auto& [cname, ctype] = rec.columns[targets[i]];
                row[targets[i]] = coerce_value(e.literal, ctype, cname);
            }
            int64_t rowid = rec.next_rowid++;
            tree.upsert(rowid, serialize_row(row));
        }

        // persist tree root (may have changed on split) + next_rowid
        rec.root = tree.root();
        impl_.persist_catalog_record(impl_.table_ids[lower(stmt.table_name)], rec);
        res.rows_affected = stmt.insert_rows.size();
        res.message = std::to_string(stmt.insert_rows.size()) + " row" +
                      (stmt.insert_rows.size() == 1 ? "" : "s") + " inserted into '" +
                      stmt.table_name + "'";
    }

    // ── SELECT ─────────────────────────────────────────────────────────
    struct LoadedTable {
        std::string name;
        const TableRecord* rec;
    };

    void run_select(const Statement& stmt, Result& res) {
        std::vector<LoadedTable> loaded;
        auto add_table = [&](const std::string& name) {
            std::string key = lower(name);
            auto it = impl_.tables.find(key);
            if (it == impl_.tables.end())
                throw RajSQLError("no such table: " + name);
            for (const auto& t : loaded) {
                if (lower(t.name) == key)
                    throw RajSQLError("table '" + name + "' appears more than once "
                                      "(self-joins are not supported in v1)");
            }
            loaded.push_back({it->second.name, &it->second});
        };
        add_table(stmt.from_table);
        for (const auto& j : stmt.joins) add_table(j.table);

        // schema = every column of every table, in join order
        std::vector<ColRef> schema;
        for (const auto& t : loaded) {
            for (const auto& [cname, ctype] : t.rec->columns)
                schema.push_back(ColRef{t.name, cname});
        }

        // base rows, then left-deep nested-loop joins
        std::vector<std::vector<Value>> rows = load_rows(*loaded[0].rec);
        for (size_t j = 0; j < stmt.joins.size(); ++j) {
            std::vector<std::vector<Value>> right = load_rows(*loaded[j + 1].rec);
            size_t upto = 0;
            for (size_t t = 0; t <= j + 1; ++t)
                upto += loaded[t].rec->columns.size();
            std::vector<ColRef> schema_upto(schema.begin(), schema.begin() + upto);
            std::vector<std::vector<Value>> joined;
            for (const auto& l : rows) {
                for (const auto& r : right) {
                    std::vector<Value> combined;
                    combined.reserve(l.size() + r.size());
                    combined.insert(combined.end(), l.begin(), l.end());
                    combined.insert(combined.end(), r.begin(), r.end());
                    Value ok = eval_expr(*stmt.joins[j].on, schema_upto, combined);
                    if (is_true(ok)) joined.push_back(std::move(combined));
                }
            }
            rows = std::move(joined);
        }

        if (stmt.where) {
            std::vector<std::vector<Value>> filtered;
            for (auto& row : rows) {
                if (is_true(eval_expr(*stmt.where, schema, row)))
                    filtered.push_back(std::move(row));
            }
            rows = std::move(filtered);
        }

        bool has_aggregate = false;
        for (const auto& item : stmt.items) {
            if (item.expr && item.expr->kind == Expr::Kind::Func) has_aggregate = true;
        }

        if (has_aggregate) {
            run_select_aggregate(stmt, schema, rows, res);
        } else {
            run_select_rows(stmt, schema, rows, res);
        }
    }

    void run_select_rows(const Statement& stmt, const std::vector<ColRef>& schema,
                         const std::vector<std::vector<Value>>& rows, Result& res) {
        // projection targets: either a row column index or a computed expression
        struct Target {
            bool computed = false;
            size_t idx = 0;
            const Expr* expr = nullptr;
        };
        std::vector<Target> proj;
        res.columns.clear();
        for (const auto& item : stmt.items) {
            if (item.star) {
                for (size_t i = 0; i < schema.size(); ++i) {
                    proj.push_back(Target{false, i, nullptr});
                    res.columns.push_back(schema[i].name);
                }
                continue;
            }
            if (item.expr->kind == Expr::Kind::Column) {
                size_t idx = resolve_column(*item.expr, schema);
                proj.push_back(Target{false, idx, nullptr});
                res.columns.push_back(item.alias.empty() ? schema[idx].name
                                                         : item.alias);
            } else {
                proj.push_back(Target{true, 0, item.expr.get()});
                res.columns.push_back(item.alias.empty()
                                          ? expr_display_name(*item.expr)
                                          : item.alias);
            }
        }

        std::vector<std::vector<Value>> out;
        out.reserve(rows.size());
        for (const auto& row : rows) {
            std::vector<Value> projected;
            projected.reserve(proj.size());
            for (const auto& t : proj)
                projected.push_back(t.computed ? eval_expr(*t.expr, schema, row)
                                               : row[t.idx]);
            out.push_back(std::move(projected));
        }

        if (!stmt.order_by.empty()) {
            std::vector<size_t> order_idx;
            for (const auto& ob : stmt.order_by) {
                size_t idx = SIZE_MAX;
                std::string col = lower(ob.column);
                std::string tab = lower(ob.table);
                for (size_t i = 0; i < schema.size(); ++i) {
                    if (lower(schema[i].name) != col) continue;
                    if (!tab.empty() && lower(schema[i].table) != tab) continue;
                    if (idx != SIZE_MAX)
                        throw RajSQLError("ambiguous ORDER BY column '" +
                                          ob.column + "'");
                    idx = i;
                }
                if (idx == SIZE_MAX)
                    throw RajSQLError("no such column in ORDER BY: " +
                                      (ob.table.empty() ? "" : ob.table + ".") +
                                      ob.column);
                order_idx.push_back(idx);
            }
            std::vector<size_t> perm(rows.size());
            for (size_t i = 0; i < perm.size(); ++i) perm[i] = i;
            std::stable_sort(perm.begin(), perm.end(), [&](size_t a, size_t b) {
                for (size_t k = 0; k < stmt.order_by.size(); ++k) {
                    size_t src = order_idx[k];
                    int c = rows[a][src].compare(rows[b][src]);
                    if (c != 0)
                        return stmt.order_by[k].descending ? c > 0 : c < 0;
                }
                return false;
            });
            std::vector<std::vector<Value>> sorted;
            sorted.reserve(out.size());
            for (size_t i : perm) sorted.push_back(std::move(out[i]));
            out = std::move(sorted);
        }

        if (stmt.limit >= 0 && static_cast<size_t>(stmt.limit) < out.size())
            out.resize(static_cast<size_t>(stmt.limit));

        res.rows = std::move(out);
    }

    void run_select_aggregate(const Statement& stmt, const std::vector<ColRef>& schema,
                              const std::vector<std::vector<Value>>& rows, Result& res) {
        res.columns.clear();
        std::vector<Value> out_row;
        for (const auto& item : stmt.items) {
            const Expr* e = item.expr.get();
            if (!e || e->kind != Expr::Kind::Func)
                throw RajSQLError("mixing aggregates and plain columns is not "
                                  "supported (no GROUP BY in v1)");
            res.columns.push_back(item.alias.empty() ? expr_display_name(*e)
                                                     : item.alias);

            if (e->agg == Expr::FuncKind::CountStar) {
                out_row.push_back(Value::make_int(static_cast<int64_t>(rows.size())));
                continue;
            }
            auto col = Expr::column_ref("", e->column);
            size_t idx = resolve_column(*col, schema);
            std::vector<Value> vals;
            for (const auto& row : rows) {
                if (!row[idx].is_null()) vals.push_back(row[idx]);
            }
            switch (e->agg) {
                case Expr::FuncKind::Count:
                    out_row.push_back(
                        Value::make_int(static_cast<int64_t>(vals.size())));
                    break;
                case Expr::FuncKind::Sum: {
                    if (vals.empty()) {
                        out_row.push_back(Value::null());
                        break;
                    }
                    bool anyReal = false;
                    for (auto& v : vals) anyReal |= v.tag == Value::Tag::Real;
                    if (anyReal) {
                        double s = 0;
                        for (auto& v : vals) s += v.tag == Value::Tag::Int
                                                      ? static_cast<double>(v.int_v)
                                                      : v.real_v;
                        out_row.push_back(Value::make_real(s));
                    } else {
                        int64_t s = 0;
                        for (auto& v : vals) s += v.int_v;
                        out_row.push_back(Value::make_int(s));
                    }
                    break;
                }
                case Expr::FuncKind::Avg: {
                    if (vals.empty()) {
                        out_row.push_back(Value::null());
                        break;
                    }
                    double s = 0;
                    for (auto& v : vals)
                        s += v.tag == Value::Tag::Int ? static_cast<double>(v.int_v)
                                                      : v.real_v;
                    out_row.push_back(
                        Value::make_real(s / static_cast<double>(vals.size())));
                    break;
                }
                case Expr::FuncKind::Min:
                case Expr::FuncKind::Max: {
                    if (vals.empty()) {
                        out_row.push_back(Value::null());
                        break;
                    }
                    Value best = vals[0];
                    for (auto& v : vals) {
                        int c = v.compare(best);
                        if ((e->agg == Expr::FuncKind::Min && c < 0) ||
                            (e->agg == Expr::FuncKind::Max && c > 0))
                            best = v;
                    }
                    out_row.push_back(best);
                    break;
                }
                case Expr::FuncKind::CountStar:
                    break;  // handled above
            }
        }
        res.rows.push_back(std::move(out_row));
    }

    // ── UPDATE / DELETE ────────────────────────────────────────────────
    void run_update(const Statement& stmt, Result& res) {
        TableRecord& rec = impl_.must_get_table(stmt.table_name);
        BPTree tree = tree_of(rec);
        std::vector<ColRef> schema;
        for (const auto& [cname, ctype] : rec.columns)
            schema.push_back(ColRef{"", cname});

        std::vector<size_t> set_idx;
        for (const auto& [cname, expr] : stmt.set_clauses) {
            size_t idx = SIZE_MAX;
            for (size_t i = 0; i < rec.columns.size(); ++i) {
                if (lower(rec.columns[i].first) == lower(cname)) {
                    idx = i;
                    break;
                }
            }
            if (idx == SIZE_MAX) throw RajSQLError("no such column: " + cname);
            set_idx.push_back(idx);
        }

        size_t affected = 0;
        std::vector<std::pair<int64_t, std::vector<uint8_t>>> raw;
        tree.scan(raw);
        for (auto& [rowid, bytes] : raw) {
            std::vector<Value> row = deserialize_row(bytes);
            if (stmt.where && !is_true(eval_expr(*stmt.where, schema, row))) continue;
            // SET expressions evaluate against the pre-update row
            std::vector<Value> updated = row;
            for (size_t i = 0; i < set_idx.size(); ++i) {
                const Expr& e = *stmt.set_clauses[i].second;
                Value v = eval_expr(e, schema, row);
                updated[set_idx[i]] = coerce_value(
                    v, rec.columns[set_idx[i]].second, rec.columns[set_idx[i]].first);
            }
            tree.upsert(rowid, serialize_row(updated));
            ++affected;
        }
        rec.root = tree.root();
        impl_.persist_catalog_record(impl_.table_ids[lower(stmt.table_name)], rec);
        res.rows_affected = affected;
        res.message = std::to_string(affected) + " row" + (affected == 1 ? "" : "s") +
                      " updated";
    }

    void run_delete(const Statement& stmt, Result& res) {
        TableRecord& rec = impl_.must_get_table(stmt.table_name);
        BPTree tree = tree_of(rec);
        std::vector<ColRef> schema;
        for (const auto& [cname, ctype] : rec.columns)
            schema.push_back(ColRef{"", cname});

        std::vector<std::pair<int64_t, std::vector<uint8_t>>> raw;
        tree.scan(raw);
        size_t affected = 0;
        for (auto& [rowid, bytes] : raw) {
            std::vector<Value> row = deserialize_row(bytes);
            if (stmt.where && !is_true(eval_expr(*stmt.where, schema, row))) continue;
            tree.erase(rowid);
            ++affected;
        }
        rec.root = tree.root();
        impl_.persist_catalog_record(impl_.table_ids[lower(stmt.table_name)], rec);
        res.rows_affected = affected;
        res.message = std::to_string(affected) + " row" + (affected == 1 ? "" : "s") +
                      " deleted";
    }
};

}  // namespace

std::vector<Result> Database::exec(const std::string& sql) {
    std::vector<Result> results;
    std::vector<std::unique_ptr<Statement>> statements;
    try {
        statements = parse_sql(sql);
    } catch (const RajSQLError& e) {
        // parse errors surface as a normal failed Result, never as a throw
        Result r;
        r.ok = false;
        r.error = e.message;
        results.push_back(std::move(r));
        return results;
    }
    Executor executor(*impl_, /*crash_after_wal=*/false);
    executor.in_txn_scope = impl_->txn_active;
    for (const auto& stmt : statements) {
        Result r = executor.run(*stmt);
        results.push_back(std::move(r));
        if (!results.back().ok) break;  // stop at the first failure
    }
    return results;
}

// ── crash-recovery demo ────────────────────────────────────────────────────
//
// Runs against this database's file using two throwaway Database instances:
//   1. insert 500 rows and commit them with the FULL protocol (checkpointed)
//   2. insert 200 more rows, but stop the commit protocol right after the
//      WAL commit, but the data file is NOT updated yet
//   3. destroy the instance (nothing further is checkpointed, like a crash)
//   4. reopen: the pager must replay the WAL and all 700 rows must be there

std::string Database::crash_demo_json() {
    const std::string path = impl_->pager.path();
    std::vector<std::string> steps;
    int64_t before = 0;
    int64_t after = 0;

    auto count_rows = [](Database& db) -> int64_t {
        auto results = db.exec("SELECT COUNT(*) FROM wal_demo");
        if (!results.empty() && results[0].ok && !results[0].rows.empty())
            return results[0].rows[0][0].int_v;
        return -1;
    };

    {
        Database db(path);
        // fresh scratch table (ignore "no such table" on first run)
        db.exec("DROP TABLE wal_demo");
        auto created = db.exec("CREATE TABLE wal_demo (id INTEGER, payload TEXT)");
        if (created.empty() || !created[0].ok)
            throw RajSQLError("crash demo setup failed");
        steps.push_back("Created scratch table 'wal_demo'");

        std::string sql500 = "INSERT INTO wal_demo VALUES ";
        for (int i = 1; i <= 500; ++i) {
            if (i > 1) sql500 += ",";
            sql500 += "(" + std::to_string(i) + ", 'row-" + std::to_string(i) + "')";
        }
        auto ins = db.exec(sql500);
        if (ins.empty() || !ins[0].ok) throw RajSQLError("crash demo insert failed");
        steps.push_back("Inserted 500 rows (full commit: WAL + checkpoint)");

        // Insert 200 more rows but stop after the WAL commit: the data file
        // is NOT updated, and the instance is then destroyed ("power cut").
        // Note: no exec() may run on this instance after the simulated crash.
        // A normal auto-commit would checkpoint/reset the WAL and defeat the
        // demo, so the row count is taken in memory before the crash.
        std::string sql200 = "INSERT INTO wal_demo VALUES ";
        for (int i = 501; i <= 700; ++i) {
            if (i > 501) sql200 += ",";
            sql200 += "(" + std::to_string(i) + ", 'row-" + std::to_string(i) + "')";
        }
        {
            auto stmts = parse_sql(sql200);
            if (stmts.size() != 1) throw RajSQLError("crash demo parse failed");
            // Run against the throwaway instance's own state, then stop its
            // commit protocol right after the WAL write.
            Executor executor(*db.impl_, /*crash_after_wal=*/true);
            executor.in_txn_scope = true;
            Result inserted = executor.run(*stmts[0]);
            if (!inserted.ok) throw RajSQLError("crash demo insert failed");

            auto count_stmts = parse_sql("SELECT COUNT(*) FROM wal_demo");
            Executor counter(*db.impl_, /*crash_after_wal=*/false);
            counter.in_txn_scope = true;
            Result counted = counter.run(*count_stmts[0]);
            before = counted.ok && !counted.rows.empty() &&
                             !counted.rows[0].empty()
                         ? counted.rows[0][0].int_v
                         : -1;

            db.impl_->pager.commit(/*simulate_crash_after_wal=*/true);
        }
        steps.push_back(
            "Inserted 200 more rows (commit stopped after the WAL write, "
            "(no checkpoint)");
        steps.push_back("SIMULATED POWER LOSS: process destroyed");
    }

    {
        Database db(path);  // constructor replays the WAL
        after = count_rows(db);
        steps.push_back("Reopened database; WAL replayed on startup");
    }

    bool recovered = before > 0 && after == before;
    steps.push_back(recovered
                        ? "Recovered: all " + std::to_string(after) +
                              " rows survived the crash"
                        : "FAILED: " + std::to_string(after) + " of " +
                              std::to_string(before) + " rows survived");

    std::string out = "{\"steps\":[";
    for (size_t i = 0; i < steps.size(); ++i) {
        if (i) out += ',';
        out += '"' + json_escape(steps[i]) + '"';
    }
    out += "],\"rowsBeforeCrash\":" + std::to_string(before) +
           ",\"rowsAfterRecovery\":" + std::to_string(after) +
           ",\"recovered\":" + (recovered ? "true" : "false") + "}";
    return out;
}

}  // namespace rajsql
