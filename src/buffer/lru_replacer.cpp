#include "minidb/buffer/lru_replacer.hpp"

namespace minidb {

void LruReplacer::RecordAccess(FrameId frame_id) {
    const auto it = positions_.find(frame_id);
    if (it == positions_.end()) {
        // The frame is pinned, so it is not a candidate and its access order
        // does not matter yet. SetEvictable will insert it when it is unpinned.
        return;
    }
    frames_.splice(frames_.begin(), frames_, it->second);
}

void LruReplacer::SetEvictable(FrameId frame_id, bool evictable) {
    const auto it = positions_.find(frame_id);

    if (evictable) {
        if (it == positions_.end()) {
            frames_.push_front(frame_id);
            positions_[frame_id] = frames_.begin();
        }
        return;
    }

    if (it != positions_.end()) {
        frames_.erase(it->second);
        positions_.erase(it);
    }
}

std::optional<FrameId> LruReplacer::Evict() {
    if (frames_.empty()) {
        return std::nullopt;
    }
    const FrameId victim = frames_.back();
    frames_.pop_back();
    positions_.erase(victim);
    return victim;
}

void LruReplacer::Remove(FrameId frame_id) { SetEvictable(frame_id, false); }

}  // namespace minidb
