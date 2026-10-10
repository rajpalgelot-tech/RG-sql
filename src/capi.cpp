// RajSQL: capi.cpp
// Minimal C ABI over the engine, consumed by the WebAssembly bindings and
// usable from any FFI. All returned strings point at a static buffer that
// stays valid until the next call on ANY handle (documented contract).

#include <cstring>
#include <map>
#include <memory>
#include <string>

#include "database.hpp"

namespace {

std::map<int32_t, std::unique_ptr<rajsql::Database>>& databases() {
    static std::map<int32_t, std::unique_ptr<rajsql::Database>> g_dbs;
    return g_dbs;
}

int32_t g_next_handle = 1;

std::string& result_buffer() {
    static std::string g_buf;
    return g_buf;
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

}  // namespace

extern "C" {

// Opens (creating if needed) a database file. Returns a positive handle or
// -1 on failure.
int32_t rsql_open(const char* path) {
    try {
        int32_t handle = g_next_handle++;
        databases()[handle] =
            std::make_unique<rajsql::Database>(path ? path : "rajsql.db");
        return handle;
    } catch (const std::exception& e) {
        result_buffer() = "{\"error\":\"" + json_escape(e.what()) + "\"}";
        return -1;
    }
}

void rsql_close(int32_t handle) { databases().erase(handle); }

// Executes SQL (may contain several ';'-separated statements). Returns a
// JSON array with one object per statement (see database.hpp for the shape).
const char* rsql_exec(int32_t handle, const char* sql) {
    auto& out = result_buffer();
    auto it = databases().find(handle);
    if (!sql || it == databases().end()) {
        out = "[{\"ok\":false,\"error\":\"invalid handle\"}]";
        return out.c_str();
    }
    try {
        auto results = it->second->exec(sql);
        out = "[";
        for (size_t i = 0; i < results.size(); ++i) {
            if (i) out += ',';
            out += results[i].to_json();
        }
        out += "]";
        if (results.empty()) out = "[]";
    } catch (const std::exception& e) {
        out = "[{\"ok\":false,\"error\":\"" + json_escape(e.what()) + "\"}]";
    }
    return out.c_str();
}

// JSON description of the current schema.
const char* rsql_schema(int32_t handle) {
    auto& out = result_buffer();
    auto it = databases().find(handle);
    if (it == databases().end()) {
        out = "{\"error\":\"invalid handle\"}";
        return out.c_str();
    }
    try {
        out = it->second->schema_json();
    } catch (const std::exception& e) {
        out = "{\"error\":\"" + json_escape(e.what()) + "\"}";
    }
    return out.c_str();
}

// Runs the crash-recovery demo; returns its JSON narrative.
const char* rsql_crash_demo(int32_t handle) {
    auto& out = result_buffer();
    auto it = databases().find(handle);
    if (it == databases().end()) {
        out = "{\"error\":\"invalid handle\"}";
        return out.c_str();
    }
    try {
        out = it->second->crash_demo_json();
    } catch (const std::exception& e) {
        out = "{\"error\":\"" + json_escape(e.what()) + "\"}";
    }
    return out.c_str();
}

}  // extern "C"
