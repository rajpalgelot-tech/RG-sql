// RajSQL: wal.hpp
// Append-only write-ahead log. Record format (little-endian):
//   [type: u8][payload...]
//     type 1 = PAGE : [pageNum: u32][bytes: kPageSize]
//     type 2 = COMMIT (no payload)
// A transaction is a run of PAGE records terminated by a COMMIT record.
// Recovery applies only fully-committed runs and then truncates the log.

#pragma once

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace rajsql {

class WAL {
public:
    explicit WAL(std::string path) : path_(std::move(path)) {}

    // Replays committed transactions, writing each page into `data_file`.
    // Returns the number of pages recovered. Truncates the log afterwards.
    // `data_file` must be opened read/write and positioned arbitrarily; it is
    // flushed by this call.
    size_t recover(std::fstream& data_file);

    // Appends records. buffered until flush().
    void log_page(uint32_t page_num, const std::vector<uint8_t>& bytes);
    void log_commit();
    void flush();

    // Discard the log (after a successful checkpoint).
    void reset();

    bool empty() const { return buffer_.empty(); }

private:
    void append_u32(uint32_t v);

    std::string path_;
    std::vector<uint8_t> buffer_;
};

}  // namespace rajsql
