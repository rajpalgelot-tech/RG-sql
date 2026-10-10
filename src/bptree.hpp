// RajSQL: bptree.hpp
// A disk-backed B+tree. One tree per table (keyed by rowid) plus one for
// the catalog. Leaves hold the row payloads and are chained left→right for
// full scans; interior nodes route by key.
//
// Interior semantics (min-key routing): each interior cell stores
//   {childPage, minKey} = the smallest key reachable through that child,
// plus one implicit rightmost child covering keys ≥ the last cell's min.
// This keeps inserts append-friendly: growing a leaf's maximum never
// invalidates a stored separator.
//
// Page layouts (little-endian, 4096-byte pages):
//   leaf:     [type:u8 = 1][cellCount:u16][nextLeaf:u32][cells...]
//   interior: [type:u8 = 2][cellCount:u16][rightmostChild:u32][cells...]
//   interior cell: [childPage:u32][minKey:i64]
//   leaf cell:     [key:i64][payloadLen:u16][tombstone:u8][payload bytes]
//
// v1 scope (documented in the README): inserts split nodes bottom-up;
// deletes are tombstones (no rebalancing); a row must fit in one page.

#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "pager.hpp"

namespace rajsql {

class BPTree {
public:
    using Path = std::vector<std::pair<uint32_t, size_t>>;  // (page, child index)

    // Creates a new empty tree (a fresh leaf root) and returns its page id.
    static uint32_t create(Pager& pager);

    BPTree(Pager& pager, uint32_t root_page) : pager_(pager), root_(root_page) {}

    // Root page id. This can change when a split grows the tree; the database
    // layer persists this back into the catalog after mutations.
    uint32_t root() const { return root_; }

    // Inserts (or replaces) `key` → `payload`. Returns true when a new key
    // was inserted, false when an existing live entry was overwritten.
    bool upsert(int64_t key, const std::vector<uint8_t>& payload);

    // Marks `key` deleted (tombstone). Returns false if absent.
    bool erase(int64_t key);

    // Looks up one live entry.
    bool find(int64_t key, std::vector<uint8_t>& out) const;

    // Full scan in key order, skipping tombstones.
    void scan(std::vector<std::pair<int64_t, std::vector<uint8_t>>>& out) const;

    // Number of live entries (full scan; fine at demo scale).
    size_t count_live() const;

private:
    static constexpr uint16_t LEAF_HEADER = 1 + 2 + 4;   // type + count + next
    static constexpr uint16_t INTERIOR_HEADER = 1 + 2 + 4;
    static constexpr uint16_t LEAF_CELL_HEADER = 8 + 2 + 1;  // key + len + tomb
    static constexpr uint16_t INTERIOR_CELL = 4 + 8;         // child + minKey

    struct LeafCell {
        int64_t key;
        bool deleted;
        std::vector<uint8_t> payload;
    };
    struct InteriorCell {
        uint32_t child;
        int64_t min_key;
    };

    static bool leaf_fits(size_t cell_bytes) {
        return LEAF_HEADER + cell_bytes <= Pager::kPageSize;
    }
    static bool interior_fits(size_t cells) {
        return INTERIOR_HEADER + cells * INTERIOR_CELL + 4 <= Pager::kPageSize;
    }

    // ── page access ────────────────────────────────────────────────────
    uint8_t page_type(uint32_t page) const { return pager_.read_page(page)[0]; }
    static uint16_t read_u16_at(const uint8_t* p);
    static void write_u16_at(uint8_t* p, uint16_t v);
    static uint32_t read_u32_at(const uint8_t* p);
    static void write_u32_at(uint8_t* p, uint32_t v);
    static int64_t read_i64_at(const uint8_t* p);
    static void write_i64_at(uint8_t* p, int64_t v);

    std::vector<LeafCell> read_leaf(uint32_t page) const;
    void write_leaf(uint32_t page, const std::vector<LeafCell>& cells, uint32_t next);
    std::vector<InteriorCell> read_interior(uint32_t page) const;
    void write_interior(uint32_t page, const std::vector<InteriorCell>& cells,
                        uint32_t rightmost);

    uint32_t leaf_next(uint32_t page) const {
        return read_u32_at(pager_.read_page(page).data() + 3);
    }
    uint32_t interior_rightmost(uint32_t page) const {
        return read_u32_at(pager_.read_page(page).data() + 3);
    }

    uint32_t find_leaf(int64_t key, Path& path) const;

    uint32_t split_leaf(uint32_t page, const Path& path);
    uint32_t split_interior(uint32_t page, const Path& path);
    void insert_in_parent(uint32_t left_page, int64_t left_min,
                          uint32_t right_page, int64_t right_min,
                          const Path& path);

    Pager& pager_;
    uint32_t root_;
};

}  // namespace rajsql
