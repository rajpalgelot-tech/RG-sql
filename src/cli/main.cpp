// RajSQL: cli/main.cpp
// Interactive REPL front-end. Usage: rajsql [dbfile]
//
// Reads lines from stdin, accumulating until a statement is terminated by
// ';', then executes via Database::exec() and pretty-prints the results
// as ASCII tables.

#include <cstdio>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

#include "../database.hpp"
#include "../value.hpp"

namespace {

// ── ASCII table rendering ──────────────────────────────────────────────────

std::string pad_right(const std::string& s, size_t width) {
    if (s.size() >= width) return s;
    return s + std::string(width - s.size(), ' ');
}

// One Result per statement. SELECT gets a table, non-SELECT gets its
// message + rowsAffected, errors go to stderr.
void print_result(const rajsql::Result& r) {
    if (!r.ok) {
        std::cerr << "Error: " << r.error << "\n";
        return;
    }

    if (!r.columns.empty()) {
        // Column widths: header vs every cell.
        std::vector<size_t> widths(r.columns.size(), 0);
        for (size_t c = 0; c < r.columns.size(); ++c)
            widths[c] = r.columns[c].size();
        for (const auto& row : r.rows) {
            for (size_t c = 0; c < row.size() && c < widths.size(); ++c) {
                size_t len = row[c].to_display_string().size();
                if (len > widths[c]) widths[c] = len;
            }
        }

        // Header: " id | name | gpa"
        std::string head = " ";
        for (size_t c = 0; c < r.columns.size(); ++c) {
            if (c > 0) head += " | ";
            head += pad_right(r.columns[c], widths[c]);
        }
        std::cout << head << "\n";

        // Separator: "----+-----+------"
        std::string sep;
        for (size_t c = 0; c < widths.size(); ++c) {
            if (c > 0) sep += "+";
            sep += std::string(widths[c] + 2, '-');
        }
        std::cout << sep << "\n";

        for (const auto& row : r.rows) {
            std::string line = " ";
            for (size_t c = 0; c < row.size() && c < widths.size(); ++c) {
                if (c > 0) line += " | ";
                line += pad_right(row[c].to_display_string(), widths[c]);
            }
            std::cout << line << "\n";
        }
    }

    size_t n = r.rows.size();
    std::cout << "(" << n << (n == 1 ? " row" : " rows");
    char buf[32];
    std::snprintf(buf, sizeof(buf), " · %.1f ms)\n", r.elapsed_ms);
    std::cout << buf;
}

void print_non_select(const rajsql::Result& r) {
    if (!r.message.empty()) std::cout << r.message << "\n";
    if (r.rows_affected > 0)
        std::cout << r.rows_affected << (r.rows_affected == 1 ? " row affected" : " rows affected")
                  << "\n";
}

void print_help() {
    std::cout
        << "Dot commands:\n"
        << "  .tables   list tables (JSON)\n"
        << "  .schema   print schema (JSON)\n"
        << "  .help     show this help\n"
        << "  .quit     exit (also .exit)\n"
        << "SQL statements end with ';'.\n";
}

bool ends_with_semicolon(const std::string& s) {
    // Trim trailing whitespace, then look for ';'.
    size_t end = s.find_last_not_of(" \t\r\n");
    if (end == std::string::npos) return false;
    return s[end] == ';';
}

}  // namespace

int main(int argc, char** argv) {
    std::string path = (argc > 1) ? argv[1] : "rajsql.db";

    rajsql::Database* db = nullptr;
    try {
        db = new rajsql::Database(path);
    } catch (const rajsql::RajSQLError& e) {
        std::cerr << "Error: failed to open database '" << path << "': " << e.message << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error: failed to open database '" << path << "': " << e.what() << "\n";
        return 1;
    }

    std::cout << "RajSQL: enter '.help' for commands. Database: " << path << "\n";

    std::string pending;  // accumulated SQL across lines
    std::string line;
    while (true) {
        std::cout << "rajsql> " << std::flush;
        if (!std::getline(std::cin, line)) break;  // EOF: exit cleanly

        if (pending.empty() && !line.empty() && line[0] == '.') {
            // Dot-commands run immediately, no ';' needed.
            std::string cmd = line;
            // Trim trailing whitespace/CR.
            size_t end = cmd.find_last_not_of(" \t\r\n");
            if (end != std::string::npos) cmd = cmd.substr(0, end + 1);
            if (cmd == ".quit" || cmd == ".exit") break;
            if (cmd == ".tables" || cmd == ".schema") {
                try {
                    std::cout << db->schema_json() << "\n";
                } catch (const std::exception& e) {
                    std::cerr << "Error: " << e.what() << "\n";
                }
            } else if (cmd == ".help") {
                print_help();
            } else {
                std::cerr << "Unknown command: " << cmd << " (try .help)\n";
            }
            continue;
        }

        pending += line;
        pending += '\n';
        if (!ends_with_semicolon(line)) continue;  // keep accumulating

        try {
            for (const auto& r : db->exec(pending)) {
                if (r.columns.empty())
                    print_non_select(r);
                else
                    print_result(r);
            }
        } catch (const rajsql::RajSQLError& e) {
            std::cerr << "Error: " << e.message << "\n";
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << "\n";
        }
        pending.clear();
    }

    delete db;
    return 0;
}
