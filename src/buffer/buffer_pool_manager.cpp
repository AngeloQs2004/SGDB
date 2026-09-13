#include "minidb/buffer/buffer_pool_manager.hpp"

#include <algorithm>
#include <string>

namespace minidb {

BufferPoolManager::BufferPoolManager(DiskManager& disk_manager, std::size_t frame_count)
    : disk_manager_(disk_manager) {
    if (frame_count == 0) {
        throw StorageError("El Buffer Pool necesita al menos un frame");
    }

    frames_.resize(frame_count);
    free_frames_.reserve(frame_count);
    // Handed out from the back, so the first pages land in frame 0 upward and
    // the `.buffer` listing reads in a natural order.
    for (std::size_t i = frame_count; i > 0; --i) {
        free_frames_.push_back(i - 1);
    }
}

BufferPoolManager::~BufferPoolManager() {
    // Best effort: a destructor must not throw. Database::Close flushes
    // explicitly, so this only covers abnormal teardown.
    try {
        FlushAllPages();
    } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch)
    }
}

void BufferPoolManager::WriteBack(Frame& frame) {
    if (!frame.is_dirty || !frame.is_occupied) {
        return;
    }
    disk_manager_.WritePage(frame.page_id, frame.data);
    ++statistics_.disk_writes;
    // Cleared only after the write actually succeeded, so a failed write leaves
    // the page marked dirty and the data is not silently dropped.
    frame.is_dirty = false;
}

FrameId BufferPoolManager::AcquireFrame() {
    if (!free_frames_.empty()) {
        const FrameId frame_id = free_frames_.back();
        free_frames_.pop_back();
        return frame_id;
    }

    const std::optional<FrameId> victim = replacer_.Evict();
    if (!victim.has_value()) {
        throw StorageError("No hay frames disponibles en el Buffer Pool: las " +
                           std::to_string(frames_.size()) +
                           " páginas residentes están fijadas (pin_count > 0)");
    }

    Frame& frame = frames_[*victim];
    // A modified page must reach the disk before its frame is reused. Skipping
    // this is the classic silent data loss.
    WriteBack(frame);

    page_table_.erase(frame.page_id);
    frame.is_occupied = false;
    frame.page_id = kInvalidPageId;
    ++statistics_.evictions;

    return *victim;
}

std::span<std::byte, kPageSize> BufferPoolManager::FetchPage(PageId page_id) {
    if (page_id == kInvalidPageId) {
        throw StorageError("FetchPage recibió un identificador de página inválido");
    }

    const auto resident = page_table_.find(page_id);
    if (resident != page_table_.end()) {
        Frame& frame = frames_[resident->second];
        ++frame.pin_count;
        replacer_.SetEvictable(resident->second, false);
        replacer_.RecordAccess(resident->second);
        ++statistics_.hits;
        return frame.Data();
    }

    ++statistics_.misses;
    const FrameId frame_id = AcquireFrame();
    Frame& frame = frames_[frame_id];

    disk_manager_.ReadPage(page_id, frame.data);
    ++statistics_.disk_reads;

    frame.page_id = page_id;
    frame.pin_count = 1;
    frame.is_dirty = false;
    frame.is_occupied = true;
    page_table_[page_id] = frame_id;
    replacer_.SetEvictable(frame_id, false);

    return frame.Data();
}

std::span<std::byte, kPageSize> BufferPoolManager::NewPage(PageId& page_id) {
    // Allocate first: if the file cannot grow, no frame has been disturbed.
    const PageId allocated = disk_manager_.AllocatePage();

    const FrameId frame_id = AcquireFrame();
    Frame& frame = frames_[frame_id];

    // A fresh page is known to be zeroed, so reading it back from disk would be
    // wasted I/O.
    std::ranges::fill(frame.data, std::byte{0});
    frame.page_id = allocated;
    frame.pin_count = 1;
    frame.is_dirty = true;
    frame.is_occupied = true;
    page_table_[allocated] = frame_id;
    replacer_.SetEvictable(frame_id, false);

    page_id = allocated;
    return frame.Data();
}

bool BufferPoolManager::UnpinPage(PageId page_id, bool is_dirty) {
    const auto resident = page_table_.find(page_id);
    if (resident == page_table_.end()) {
        return false;
    }

    Frame& frame = frames_[resident->second];
    if (frame.pin_count == 0) {
        throw StorageError("UnpinPage sobre la página " + std::to_string(page_id) +
                           ", que no estaba fijada");
    }

    // Dirtiness accumulates: one reader unpinning cleanly must not erase the
    // fact that another writer modified the page.
    if (is_dirty) {
        frame.is_dirty = true;
    }

    --frame.pin_count;
    if (frame.pin_count == 0) {
        replacer_.SetEvictable(resident->second, true);
    }
    return true;
}

bool BufferPoolManager::FlushPage(PageId page_id) {
    const auto resident = page_table_.find(page_id);
    if (resident == page_table_.end()) {
        return false;
    }
    WriteBack(frames_[resident->second]);
    return true;
}

void BufferPoolManager::FlushAllPages() {
    for (Frame& frame : frames_) {
        WriteBack(frame);
    }
    disk_manager_.Flush();
}

bool BufferPoolManager::DeletePage(PageId page_id) {
    const auto resident = page_table_.find(page_id);

    if (resident != page_table_.end()) {
        Frame& frame = frames_[resident->second];
        if (frame.pin_count > 0) {
            return false;
        }
        // The page is about to be freed, so its contents are irrelevant: drop
        // the dirty flag instead of writing bytes nobody will read.
        frame.is_dirty = false;
        frame.is_occupied = false;
        frame.page_id = kInvalidPageId;
        replacer_.Remove(resident->second);
        free_frames_.push_back(resident->second);
        page_table_.erase(resident);
    }

    disk_manager_.DeallocatePage(page_id);
    return true;
}

std::uint32_t BufferPoolManager::GetPinCount(PageId page_id) const {
    const auto resident = page_table_.find(page_id);
    return (resident == page_table_.end()) ? 0 : frames_[resident->second].pin_count;
}

bool BufferPoolManager::AllPagesUnpinned() const {
    return std::ranges::none_of(frames_, [](const Frame& frame) {
        return frame.is_occupied && frame.pin_count > 0;
    });
}

PageGuard FetchGuarded(BufferPoolManager& pool, PageId page_id) {
    return PageGuard(pool, page_id, pool.FetchPage(page_id));
}

PageGuard NewGuarded(BufferPoolManager& pool, PageId& page_id) {
    auto data = pool.NewPage(page_id);
    return PageGuard(pool, page_id, data);
}

}  // namespace minidb
