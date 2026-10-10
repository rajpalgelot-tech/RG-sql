// RajSQL: pager.hpp
// The pager owns the database file. It splits the file into fixed 4 KiB
// pages, keeps an in-memory page cache with dirty tracking, and drives the
// write-ahead logging protocol on commit.
//
// Page 0 is the file header:
//   [magic "RSQDBv01": 8 bytes][pageCount: u32][catalogRoot: u32][nextTableId: u32]

#pragma once

#include <cstdint>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "value.hpp"
#include "wal.hpp"

namespace rajsql {

class Pager {
public:
    static constexpr uint32_t kPageSize = 4096;

    // Opens (or creates) the database at `path` and runs WAL recovery.
    explicit Pager(const std::string& path);

    uint32_t page_count() const { return page_count_; }
    const std::string& path() const { return path_; }
    uint32_t catalog_root() const { return catalog_root_; }
    uint32_t next_table_id() const { return next_table_id_; }
    void set_catalog_root(uint32_t root) {
        catalog_root_ = root;
        header_dirty_ = true;
    }
    void set_next_table_id(uint32_t id) {
        next_table_id_ = id;
        header_dirty_ = true;
    }

    // Reads a page into the cache and returns it (read-only).
    const std::vector<uint8_t>& read_page(uint32_t page_id);

    // Returns the page for mutation and marks it dirty.
    std::vector<uint8_t>& mutate_page(uint32_t page_id);

    // Appends a new zeroed page and returns its id.
    uint32_t alloc_page();

    // Durability protocol:
    //   1. append all dirty pages to the WAL
    //   2. append a COMMIT record (WAL is now authoritative)
    //   3. checkpoint: write dirty pages + header to the data file
    //   4. reset the WAL
    // When `simulate_crash_after_wal` is true, step 3+4 are skipped: the
    // process is expected to die right after, so the next open() must
    // recover from the WAL alone. (Used by the crash-recovery demo.)
    void commit(bool simulate_crash_after_wal = false);

    // Discards all uncommitted in-memory changes (ROLLBACK).
    void rollback();

    // True if there are uncommitted dirty pages.
    bool has_uncommitted() const { return !dirty_.empty() || header_dirty_; }

private:
    struct Slot {
        std::vector<uint8_t> data;
    };

    void load_or_init();
    void recover_from_wal();
    void write_page_file(uint32_t page_id, const std::vector<uint8_t>& data);
    void persist_header();
    void drop_cache();

    std::string path_;
    std::string wal_path_;
    std::fstream file_;
    uint32_t page_count_ = 0;
    uint32_t catalog_root_ = 0;
    uint32_t next_table_id_ = 1;
    bool header_dirty_ = false;
    std::map<uint32_t, Slot> cache_;
    std::map<uint32_t, bool> dirty_;
    WAL wal_;
};

}  // namespace rajsql
