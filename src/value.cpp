// RajSQL: value.cpp

#include "value.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <stdexcept>

namespace rajsql {

int Value::compare(const Value& other) const {
    auto rank = [](Tag t) {
        switch (t) {
            case Tag::Null: return 0;
            case Tag::Int: return 1;
            case Tag::Real: return 2;
            case Tag::Text: return 3;
        }
        return 0;
    };
    if (tag != other.tag) {
        int ra = rank(tag), rb = rank(other.tag);
        if (ra != rb) return ra < rb ? -1 : 1;
    }
    switch (tag) {
        case Tag::Null: return 0;
        case Tag::Int:
            if (int_v == other.int_v) return 0;
            return int_v < other.int_v ? -1 : 1;
        case Tag::Real:
            if (real_v == other.real_v) return 0;
            return real_v < other.real_v ? -1 : 1;
        case Tag::Text: return text_v.compare(other.text_v);
    }
    return 0;
}

std::string Value::to_display_string() const {
    switch (tag) {
        case Tag::Null: return "NULL";
        case Tag::Int: return std::to_string(int_v);
        case Tag::Real: {
            std::string s = std::to_string(real_v);
            // std::to_string always emits 6 decimals; trim trailing zeros.
            auto dot = s.find('.');
            if (dot != std::string::npos) {
                auto last = s.find_last_not_of('0');
                if (last == dot) last = dot - 1;
                s.erase(last + 1);
            }
            return s;
        }
        case Tag::Text: return text_v;
    }
    return "";
}

static std::string json_escape(const std::string& in) {
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

std::string Value::to_json() const {
    switch (tag) {
        case Tag::Null: return "null";
        case Tag::Int: return std::to_string(int_v);
        case Tag::Real: {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.17g", real_v);
            return buf;
        }
        case Tag::Text: return "\"" + json_escape(text_v) + "\"";
    }
    return "null";
}

void write_u16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xff));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
}

void write_u64(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i)
        out.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xff));
}

uint16_t read_u16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint64_t read_u64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (i * 8);
    return v;
}

std::vector<uint8_t> serialize_row(const std::vector<Value>& row) {
    std::vector<uint8_t> out;
    out.reserve(64);
    for (const Value& v : row) {
        switch (v.tag) {
            case Value::Tag::Null:
                out.push_back(0);
                break;
            case Value::Tag::Int:
                out.push_back(1);
                write_u64(out, static_cast<uint64_t>(v.int_v));
                break;
            case Value::Tag::Real:
                out.push_back(2);
                write_u64(out, 0);
                std::memcpy(out.data() + out.size() - 8, &v.real_v, 8);
                break;
            case Value::Tag::Text: {
                out.push_back(3);
                if (v.text_v.size() > 65535)
                    throw RajSQLError("text value too large (max 65535 bytes)");
                write_u16(out, static_cast<uint16_t>(v.text_v.size()));
                out.insert(out.end(), v.text_v.begin(), v.text_v.end());
                break;
            }
        }
    }
    return out;
}

std::vector<Value> deserialize_row(const std::vector<uint8_t>& data) {
    std::vector<Value> row;
    size_t pos = 0;
    while (pos < data.size()) {
        uint8_t tag = data[pos++];
        switch (tag) {
            case 0: row.push_back(Value::null()); break;
            case 1: {
                if (pos + 8 > data.size()) throw RajSQLError("corrupt row: truncated int");
                row.push_back(Value::make_int(static_cast<int64_t>(read_u64(data.data() + pos))));
                pos += 8;
                break;
            }
            case 2: {
                if (pos + 8 > data.size()) throw RajSQLError("corrupt row: truncated real");
                double d;
                std::memcpy(&d, data.data() + pos, 8);
                row.push_back(Value::make_real(d));
                pos += 8;
                break;
            }
            case 3: {
                if (pos + 2 > data.size()) throw RajSQLError("corrupt row: truncated text");
                uint16_t len = read_u16(data.data() + pos);
                pos += 2;
                if (pos + len > data.size()) throw RajSQLError("corrupt row: text overruns page");
                row.push_back(Value::make_text(
                    std::string(reinterpret_cast<const char*>(data.data() + pos), len)));
                pos += len;
                break;
            }
            default:
                throw RajSQLError("corrupt row: unknown value tag " +
                                  std::to_string(static_cast<int>(tag)));
        }
    }
    return row;
}

Value coerce_value(const Value& v, ColumnType target, const std::string& column) {
    if (v.is_null()) return Value::null();
    switch (target) {
        case ColumnType::Integer:
            if (v.tag == Value::Tag::Int) return v;
            throw RajSQLError("cannot store " + v.to_display_string() +
                              " in INTEGER column '" + column + "'");
        case ColumnType::Real:
            if (v.tag == Value::Tag::Real) return v;
            if (v.tag == Value::Tag::Int) return Value::make_real(static_cast<double>(v.int_v));
            throw RajSQLError("cannot store " + v.to_display_string() +
                              " in REAL column '" + column + "'");
        case ColumnType::Text:
            if (v.tag == Value::Tag::Text) return v;
            throw RajSQLError("cannot store " + v.to_display_string() +
                              " in TEXT column '" + column + "'");
    }
    return v;
}

}  // namespace rajsql
