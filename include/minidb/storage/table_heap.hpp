#pragma once

#include <cstdint>
#include <optional>
#include <utility>

#include "minidb/buffer/buffer_pool_manager.hpp"
#include "minidb/catalog/catalog.hpp"
#include "minidb/storage/record.hpp"
#include "minidb/storage/table_page.hpp"

namespace minidb {

/// The table's records, spread over a singly-linked chain of slotted pages.
///
/// Every page is reached through the BufferPoolManager, so the heap never sees
/// a file. Nothing is cached in RAM either: a scan reads one page at a time,
/// which is what lets the table be larger than memory.
///
/// Pages are never unlinked when they empty. An empty page stays in the chain
/// and is handed to the next insert, so deleting and reinserting reuses space
/// instead of growing the file — and no previous-page pointer is needed.
class TableHeap {
public:
    TableHeap(BufferPoolManager& pool, Catalog& catalog);

    /// Allocates and initialises the table's first page. Used by CREATE TABLE
    /// before the catalog exists.
    [[nodiscard]] static PageId CreateFirstPage(BufferPoolManager& pool);

    /// Stores a record and returns where it landed.
    [[nodiscard]] RecordId InsertRecord(const Record& record);

    /// Reads a record, or std::nullopt when the slot is free or out of range.
    [[nodiscard]] std::optional<Record> GetRecord(RecordId rid) const;

    /// Overwrites a record and returns its location afterwards.
    ///
    /// The returned RecordId differs from `rid` when the new version no longer
    /// fits in its page and had to be relocated. The caller must then update the
    /// index, or its entry would point at a record that is no longer there.
    [[nodiscard]] RecordId UpdateRecord(RecordId rid, const Record& record);

    /// Frees a record's slot. Returns false when it was already free.
    bool DeleteRecord(RecordId rid);

    /// Forward-only cursor over the live records of the table.
    ///
    /// The iterator holds no pinned page between calls: each step fetches,
    /// reads and unpins. Resident pages make that cheap, and it means an
    /// operator tree can never exhaust the pool by holding pages open.
    class Iterator {
    public:
        Iterator(const TableHeap& heap, PageId first_page_id)
            : heap_(&heap), page_id_(first_page_id) {}

        /// Returns the next record and its location, or std::nullopt at the end.
        [[nodiscard]] std::optional<std::pair<RecordId, Record>> Next();

    private:
        const TableHeap* heap_;
        PageId page_id_;
        SlotId slot_id_ = 0;
    };

    [[nodiscard]] Iterator Begin() const { return Iterator(*this, catalog_.FirstTablePageId()); }

    [[nodiscard]] PageId FirstPageId() const { return catalog_.FirstTablePageId(); }

    /// Visits every live record of one page and returns the next page in the
    /// chain, or kInvalidPageId at the end.
    ///
    /// The page is fetched **once**, whereas Iterator fetches it again for every
    /// record it returns. That is the whole reason a batch-at-a-time scan is
    /// cheaper: it pays one buffer pool round trip per page instead of one per
    /// record. The visitor form lets the caller fill its own container without an
    /// intermediate copy and without the storage layer knowing what that
    /// container is.
    template <typename Visitor>
    [[nodiscard]] PageId ForEachRecordInPage(PageId page_id, Visitor&& visit) const {
        if (page_id == kInvalidPageId) {
            return kInvalidPageId;
        }

        PageGuard guard = FetchGuarded(pool_, page_id);
        const TablePage page(guard.Data());
        const Schema& schema = catalog_.GetSchema();

        for (std::optional<SlotId> slot = page.NextOccupiedSlot(0); slot.has_value();
             slot = page.NextOccupiedSlot(static_cast<SlotId>(*slot + 1))) {
            const auto bytes = page.GetRecord(*slot);
            visit(RecordId{page_id, *slot}, Record::DeserializeFrom(schema, *bytes));
        }
        return page.NextPageId();
    }

    /// Counts live records by scanning. Used to check the catalog's cached
    /// counter has not drifted.
    [[nodiscard]] std::uint64_t CountRecordsByScan() const;

    /// Number of pages in the chain, for the `.pages` command.
    [[nodiscard]] std::uint32_t PageCountInChain() const;

private:
    /// Tries to place `bytes` in the given page, compacting first if that is
    /// what it takes. Returns the slot, or std::nullopt when the page is full.
    [[nodiscard]] std::optional<SlotId> TryInsertInto(PageId page_id,
                                                      std::span<const std::byte> bytes);

    /// Appends a fresh page to the end of the chain and returns its id.
    [[nodiscard]] PageId AppendPage();

    BufferPoolManager& pool_;
    Catalog& catalog_;
};

}  // namespace minidb
