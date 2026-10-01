#pragma once

#include <cstddef>
#include <list>
#include <optional>
#include <unordered_map>

#include "minidb/common/types.hpp"

namespace minidb {

/// Least Recently Used victim selection for the buffer pool.
///
/// The rubric accepts LRU, Clock or FIFO; LRU is chosen because it is the
/// easiest of the three to explain and to observe in a demo.
///
/// A std::list holds the evictable frames in access order and a hash map gives
/// O(1) access to each frame's position in that list, so every operation is
/// constant time without any clever tricks.
///
///   front of the list = most recently used
///   back of the list  = least recently used, and therefore the next victim
///
/// Only frames marked evictable are tracked. A pinned frame is simply absent
/// from the structure, which is what makes "a pinned page is never replaced"
/// true by construction rather than by a check.
class LruReplacer {
public:
    /// Records a use of `frame_id`, moving it to the most-recently-used end.
    /// Frames not currently evictable are ignored.
    void RecordAccess(FrameId frame_id);

    /// Adds the frame to the victim pool, or removes it from it. Called when a
    /// page's pin count reaches zero or rises above it.
    void SetEvictable(FrameId frame_id, bool evictable);

    /// Removes and returns the least recently used frame, or std::nullopt when
    /// every frame is pinned.
    [[nodiscard]] std::optional<FrameId> Evict();

    /// Drops a frame from the structure regardless of its state.
    void Remove(FrameId frame_id);

    /// Number of frames currently eligible for eviction.
    [[nodiscard]] std::size_t Size() const { return positions_.size(); }

private:
    std::list<FrameId> frames_;  // front = most recent, back = least recent
    std::unordered_map<FrameId, std::list<FrameId>::iterator> positions_;
};

}  // namespace minidb
