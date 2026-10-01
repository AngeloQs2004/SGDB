#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>

#include "minidb/common/constants.hpp"
#include "minidb/common/types.hpp"

namespace minidb {

/// Owns the database file and reads/writes it exclusively in whole 4096-byte
/// pages. This is the only class in the system that touches a std::fstream.
///
/// Page 0 is the file header and belongs to the DiskManager alone: it holds the
/// allocator's own bookkeeping (page count and free-list head). Every other page
/// reaches callers through the BufferPoolManager. Keeping page 0 private is what
/// avoids a circular dependency between the allocator and the buffer pool.
///
/// On-disk layout of page 0 (little-endian):
///
///   offset  size  field
///   ------  ----  -----------------------------------------------
///        0     4  magic_number      0x444E494D, reads as "MIND" on disk
///        4     2  format_version    1
///        6     2  page_size         4096
///        8     4  page_count        total pages in the file
///       12     4  free_page_head    head of the free list, or kInvalidPageId
///       16     4  catalog_page_id   always 1
///       20  4076  zero padding
///
/// A freed page becomes a node of an intrusive singly-linked list, so the free
/// list costs no extra pages:
///
///   offset  size  field
///   ------  ----  -----------------------------------------------
///        0     1  page_type         kFree
///        1     3  reserved
///        4     4  next_free_page_id
class DiskManager {
public:
    /// Opens `database_path`, creating and initialising it if it does not exist.
    /// Throws StorageError if the file exists but is not a valid database.
    explicit DiskManager(std::filesystem::path database_path);

    ~DiskManager();

    DiskManager(const DiskManager&) = delete;
    DiskManager& operator=(const DiskManager&) = delete;
    DiskManager(DiskManager&&) = delete;
    DiskManager& operator=(DiskManager&&) = delete;

    /// Reads exactly kPageSize bytes. A short read means the file is truncated
    /// and raises StorageError rather than leaving the buffer half-filled.
    void ReadPage(PageId page_id, std::span<std::byte, kPageSize> destination);

    /// Writes exactly kPageSize bytes.
    void WritePage(PageId page_id, std::span<const std::byte, kPageSize> source);

    /// Returns a zeroed page, reusing a freed one when the free list is not
    /// empty and extending the file otherwise. Reuse is what keeps the file from
    /// growing forever across delete/insert cycles.
    [[nodiscard]] PageId AllocatePage();

    /// Returns a page to the free list. Page 0 can never be freed.
    void DeallocatePage(PageId page_id);

    /// Persists the header and flushes the stream to the operating system.
    void Flush();

    /// Real size of the file on disk, in bytes. Always a multiple of kPageSize.
    [[nodiscard]] std::uint64_t FileSize();

    [[nodiscard]] PageId PageCount() const { return page_count_; }

    [[nodiscard]] PageId FreePageHead() const { return free_page_head_; }

    [[nodiscard]] const std::filesystem::path& Path() const { return path_; }

    /// True when this run created the file, so upper layers know they still have
    /// to lay out the catalog.
    [[nodiscard]] bool WasCreated() const { return was_created_; }

private:
    void CreateDatabase();
    void LoadAndValidateHeader();
    void WriteHeader();
    void ValidatePageId(PageId page_id, const char* operation) const;
    void ReadRaw(PageId page_id, std::span<std::byte, kPageSize> destination);
    void WriteRaw(PageId page_id, std::span<const std::byte, kPageSize> source);

    std::filesystem::path path_;
    std::fstream file_;
    PageId page_count_ = 0;
    PageId free_page_head_ = kInvalidPageId;
    bool header_dirty_ = false;
    bool was_created_ = false;
};

}  // namespace minidb
