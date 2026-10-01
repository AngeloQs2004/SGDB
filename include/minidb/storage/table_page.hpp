#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "minidb/common/constants.hpp"
#include "minidb/common/types.hpp"

namespace minidb {

/// Slotted-page view over a 4096-byte buffer.
///
/// This class owns no memory: it interprets a page that the buffer pool holds.
/// That is what lets it be unit-tested without a buffer pool at all, and what
/// keeps the page layout in one place.
///
/// Layout. The header sits at the front, the slot directory grows upward right
/// behind it, and record data grows downward from the end of the page. The gap
/// between the two is the free space:
///
///   0        12                 free_space_begin      free_space_end      4096
///   |--------|------------------|---------------------|-------------------|
///    header     slot directory       free space          record data
///               (grows right)                           (grows left)
///
/// Header (12 bytes):
///
///   offset  size  field
///   ------  ----  ------------------------------------------------------
///        0     1  page_type         kTableData
///        1     1  reserved
///        2     2  slot_count        length of the slot directory
///        4     2  record_count      live records (slot_count minus holes)
///        6     2  free_space_end    lowest byte occupied by record data
///        8     4  next_page_id      next page of the table, or kInvalidPageId
///
/// free_space_begin is deliberately *not* stored: it is always
/// 12 + 4 * slot_count. A stored copy would be a second invariant that can
/// drift out of sync with the directory.
///
/// Slot directory entry (4 bytes):
///
///   offset  size  field
///   ------  ----  ------------------------------------------------------
///        0     2  offset            where the record starts in the page
///        2     2  size              record length in bytes
///
/// A slot with offset == 0 is free. Offset 0 is impossible for real data
/// because the header occupies bytes 0..11, so it works as a sentinel and
/// saves a per-slot state byte. "Never used" and "deleted" mean the same thing
/// here — both are reusable — so one sentinel covers both.
class TablePage {
public:
    static constexpr std::size_t kHeaderSize = 12;
    static constexpr std::size_t kSlotSize = 4;

    /// Largest record that can ever be stored: a full page minus the header and
    /// one slot. 4096 - 12 - 4 = 4080 bytes.
    static constexpr std::size_t kMaxRecordSize = minidb::kMaxRecordSize;
    static_assert(kMaxRecordSize == kPageSize - kHeaderSize - kSlotSize,
                  "el límite de constants.hpp no coincide con esta disposición de página");

    explicit TablePage(std::span<std::byte, kPageSize> page) : page_(page) {}

    /// Turns a freshly allocated (zeroed) page into an empty table page.
    void Initialize();

    [[nodiscard]] PageType GetPageType() const;
    [[nodiscard]] SlotId SlotCount() const;
    [[nodiscard]] std::uint16_t RecordCount() const;
    [[nodiscard]] PageId NextPageId() const;
    void SetNextPageId(PageId page_id);

    /// Bytes available for a new record *and* its slot, without compacting.
    [[nodiscard]] std::size_t FreeSpace() const;

    /// True when a record of `record_size` bytes fits as things stand. Reusing a
    /// free slot avoids paying for a new directory entry.
    [[nodiscard]] bool HasSpaceFor(std::size_t record_size) const;

    /// Space that would be available after compaction, i.e. including the holes
    /// left behind by deletes and shrinking updates.
    [[nodiscard]] std::size_t CompactableSpace() const;

    /// Inserts a record and returns its slot, or std::nullopt when it does not
    /// fit. Callers compact and retry before giving up on the page.
    [[nodiscard]] std::optional<SlotId> InsertRecord(std::span<const std::byte> record);

    /// Returns the bytes of a live record, or std::nullopt when the slot is
    /// free or out of range.
    [[nodiscard]] std::optional<std::span<const std::byte>> GetRecord(SlotId slot_id) const;

    /// Overwrites a record. Returns false when the new bytes do not fit even
    /// after compaction, in which case the caller relocates the record to
    /// another page. The slot id never changes.
    [[nodiscard]] bool UpdateRecord(SlotId slot_id, std::span<const std::byte> record);

    /// Marks a slot free. Returns false when the slot was already free.
    bool DeleteRecord(SlotId slot_id);

    /// Repacks live records against the end of the page, reclaiming every hole.
    ///
    /// Slot ids are preserved. This is critical: the hash index maps primary
    /// keys to (page_id, slot_id), so renumbering slots here would leave every
    /// index entry dangling.
    void Compact();

    /// Walks live slots. Returns the next occupied slot at or after `from`, or
    /// std::nullopt when the page has no more records.
    [[nodiscard]] std::optional<SlotId> NextOccupiedSlot(SlotId from) const;

    /// Throws StorageError when the header and directory contradict each other.
    /// Used by tests after every mutation.
    void CheckInvariants() const;

private:
    [[nodiscard]] std::uint16_t FreeSpaceEnd() const;
    void SetFreeSpaceEnd(std::uint16_t value);
    void SetSlotCount(SlotId value);
    void SetRecordCount(std::uint16_t value);

    [[nodiscard]] std::size_t FreeSpaceBegin() const {
        return kHeaderSize + kSlotSize * SlotCount();
    }

    [[nodiscard]] std::uint16_t SlotOffset(SlotId slot_id) const;
    [[nodiscard]] std::uint16_t SlotLength(SlotId slot_id) const;
    void SetSlot(SlotId slot_id, std::uint16_t offset, std::uint16_t length);

    /// Index of a reusable free slot, if the directory has one.
    [[nodiscard]] std::optional<SlotId> FindFreeSlot() const;

    /// Writes `record` into the data region and returns its offset. The caller
    /// must have checked that it fits.
    [[nodiscard]] std::uint16_t AppendData(std::span<const std::byte> record);

    std::span<std::byte, kPageSize> page_;
};

}  // namespace minidb
