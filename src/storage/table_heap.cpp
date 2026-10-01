#include "minidb/storage/table_heap.hpp"

#include <vector>

namespace minidb {

TableHeap::TableHeap(BufferPoolManager& pool, Catalog& catalog) : pool_(pool), catalog_(catalog) {}

PageId TableHeap::CreateFirstPage(BufferPoolManager& pool) {
    PageId page_id = kInvalidPageId;
    PageGuard guard = NewGuarded(pool, page_id);
    TablePage page(guard.Data());
    page.Initialize();
    guard.MarkDirty();
    return page_id;
}

std::optional<SlotId> TableHeap::TryInsertInto(PageId page_id, std::span<const std::byte> bytes) {
    PageGuard guard = FetchGuarded(pool_, page_id);
    TablePage page(guard.Data());

    std::optional<SlotId> slot = page.InsertRecord(bytes);
    if (!slot.has_value() && page.CompactableSpace() >= bytes.size() + TablePage::kSlotSize) {
        // The page has room, just not in one piece: reclaim the holes left by
        // deletes and shrinking updates, then retry.
        page.Compact();
        slot = page.InsertRecord(bytes);
    }

    if (slot.has_value()) {
        guard.MarkDirty();
    }
    return slot;
}

PageId TableHeap::AppendPage() {
    PageId new_page_id = kInvalidPageId;
    {
        PageGuard guard = NewGuarded(pool_, new_page_id);
        TablePage page(guard.Data());
        page.Initialize();
        guard.MarkDirty();
    }

    const PageId last_page_id = catalog_.LastTablePageId();
    {
        PageGuard guard = FetchGuarded(pool_, last_page_id);
        TablePage last(guard.Data());
        last.SetNextPageId(new_page_id);
        guard.MarkDirty();
    }

    catalog_.SetLastTablePageId(new_page_id);
    return new_page_id;
}

RecordId TableHeap::InsertRecord(const Record& record) {
    const Schema& schema = catalog_.GetSchema();
    record.Validate(schema);

    std::vector<std::byte> bytes(record.SerializedSize(schema));
    record.SerializeTo(schema, bytes);

    if (bytes.size() > TablePage::kMaxRecordSize) {
        // Unreachable for any legal schema (8 columns of VARCHAR(255) is 2056
        // bytes against a 4080-byte limit), but checked rather than assumed.
        throw QueryError("El registro ocupa " + std::to_string(bytes.size()) +
                         " bytes y no cabe en una página");
    }

    // The last page is where an append-heavy workload finds room, so try it
    // first and keep the common case at one page access.
    const PageId last_page_id = catalog_.LastTablePageId();
    if (const auto slot = TryInsertInto(last_page_id, bytes)) {
        return RecordId{last_page_id, *slot};
    }

    // Otherwise walk the chain looking for a page with reclaimable space. This
    // is what puts pages emptied by DELETE back to work instead of letting the
    // file grow.
    PageId page_id = catalog_.FirstTablePageId();
    while (page_id != kInvalidPageId) {
        if (page_id != last_page_id) {
            if (const auto slot = TryInsertInto(page_id, bytes)) {
                return RecordId{page_id, *slot};
            }
        }
        PageGuard guard = FetchGuarded(pool_, page_id);
        page_id = TablePage(guard.Data()).NextPageId();
    }

    // Every existing page is full: grow the table by one page.
    const PageId fresh = AppendPage();
    const auto slot = TryInsertInto(fresh, bytes);
    if (!slot.has_value()) {
        throw StorageError("Un registro de " + std::to_string(bytes.size()) +
                           " bytes no cabe ni en una página recién creada");
    }
    return RecordId{fresh, *slot};
}

std::optional<Record> TableHeap::GetRecord(RecordId rid) const {
    if (!rid.IsValid()) {
        return std::nullopt;
    }

    PageGuard guard = FetchGuarded(pool_, rid.page_id);
    const TablePage page(guard.Data());

    const auto bytes = page.GetRecord(rid.slot_id);
    if (!bytes.has_value()) {
        return std::nullopt;
    }
    return Record::DeserializeFrom(catalog_.GetSchema(), *bytes);
}

RecordId TableHeap::UpdateRecord(RecordId rid, const Record& record) {
    const Schema& schema = catalog_.GetSchema();
    record.Validate(schema);

    std::vector<std::byte> bytes(record.SerializedSize(schema));
    record.SerializeTo(schema, bytes);

    {
        PageGuard guard = FetchGuarded(pool_, rid.page_id);
        TablePage page(guard.Data());
        if (page.UpdateRecord(rid.slot_id, bytes)) {
            guard.MarkDirty();
            return rid;  // stayed put, so the index needs no update
        }
        // UpdateRecord left the old version in place, so the record still
        // exists while the new copy is being placed elsewhere.
    }

    // Relocate. Insert first, then remove the old copy: if the insert throws,
    // the original is still there and nothing has been lost.
    const RecordId relocated = InsertRecord(record);
    if (!DeleteRecord(rid)) {
        throw StorageError("La reubicación dejó el registro original sin borrar");
    }
    return relocated;
}

bool TableHeap::DeleteRecord(RecordId rid) {
    if (!rid.IsValid()) {
        return false;
    }

    PageGuard guard = FetchGuarded(pool_, rid.page_id);
    TablePage page(guard.Data());
    if (!page.DeleteRecord(rid.slot_id)) {
        return false;
    }
    guard.MarkDirty();
    return true;
}

std::optional<std::pair<RecordId, Record>> TableHeap::Iterator::Next() {
    while (page_id_ != kInvalidPageId) {
        PageGuard guard = FetchGuarded(heap_->pool_, page_id_);
        const TablePage page(guard.Data());

        if (const auto slot = page.NextOccupiedSlot(slot_id_)) {
            const auto bytes = page.GetRecord(*slot);
            const RecordId rid{page_id_, *slot};
            slot_id_ = static_cast<SlotId>(*slot + 1);
            return std::make_pair(rid,
                                  Record::DeserializeFrom(heap_->catalog_.GetSchema(), *bytes));
        }

        page_id_ = page.NextPageId();
        slot_id_ = 0;
    }
    return std::nullopt;
}

std::uint64_t TableHeap::CountRecordsByScan() const {
    std::uint64_t total = 0;
    Iterator it = Begin();
    while (it.Next().has_value()) {
        ++total;
    }
    return total;
}

std::uint32_t TableHeap::PageCountInChain() const {
    std::uint32_t pages = 0;
    PageId page_id = catalog_.FirstTablePageId();
    while (page_id != kInvalidPageId) {
        ++pages;
        PageGuard guard = FetchGuarded(pool_, page_id);
        page_id = TablePage(guard.Data()).NextPageId();
    }
    return pages;
}

}  // namespace minidb
