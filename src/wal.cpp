// RajSQL: wal.cpp

#include "wal.hpp"

#include <cstring>

namespace rajsql {

static constexpr uint8_t REC_PAGE = 1;
static constexpr uint8_t REC_COMMIT = 2;
// Must equal Pager::kPageSize (pager.hpp owns the authoritative constant;
// the WAL cannot include it back without a dependency cycle).
static constexpr uint32_t walPageBytes = 4096;

void WAL::append_u32(uint32_t v) {
    for (int i = 0; i < 4; ++i)
        buffer_.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xff));
}

void WAL::log_page(uint32_t page_num, const std::vector<uint8_t>& bytes) {
    buffer_.push_back(REC_PAGE);
    append_u32(page_num);
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
}

void WAL::log_commit() {
    buffer_.push_back(REC_COMMIT);
}

void WAL::flush() {
    if (buffer_.empty()) return;
    std::fstream out(path_, std::ios::out | std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(buffer_.data()),
              static_cast<std::streamsize>(buffer_.size()));
    out.flush();
}

void WAL::reset() {
    buffer_.clear();
    std::ofstream(path_, std::ios::binary | std::ios::trunc).close();
}

size_t WAL::recover(std::fstream& data_file) {
    std::ifstream in(path_, std::ios::binary);
    if (!in.is_open()) return 0;
    std::vector<uint8_t> log((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
    if (log.empty()) return 0;

    // Walk the log; collect pages of the current (uncommitted) run.
    // Only apply a run when its COMMIT record is found at the end.
    size_t pos = 0;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> committed;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> current;

    while (pos < log.size()) {
        uint8_t type = log[pos++];
        if (type == REC_PAGE) {
            if (pos + 4 > log.size()) break;  // torn write
            uint32_t page =
                log[pos] | (log[pos + 1] << 8) | (log[pos + 2] << 16) |
                (static_cast<uint32_t>(log[pos + 3]) << 24);
            pos += 4;
            if (pos + walPageBytes > log.size()) break;  // torn write
            std::vector<uint8_t> bytes(log.begin() + pos,
                                       log.begin() + pos + walPageBytes);
            pos += walPageBytes;
            current.emplace_back(page, std::move(bytes));
        } else if (type == REC_COMMIT) {
            // Transaction completed: its pages become authoritative.
            for (auto& e : current) committed.push_back(std::move(e));
            current.clear();
        } else {
            break;  // corrupt record
        }
    }

    for (const auto& [page, bytes] : committed) {
        data_file.clear();
        data_file.seekp(static_cast<std::streamoff>(page) * walPageBytes);
        data_file.write(reinterpret_cast<const char*>(bytes.data()),
                        walPageBytes);
    }
    if (!committed.empty()) data_file.flush();
    reset();
    return committed.size();
}

}  // namespace rajsql
