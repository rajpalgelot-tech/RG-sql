// RajSQL: value.hpp
// A dynamically-typed SQL value (NULL / INTEGER / REAL / TEXT) with
// serialization for on-disk row storage and JSON output for the API layer.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rajsql {

enum class ColumnType { Integer, Real, Text };

struct Value {
    enum class Tag { Null, Int, Real, Text };

    Tag tag{Tag::Null};
    int64_t int_v{0};
    double real_v{0.0};
    std::string text_v;

    Value() = default;
    static Value null() { return Value{}; }
    static Value make_int(int64_t v) {
        Value out;
        out.tag = Tag::Int;
        out.int_v = v;
        return out;
    }
    static Value make_real(double v) {
        Value out;
        out.tag = Tag::Real;
        out.real_v = v;
        return out;
    }
    static Value make_text(std::string v) {
        Value out;
        out.tag = Tag::Text;
        out.text_v = std::move(v);
        return out;
    }

    bool is_null() const { return tag == Tag::Null; }
    bool is_numeric() const { return tag == Tag::Int || tag == Tag::Real; }

    // Total ordering used by ORDER BY and index comparisons:
    // NULL < INTEGER < REAL < TEXT (text compares byte-wise).
    int compare(const Value& other) const;

    // Canonical SQL-ish display form ("NULL", 42, 3.5, 'hello').
    std::string to_display_string() const;

    // JSON encoding used by the C/WASM API.
    std::string to_json() const;
};

// ── Row serialization ──────────────────────────────────────────────────────
// A row is a sequence of Values. On disk each cell value is encoded as:
//   [tag:u8][int:8 bytes | real:8 bytes | textLen:u16 + bytes]

std::vector<uint8_t> serialize_row(const std::vector<Value>& row);
std::vector<Value> deserialize_row(const std::vector<uint8_t>& data);

// Coerce a literal into the declared column type (INSERT/UPDATE paths).
// Throws RajSQLError on impossible conversions.
struct RajSQLError {
    std::string message;
    explicit RajSQLError(std::string msg) : message(std::move(msg)) {}
};
Value coerce_value(const Value& v, ColumnType target, const std::string& column);

}  // namespace rajsql
