// RajSQL: pager.cpp

#include "pager.hpp"

#include <cstring>
#include <stdexcept>

namespace rajsql {

static const char MAGIC[8] = {'R', 'S', 'Q', 'D', 'B', 'v', '0', '1'};

Pager::Pager(const std::string& path)
    : path_(path), wal_path_(path + ".wal"), wal_(wal_path_) {
    file_.open(path_, std::ios::in | std::ios::out | std::ios::binary);
    if (!file_.is_open()) {
        // create then reopen
        std::fstream create(path_, std::ios::out | std::ios::binary);
        create.close();
        file_.open(path_, std::ios::in | std::ios::out | std::ios::binary);
        if (!file_.is_open())
            throw RajSQLError("cannot open database file '" + path_ + "'");
    }
    load_or_init();
    recover_from_wal();
    // Recovery may have rewritten pages that extend the file (allocated but
    // never checkpointed); reload the header so page_count_ is current.
    load_or_init();
}

void Pager::load_or_init() {
    file_.seekg(0, std::ios::end);
    std::streamoff size = file_.tellg();
    if (size == 0) {
        // Fresh database: the header page is written on the first commit.
        page_count_ = 1;
        catalog_root_ = 0;  // no catalog tree yet
        next_table_id_ = 1;
        persist_header();
        return;
    }
    file_.seekg(0);
    char magic[8];
    file_.read(magic, 8);
    if (file_.gcount() != 8 || std::memcmp(magic, MAGIC, 8) != 0)
        throw RajSQLError("file '" + path_ + "' is not a RajSQL database");
    uint32_t vals[3];
    file_.read(reinterpret_cast<char*>(vals), sizeof(vals));
    if (file_.gcount() != sizeof(vals))
        throw RajSQLError("truncated database header");
    page_count_ = vals[0];
    catalog_root_ = vals[1];
    next_table_id_ = vals[2];
}

void Pager::recover_from_wal() {
    size_t recovered = wal_.recover(file_);
    (void)recovered;
}

const std::vector<uint8_t>& Pager::read_page(uint32_t page_id) {
    if (page_id >= page_count_)
        throw RajSQLError("page id " + std::to_string(page_id) + " out of range");
    auto it = cache_.find(page_id);
    if (it == cache_.end()) {
        Slot slot;
        slot.data.resize(kPageSize);
        file_.clear();
        file_.seekg(static_cast<std::streamoff>(page_id) * kPageSize);
        file_.read(reinterpret_cast<char*>(slot.data.data()), kPageSize);
        if (file_.gcount() != static_cast<std::streamsize>(kPageSize)) {
            // extend file lazily (fresh pages beyond EOF)
            for (auto& b : slot.data) b = 0;
        }
        it = cache_.emplace(page_id, std::move(slot)).first;
    }
    return it->second.data;
}

std::vector<uint8_t>& Pager::mutate_page(uint32_t page_id) {
    (void)read_page(page_id);  // ensure cached
    dirty_[page_id] = true;
    if (page_id == 0) header_dirty_ = true;
    return cache_[page_id].data;
}

uint32_t Pager::alloc_page() {
    uint32_t id = page_count_++;
    cache_[id] = Slot{std::vector<uint8_t>(kPageSize, 0)};
    dirty_[id] = true;
    return id;
}

void Pager::write_page_file(uint32_t page_id, const std::vector<uint8_t>& data) {
    file_.clear();
    file_.seekp(static_cast<std::streamoff>(page_id) * kPageSize);
    file_.write(reinterpret_cast<const char*>(data.data()), kPageSize);
}

void Pager::persist_header() {
    std::vector<uint8_t> header = cache_.count(0) ? cache_[0].data
                                                   : std::vector<uint8_t>(kPageSize, 0);
    std::memcpy(header.data(), MAGIC, 8);
    uint32_t vals[3] = {page_count_, catalog_root_, next_table_id_};
    std::memcpy(header.data() + 8, vals, sizeof(vals));
    cache_[0] = Slot{std::move(header)};
    dirty_[0] = true;  // header must be logged + checkpointed too
}

void Pager::commit(bool simulate_crash_after_wal) {
    if (dirty_.empty() && !header_dirty_) return;  // nothing to durably record

    // Rebuild header page first so it is logged together with data pages.
    persist_header();

    if (!dirty_.empty()) {
        for (const auto& [id, _] : dirty_) {
            wal_.log_page(id, cache_[id].data);
        }
        wal_.log_commit();
        wal_.flush();  // WAL is now the durable source of truth

        if (simulate_crash_after_wal) {
            // Simulate a power cut between WAL commit and checkpoint: leave
            // the committed log on disk, drop the in-memory dirty set so
            // nothing else gets checkpointed, and let the caller destroy the
            // database. The next open() recovers from the WAL.
            dirty_.clear();
            return;
        }

        for (const auto& [id, _] : dirty_) {
            write_page_file(id, cache_[id].data);
        }
        file_.flush();
        dirty_.clear();
    }
    wal_.reset();
    header_dirty_ = false;
}

void Pager::rollback() {
    drop_cache();
    dirty_.clear();
    header_dirty_ = false;
    wal_.reset();
    load_or_init();  // re-read header from disk
}

void Pager::drop_cache() {
    cache_.clear();
}

}  // namespace rajsql
