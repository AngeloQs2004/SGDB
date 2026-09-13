#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

#include "minidb/buffer/lru_replacer.hpp"
#include "minidb/common/constants.hpp"
#include "minidb/common/types.hpp"
#include "minidb/storage/disk_manager.hpp"

namespace minidb {

/// One slot of RAM able to hold a single page.
struct Frame {
    PageId page_id = kInvalidPageId;
    std::array<std::byte, kPageSize> data{};
    std::uint32_t pin_count = 0;
    bool is_dirty = false;
    bool is_occupied = false;

    [[nodiscard]] std::span<std::byte, kPageSize> Data() { return data; }
};

/// Counters exposed by the `.buffer` command. They cost four increments and are
/// the clearest evidence that the pool actually does its job.
struct BufferPoolStatistics {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t evictions = 0;
    std::uint64_t disk_reads = 0;
    std::uint64_t disk_writes = 0;
};

/// Keeps a fixed number of pages in RAM and decides which one to evict.
///
/// Every page above the disk layer is reached through this class. TableHeap,
/// HashIndex and Catalog hold a reference to a BufferPoolManager and never see
/// a DiskManager or a file stream, which is what stops them from reading the
/// disk directly on every operation.
///
/// A page is only written back when it is evicted or explicitly flushed, so a
/// modified page must be reported through UnpinPage(id, true). Losing that flag
/// is exactly the "dirty pages are not synchronised" failure, so the dirty bit
/// is only ever cleared right after a successful write.
class BufferPoolManager {
public:
    BufferPoolManager(DiskManager& disk_manager, std::size_t frame_count);

    ~BufferPoolManager();

    BufferPoolManager(const BufferPoolManager&) = delete;
    BufferPoolManager& operator=(const BufferPoolManager&) = delete;
    BufferPoolManager(BufferPoolManager&&) = delete;
    BufferPoolManager& operator=(BufferPoolManager&&) = delete;

    /// Loads a page into RAM and pins it. Throws StorageError when every frame
    /// is pinned, rather than returning null and letting the caller dereference
    /// it.
    [[nodiscard]] std::span<std::byte, kPageSize> FetchPage(PageId page_id);

    /// Allocates a fresh page, pins it and returns a zeroed buffer. The new page
    /// id is written to `page_id`. The page is never read from disk.
    [[nodiscard]] std::span<std::byte, kPageSize> NewPage(PageId& page_id);

    /// Releases one reference to a page. `is_dirty` must be true when the caller
    /// modified the buffer. Returns false when the page was not pinned.
    bool UnpinPage(PageId page_id, bool is_dirty);

    /// Writes one page to disk if it is dirty. Does not unpin or evict it.
    bool FlushPage(PageId page_id);

    /// Writes every dirty page to disk and flushes the file.
    void FlushAllPages();

    /// Removes a page from the pool and returns it to the free list. Fails when
    /// the page is still pinned.
    bool DeletePage(PageId page_id);

    [[nodiscard]] std::size_t FrameCount() const { return frames_.size(); }
    [[nodiscard]] const BufferPoolStatistics& Statistics() const { return statistics_; }
    [[nodiscard]] DiskManager& Disk() { return disk_manager_; }

    /// Pin count of a page, or 0 when it is not resident. Used by tests to prove
    /// that no operation leaks a pin.
    [[nodiscard]] std::uint32_t GetPinCount(PageId page_id) const;

    /// True when every resident page is unpinned. The integration tests assert
    /// this after each SQL statement.
    [[nodiscard]] bool AllPagesUnpinned() const;

    /// Snapshot of the frame table for the `.buffer` command.
    [[nodiscard]] const std::vector<Frame>& Frames() const { return frames_; }

private:
    /// Finds a frame to use, taking a free one first and evicting the least
    /// recently used page otherwise. Writes the victim back when it is dirty.
    [[nodiscard]] FrameId AcquireFrame();

    void WriteBack(Frame& frame);

    DiskManager& disk_manager_;
    std::vector<Frame> frames_;
    std::unordered_map<PageId, FrameId> page_table_;
    std::vector<FrameId> free_frames_;
    LruReplacer replacer_;
    BufferPoolStatistics statistics_;
};

/// RAII handle that unpins a page when it goes out of scope.
///
/// Every access to a page goes through a guard. A leaked pin permanently
/// removes a frame from the pool, and with only a handful of frames that turns
/// into "no victim available" a few statements later — the most likely failure
/// in the whole system, and one that an early return or a thrown exception
/// would otherwise cause silently.
class PageGuard {
public:
    PageGuard(BufferPoolManager& pool, PageId page_id, std::span<std::byte, kPageSize> data)
        : pool_(&pool), page_id_(page_id), data_(data) {}

    ~PageGuard() {
        if (pool_ != nullptr) {
            pool_->UnpinPage(page_id_, is_dirty_);
        }
    }

    PageGuard(const PageGuard&) = delete;
    PageGuard& operator=(const PageGuard&) = delete;

    PageGuard(PageGuard&& other) noexcept
        : pool_(other.pool_), page_id_(other.page_id_), data_(other.data_),
          is_dirty_(other.is_dirty_) {
        other.pool_ = nullptr;
    }

    PageGuard& operator=(PageGuard&& other) noexcept {
        if (this != &other) {
            if (pool_ != nullptr) {
                pool_->UnpinPage(page_id_, is_dirty_);
            }
            pool_ = other.pool_;
            page_id_ = other.page_id_;
            data_ = other.data_;
            is_dirty_ = other.is_dirty_;
            other.pool_ = nullptr;
        }
        return *this;
    }

    [[nodiscard]] std::span<std::byte, kPageSize> Data() const { return data_; }
    [[nodiscard]] PageId GetPageId() const { return page_id_; }

    /// Records that the buffer was modified, so the page is written back.
    void MarkDirty() { is_dirty_ = true; }

private:
    BufferPoolManager* pool_;
    PageId page_id_;
    std::span<std::byte, kPageSize> data_;
    bool is_dirty_ = false;
};

/// Convenience wrappers so callers write `auto guard = FetchGuarded(pool, id)`
/// instead of pairing every Fetch with an Unpin by hand.
[[nodiscard]] PageGuard FetchGuarded(BufferPoolManager& pool, PageId page_id);
[[nodiscard]] PageGuard NewGuarded(BufferPoolManager& pool, PageId& page_id);

}  // namespace minidb
