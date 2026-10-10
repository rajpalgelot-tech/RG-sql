// RajSQL: bptree.cpp

#include "bptree.hpp"

#include <algorithm>
#include <cstring>

namespace rajsql {

// ── fixed-width encoding helpers ───────────────────────────────────────────

uint16_t BPTree::read_u16_at(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
void BPTree::write_u16_at(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xff);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xff);
}
uint32_t BPTree::read_u32_at(const uint8_t* p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(p[i]) << (i * 8);
    return v;
}
void BPTree::write_u32_at(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>((v >> (i * 8)) & 0xff);
}
int64_t BPTree::read_i64_at(const uint8_t* p) {
    int64_t v;
    std::memcpy(&v, p, 8);
    return v;
}
void BPTree::write_i64_at(uint8_t* p, int64_t v) { std::memcpy(p, &v, 8); }

// ── page serialization ─────────────────────────────────────────────────────

std::vector<BPTree::LeafCell> BPTree::read_leaf(uint32_t page) const {
    const std::vector<uint8_t>& data = pager_.read_page(page);
    uint16_t count = read_u16_at(data.data() + 1);
    std::vector<LeafCell> cells;
    cells.reserve(count);
    size_t pos = LEAF_HEADER;
    for (uint16_t i = 0; i < count; ++i) {
        if (pos + LEAF_CELL_HEADER > Pager::kPageSize)
            throw RajSQLError("corrupt leaf page " + std::to_string(page));
        LeafCell cell;
        cell.key = read_i64_at(data.data() + pos);
        uint16_t len = read_u16_at(data.data() + pos + 8);
        cell.deleted = data[pos + 10] != 0;
        pos += LEAF_CELL_HEADER;
        if (pos + len > Pager::kPageSize)
            throw RajSQLError("corrupt leaf cell on page " + std::to_string(page));
        cell.payload.assign(data.begin() + pos, data.begin() + pos + len);
        pos += len;
        cells.push_back(std::move(cell));
    }
    return cells;
}

void BPTree::write_leaf(uint32_t page, const std::vector<LeafCell>& cells,
                        uint32_t next) {
    size_t cell_bytes = 0;
    for (const auto& c : cells) cell_bytes += LEAF_CELL_HEADER + c.payload.size();
    if (!leaf_fits(cell_bytes))
        throw RajSQLError("leaf page overflow: " + std::to_string(cell_bytes) +
                          " bytes of cells");
    std::vector<uint8_t>& data = pager_.mutate_page(page);
    std::fill(data.begin(), data.end(), 0);
    data[0] = 1;  // leaf
    write_u16_at(data.data() + 1, static_cast<uint16_t>(cells.size()));
    write_u32_at(data.data() + 3, next);
    size_t pos = LEAF_HEADER;
    for (const auto& c : cells) {
        write_i64_at(data.data() + pos, c.key);
        write_u16_at(data.data() + pos + 8, static_cast<uint16_t>(c.payload.size()));
        data[pos + 10] = c.deleted ? 1 : 0;
        std::memcpy(data.data() + pos + 11, c.payload.data(), c.payload.size());
        pos += LEAF_CELL_HEADER + c.payload.size();
    }
}

std::vector<BPTree::InteriorCell> BPTree::read_interior(uint32_t page) const {
    const std::vector<uint8_t>& data = pager_.read_page(page);
    uint16_t count = read_u16_at(data.data() + 1);
    std::vector<InteriorCell> cells;
    cells.reserve(count);
    size_t pos = INTERIOR_HEADER;
    for (uint16_t i = 0; i < count; ++i) {
        if (pos + INTERIOR_CELL > Pager::kPageSize)
            throw RajSQLError("corrupt interior page " + std::to_string(page));
        InteriorCell cell;
        cell.child = read_u32_at(data.data() + pos);
        cell.min_key = read_i64_at(data.data() + pos + 4);
        pos += INTERIOR_CELL;
        cells.push_back(cell);
    }
    return cells;
}

void BPTree::write_interior(uint32_t page, const std::vector<InteriorCell>& cells,
                            uint32_t rightmost) {
    if (!interior_fits(cells.size()))
        throw RajSQLError("interior page overflow");
    std::vector<uint8_t>& data = pager_.mutate_page(page);
    std::fill(data.begin(), data.end(), 0);
    data[0] = 2;  // interior
    write_u16_at(data.data() + 1, static_cast<uint16_t>(cells.size()));
    write_u32_at(data.data() + 3, rightmost);
    size_t pos = INTERIOR_HEADER;
    for (const auto& c : cells) {
        write_u32_at(data.data() + pos, c.child);
        write_i64_at(data.data() + pos + 4, c.min_key);
        pos += INTERIOR_CELL;
    }
}

// ── descent ─────────────────────────────────────────────────────────────────

uint32_t BPTree::find_leaf(int64_t key, Path& path) const {
    path.clear();
    uint32_t page = root_;
    while (page_type(page) == 2) {
        auto cells = read_interior(page);
        // first cell whose min_key exceeds `key`
        size_t i = 0;
        while (i < cells.size() && key >= cells[i].min_key) ++i;
        uint32_t child;
        size_t taken;
        if (i == cells.size()) {
            child = interior_rightmost(page);
            taken = cells.size();
        } else if (i == 0) {
            child = cells[0].child;
            taken = 0;
        } else {
            child = cells[i - 1].child;
            taken = i - 1;
        }
        path.emplace_back(page, taken);
        page = child;
    }
    return page;
}

// ── public operations ──────────────────────────────────────────────────────

uint32_t BPTree::create(Pager& pager) {
    uint32_t page = pager.alloc_page();
    pager.mutate_page(page)[0] = 1;  // zeroed page → empty leaf
    return page;
}

bool BPTree::upsert(int64_t key, const std::vector<uint8_t>& payload) {
    Path path;
    uint32_t leaf = find_leaf(key, path);
    auto cells = read_leaf(leaf);

    auto it = std::lower_bound(cells.begin(), cells.end(), key,
                               [](const LeafCell& c, int64_t k) { return c.key < k; });

    bool is_new;
    if (it != cells.end() && it->key == key) {
        is_new = it->deleted;  // replacing a tombstone inserts a live row
        it->deleted = false;
        it->payload = payload;
    } else {
        is_new = true;
        cells.insert(it, LeafCell{key, false, payload});
    }

    size_t cell_bytes = 0;
    for (const auto& c : cells) cell_bytes += LEAF_CELL_HEADER + c.payload.size();
    if (leaf_fits(cell_bytes)) {
        write_leaf(leaf, cells, leaf_next(leaf));
        return is_new;
    }

    // Overflow: split what is currently on disk, then re-descend and retry.
    if (cells.size() < 2)
        throw RajSQLError("row too large for one page (max ~" +
                          std::to_string(Pager::kPageSize - LEAF_HEADER -
                                         LEAF_CELL_HEADER) + " bytes of payload)");
    split_leaf(leaf, path);
    return upsert(key, payload);
}

bool BPTree::erase(int64_t key) {
    Path path;
    uint32_t leaf = find_leaf(key, path);
    auto cells = read_leaf(leaf);
    for (auto& c : cells) {
        if (c.key == key) {
            if (c.deleted) return false;
            c.deleted = true;
            write_leaf(leaf, cells, leaf_next(leaf));
            return true;
        }
        if (c.key > key) break;
    }
    return false;
}

bool BPTree::find(int64_t key, std::vector<uint8_t>& out) const {
    Path path;
    uint32_t leaf = find_leaf(key, path);
    for (const auto& c : read_leaf(leaf)) {
        if (c.key == key) {
            if (c.deleted) return false;
            out = c.payload;
            return true;
        }
        if (c.key > key) break;
    }
    return false;
}

void BPTree::scan(std::vector<std::pair<int64_t, std::vector<uint8_t>>>& out) const {
    uint32_t page = root_;
    while (page_type(page) == 2) {
        auto cells = read_interior(page);
        page = cells.empty() ? interior_rightmost(page) : cells.front().child;
    }
    while (page != 0) {
        for (const auto& c : read_leaf(page)) {
            if (!c.deleted) out.emplace_back(c.key, c.payload);
        }
        page = leaf_next(page);
    }
}

size_t BPTree::count_live() const {
    std::vector<std::pair<int64_t, std::vector<uint8_t>>> all;
    scan(all);
    return all.size();
}

// ── splits ──────────────────────────────────────────────────────────────────

uint32_t BPTree::split_leaf(uint32_t page, const Path& path) {
    auto cells = read_leaf(page);
    if (cells.size() < 2)
        throw RajSQLError("cannot split a leaf with fewer than 2 cells");
    size_t mid = cells.size() / 2;

    std::vector<LeafCell> left_cells(cells.begin(), cells.begin() + mid);
    std::vector<LeafCell> right_cells(cells.begin() + mid, cells.end());
    uint32_t old_next = leaf_next(page);
    uint32_t right = pager_.alloc_page();

    write_leaf(page, left_cells, right);
    write_leaf(right, right_cells, old_next);

    insert_in_parent(page, left_cells.front().key, right,
                     right_cells.front().key, path);
    return right;
}

uint32_t BPTree::split_interior(uint32_t page, const Path& path) {
    auto cells = read_interior(page);
    if (cells.size() < 2)
        throw RajSQLError("cannot split an interior node with fewer than 2 cells");
    size_t mid = cells.size() / 2;

    // Left keeps children c0..c(mid-2) plus rightmost = c(mid-1).child.
    // Right keeps children c(mid)..c(n-1) plus the old rightmost.
    std::vector<InteriorCell> left_cells(cells.begin(), cells.begin() + (mid - 1));
    std::vector<InteriorCell> right_cells(cells.begin() + mid, cells.end());
    uint32_t left_rightmost = cells[mid - 1].child;
    uint32_t right_rightmost = interior_rightmost(page);
    uint32_t right = pager_.alloc_page();

    write_interior(page, left_cells, left_rightmost);
    write_interior(right, right_cells, right_rightmost);

    insert_in_parent(page, cells.front().min_key, right, cells[mid].min_key, path);
    return right;
}

void BPTree::insert_in_parent(uint32_t left_page, int64_t left_min,
                              uint32_t right_page, int64_t right_min,
                              const Path& path) {
    if (path.empty()) {
        // The split node was the root: grow the tree by one level.
        uint32_t new_root = pager_.alloc_page();
        write_interior(new_root, {InteriorCell{left_page, left_min}}, right_page);
        root_ = new_root;
        return;
    }

    auto [parent, taken] = path.back();
    auto cells = read_interior(parent);
    uint32_t rightmost = interior_rightmost(parent);

    if (taken < cells.size()) {
        // left_page sits at `taken`; slot the new sibling in right after it.
        cells.insert(cells.begin() + taken + 1, InteriorCell{right_page, right_min});
    } else {
        // left_page was the rightmost child: it becomes a regular cell and
        // the new sibling takes the rightmost slot.
        cells.push_back(InteriorCell{left_page, left_min});
        rightmost = right_page;
    }
    write_interior(parent, cells, rightmost);

    if (!interior_fits(cells.size())) {
        Path parent_path(path.begin(), path.end() - 1);
        split_interior(parent, parent_path);
    }
}

}  // namespace rajsql
