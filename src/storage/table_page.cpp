#include "minidb/storage/table_page.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#include "minidb/common/serialization.hpp"

namespace minidb {
namespace {

// Header field offsets. See the layout table in table_page.hpp.
constexpr std::size_t kOffsetPageType = 0;
constexpr std::size_t kOffsetSlotCount = 2;
constexpr std::size_t kOffsetRecordCount = 4;
constexpr std::size_t kOffsetFreeSpaceEnd = 6;
constexpr std::size_t kOffsetNextPageId = 8;

/// A slot whose offset is 0 is free; see the sentinel note in the header.
constexpr std::uint16_t kFreeSlotOffset = 0;

}  // namespace

void TablePage::Initialize() {
    std::ranges::fill(page_, std::byte{0});
    serialization::WriteU8(page_, kOffsetPageType, static_cast<std::uint8_t>(PageType::kTableData));
    SetSlotCount(0);
    SetRecordCount(0);
    SetFreeSpaceEnd(static_cast<std::uint16_t>(kPageSize));
    SetNextPageId(kInvalidPageId);
}

PageType TablePage::GetPageType() const {
    return static_cast<PageType>(serialization::ReadU8(page_, kOffsetPageType));
}

SlotId TablePage::SlotCount() const { return serialization::ReadU16(page_, kOffsetSlotCount); }

std::uint16_t TablePage::RecordCount() const {
    return serialization::ReadU16(page_, kOffsetRecordCount);
}

PageId TablePage::NextPageId() const { return serialization::ReadU32(page_, kOffsetNextPageId); }

void TablePage::SetNextPageId(PageId page_id) {
    serialization::WriteU32(page_, kOffsetNextPageId, page_id);
}

std::uint16_t TablePage::FreeSpaceEnd() const {
    return serialization::ReadU16(page_, kOffsetFreeSpaceEnd);
}

void TablePage::SetFreeSpaceEnd(std::uint16_t value) {
    serialization::WriteU16(page_, kOffsetFreeSpaceEnd, value);
}

void TablePage::SetSlotCount(SlotId value) {
    serialization::WriteU16(page_, kOffsetSlotCount, value);
}

void TablePage::SetRecordCount(std::uint16_t value) {
    serialization::WriteU16(page_, kOffsetRecordCount, value);
}

std::uint16_t TablePage::SlotOffset(SlotId slot_id) const {
    return serialization::ReadU16(page_, kHeaderSize + kSlotSize * slot_id);
}

std::uint16_t TablePage::SlotLength(SlotId slot_id) const {
    return serialization::ReadU16(page_, kHeaderSize + kSlotSize * slot_id + 2);
}

void TablePage::SetSlot(SlotId slot_id, std::uint16_t offset, std::uint16_t length) {
    serialization::WriteU16(page_, kHeaderSize + kSlotSize * slot_id, offset);
    serialization::WriteU16(page_, kHeaderSize + kSlotSize * slot_id + 2, length);
}

std::size_t TablePage::FreeSpace() const {
    const std::size_t begin = FreeSpaceBegin();
    const std::size_t end = FreeSpaceEnd();
    return (end > begin) ? end - begin : 0;
}

std::optional<SlotId> TablePage::FindFreeSlot() const {
    const SlotId slots = SlotCount();
    for (SlotId i = 0; i < slots; ++i) {
        if (SlotOffset(i) == kFreeSlotOffset) {
            return i;
        }
    }
    return std::nullopt;
}

bool TablePage::HasSpaceFor(std::size_t record_size) const {
    if (record_size > kMaxRecordSize) {
        return false;
    }
    // Reusing a free slot costs only the record bytes; a new slot also costs a
    // directory entry.
    const std::size_t needed = FindFreeSlot().has_value() ? record_size : record_size + kSlotSize;
    return FreeSpace() >= needed;
}

std::size_t TablePage::CompactableSpace() const {
    std::size_t live_bytes = 0;
    const SlotId slots = SlotCount();
    for (SlotId i = 0; i < slots; ++i) {
        if (SlotOffset(i) != kFreeSlotOffset) {
            live_bytes += SlotLength(i);
        }
    }
    return kPageSize - FreeSpaceBegin() - live_bytes;
}

std::uint16_t TablePage::AppendData(std::span<const std::byte> record) {
    const auto offset = static_cast<std::uint16_t>(FreeSpaceEnd() - record.size());
    if (!record.empty()) {
        std::memcpy(page_.data() + offset, record.data(), record.size());
    }
    SetFreeSpaceEnd(offset);
    return offset;
}

std::optional<SlotId> TablePage::InsertRecord(std::span<const std::byte> record) {
    if (record.empty()) {
        throw StorageError("No se puede insertar un registro vacío");
    }
    if (!HasSpaceFor(record.size())) {
        return std::nullopt;
    }

    const std::optional<SlotId> reusable = FindFreeSlot();
    SlotId slot_id = 0;
    if (reusable.has_value()) {
        slot_id = *reusable;
    } else {
        slot_id = SlotCount();
        SetSlotCount(static_cast<SlotId>(slot_id + 1));
    }

    const std::uint16_t offset = AppendData(record);
    SetSlot(slot_id, offset, static_cast<std::uint16_t>(record.size()));
    SetRecordCount(static_cast<std::uint16_t>(RecordCount() + 1));
    return slot_id;
}

std::optional<std::span<const std::byte>> TablePage::GetRecord(SlotId slot_id) const {
    if (slot_id >= SlotCount()) {
        return std::nullopt;
    }
    const std::uint16_t offset = SlotOffset(slot_id);
    if (offset == kFreeSlotOffset) {
        return std::nullopt;
    }
    return std::span<const std::byte>(page_.data() + offset, SlotLength(slot_id));
}

bool TablePage::UpdateRecord(SlotId slot_id, std::span<const std::byte> record) {
    if (slot_id >= SlotCount() || SlotOffset(slot_id) == kFreeSlotOffset) {
        throw StorageError("Actualización sobre un slot inexistente: " + std::to_string(slot_id));
    }
    if (record.empty()) {
        throw StorageError("No se puede actualizar a un registro vacío");
    }
    if (record.size() > kMaxRecordSize) {
        return false;
    }

    const std::uint16_t old_offset = SlotOffset(slot_id);
    const std::uint16_t old_length = SlotLength(slot_id);

    // Same size or smaller: overwrite in place. Any leftover bytes become a hole
    // that Compact reclaims later. The slot id is untouched, so the index does
    // not need to know.
    if (record.size() <= old_length) {
        std::memcpy(page_.data() + old_offset, record.data(), record.size());
        SetSlot(slot_id, old_offset, static_cast<std::uint16_t>(record.size()));
        return true;
    }

    // Bigger: try to append the new version into the free gap, leaving the old
    // bytes as a hole.
    if (FreeSpace() >= record.size()) {
        const std::uint16_t offset = AppendData(record);
        SetSlot(slot_id, offset, static_cast<std::uint16_t>(record.size()));
        return true;
    }

    // Not enough contiguous room: reclaim the holes and try once more. The old
    // bytes must be copied out first, because Compact is about to move or erase
    // them and they are still needed if the retry fails.
    const std::vector<std::byte> old_bytes(page_.data() + old_offset,
                                           page_.data() + old_offset + old_length);

    SetSlot(slot_id, kFreeSlotOffset, 0);
    SetRecordCount(static_cast<std::uint16_t>(RecordCount() - 1));
    Compact();

    if (FreeSpace() >= record.size()) {
        const std::uint16_t offset = AppendData(record);
        SetSlot(slot_id, offset, static_cast<std::uint16_t>(record.size()));
        SetRecordCount(static_cast<std::uint16_t>(RecordCount() + 1));
        return true;
    }

    // The record genuinely does not fit in this page. Put the old version back
    // so the caller sees an unchanged record and can relocate it elsewhere.
    // Space is guaranteed: the record was live here a moment ago.
    const std::uint16_t restored = AppendData(old_bytes);
    SetSlot(slot_id, restored, old_length);
    SetRecordCount(static_cast<std::uint16_t>(RecordCount() + 1));
    return false;
}

bool TablePage::DeleteRecord(SlotId slot_id) {
    if (slot_id >= SlotCount() || SlotOffset(slot_id) == kFreeSlotOffset) {
        return false;
    }
    SetSlot(slot_id, kFreeSlotOffset, 0);
    SetRecordCount(static_cast<std::uint16_t>(RecordCount() - 1));
    return true;
}

void TablePage::Compact() {
    const SlotId slots = SlotCount();

    // Copy the live records out, then lay them back down packed against the end
    // of the page. Going through a scratch buffer keeps the logic obvious and
    // avoids overlapping-copy subtleties; a page is only 4 KB.
    struct LiveRecord {
        SlotId slot_id;
        std::vector<std::byte> bytes;
    };

    std::vector<LiveRecord> live;
    live.reserve(slots);
    for (SlotId i = 0; i < slots; ++i) {
        const std::uint16_t offset = SlotOffset(i);
        if (offset == kFreeSlotOffset) {
            continue;
        }
        const std::uint16_t length = SlotLength(i);
        live.push_back({i, std::vector<std::byte>(page_.data() + offset,
                                                  page_.data() + offset + length)});
    }

    // Clear the data region only; the header and directory stay where they are.
    std::fill(page_.begin() + static_cast<std::ptrdiff_t>(FreeSpaceBegin()), page_.end(),
              std::byte{0});
    SetFreeSpaceEnd(static_cast<std::uint16_t>(kPageSize));

    for (const LiveRecord& record : live) {
        const std::uint16_t offset = AppendData(record.bytes);
        // Slot ids are preserved, which is what keeps hash index entries valid.
        SetSlot(record.slot_id, offset, static_cast<std::uint16_t>(record.bytes.size()));
    }
}

std::optional<SlotId> TablePage::NextOccupiedSlot(SlotId from) const {
    const SlotId slots = SlotCount();
    for (SlotId i = from; i < slots; ++i) {
        if (SlotOffset(i) != kFreeSlotOffset) {
            return i;
        }
    }
    return std::nullopt;
}

void TablePage::CheckInvariants() const {
    if (GetPageType() != PageType::kTableData) {
        throw StorageError("La página no es una página de tabla");
    }

    const std::size_t begin = FreeSpaceBegin();
    const std::size_t end = FreeSpaceEnd();
    if (begin > end) {
        throw StorageError("El directorio de slots invadió la zona de datos: begin=" +
                           std::to_string(begin) + ", end=" + std::to_string(end));
    }
    if (end > kPageSize) {
        throw StorageError("free_space_end fuera de la página: " + std::to_string(end));
    }

    const SlotId slots = SlotCount();
    std::uint16_t live = 0;
    for (SlotId i = 0; i < slots; ++i) {
        const std::uint16_t offset = SlotOffset(i);
        if (offset == kFreeSlotOffset) {
            continue;
        }
        ++live;
        const std::uint16_t length = SlotLength(i);
        if (offset < end || std::size_t{offset} + length > kPageSize) {
            throw StorageError("El slot " + std::to_string(i) + " apunta fuera de la zona de datos");
        }
    }
    if (live != RecordCount()) {
        throw StorageError("record_count (" + std::to_string(RecordCount()) +
                           ") no coincide con los slots ocupados (" + std::to_string(live) + ")");
    }
}

}  // namespace minidb
