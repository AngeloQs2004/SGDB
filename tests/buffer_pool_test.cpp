#include "minidb/buffer/buffer_pool_manager.hpp"

#include <gtest/gtest.h>

#include <vector>

#include "minidb/buffer/lru_replacer.hpp"
#include "minidb/common/serialization.hpp"
#include "test_helpers.hpp"

namespace minidb {
namespace {

using testing::TempDatabase;

// --- LruReplacer ---------------------------------------------------------

TEST(LruReplacerTest, EvictsTheLeastRecentlyUsedFrame) {
    LruReplacer replacer;
    for (FrameId i = 0; i < 3; ++i) {
        replacer.SetEvictable(i, true);
        replacer.RecordAccess(i);
    }
    // Access order was 0, 1, 2, so 0 is the oldest.
    EXPECT_EQ(replacer.Evict(), std::optional<FrameId>{0});
    EXPECT_EQ(replacer.Evict(), std::optional<FrameId>{1});
    EXPECT_EQ(replacer.Evict(), std::optional<FrameId>{2});
    EXPECT_EQ(replacer.Evict(), std::nullopt);
}

TEST(LruReplacerTest, RecordAccessRefreshesRecency) {
    LruReplacer replacer;
    for (FrameId i = 0; i < 3; ++i) {
        replacer.SetEvictable(i, true);
        replacer.RecordAccess(i);
    }
    replacer.RecordAccess(0);  // 0 becomes the newest, so 1 is now the oldest

    EXPECT_EQ(replacer.Evict(), std::optional<FrameId>{1});
    EXPECT_EQ(replacer.Evict(), std::optional<FrameId>{2});
    EXPECT_EQ(replacer.Evict(), std::optional<FrameId>{0});
}

TEST(LruReplacerTest, NonEvictableFramesAreNeverVictims) {
    LruReplacer replacer;
    replacer.SetEvictable(0, true);
    replacer.SetEvictable(1, true);
    replacer.SetEvictable(0, false);  // frame 0 gets pinned

    EXPECT_EQ(replacer.Size(), 1u);
    EXPECT_EQ(replacer.Evict(), std::optional<FrameId>{1});
    EXPECT_EQ(replacer.Evict(), std::nullopt);
}

TEST(LruReplacerTest, SetEvictableIsIdempotent) {
    LruReplacer replacer;
    replacer.SetEvictable(0, true);
    replacer.SetEvictable(0, true);
    EXPECT_EQ(replacer.Size(), 1u);

    replacer.SetEvictable(0, false);
    replacer.SetEvictable(0, false);
    EXPECT_EQ(replacer.Size(), 0u);
}

// --- BufferPoolManager ---------------------------------------------------

class BufferPoolTest : public ::testing::Test {
protected:
    static constexpr std::size_t kFrames = 4;

    BufferPoolTest() : temp_("buffer"), disk_(temp_.Path()), pool_(disk_, kFrames) {}

    /// Allocates a page and stamps a recognisable value into it.
    PageId MakePage(std::uint32_t marker) {
        PageId page_id = kInvalidPageId;
        auto data = pool_.NewPage(page_id);
        serialization::WriteU32(data, 0, marker);
        pool_.UnpinPage(page_id, true);
        return page_id;
    }

    [[nodiscard]] std::uint32_t ReadMarker(PageId page_id) {
        auto data = pool_.FetchPage(page_id);
        const std::uint32_t marker = serialization::ReadU32(data, 0);
        pool_.UnpinPage(page_id, false);
        return marker;
    }

    TempDatabase temp_;
    DiskManager disk_;
    BufferPoolManager pool_;
};

TEST_F(BufferPoolTest, PoolIsLimitedToConfiguredFrames) {
    EXPECT_EQ(pool_.FrameCount(), kFrames);
}

TEST_F(BufferPoolTest, NewPageReturnsAZeroedPinnedPage) {
    PageId page_id = kInvalidPageId;
    auto data = pool_.NewPage(page_id);

    EXPECT_NE(page_id, kInvalidPageId);
    EXPECT_EQ(pool_.GetPinCount(page_id), 1u);
    for (std::byte value : data) {
        EXPECT_EQ(value, std::byte{0});
    }
    pool_.UnpinPage(page_id, false);
}

TEST_F(BufferPoolTest, ResidentPageIsAHitAndCausesNoDiskRead) {
    const PageId page_id = MakePage(0xAAAA);
    const auto reads_before = pool_.Statistics().disk_reads;

    EXPECT_EQ(ReadMarker(page_id), 0xAAAAu);

    EXPECT_EQ(pool_.Statistics().disk_reads, reads_before);  // served from RAM
    EXPECT_GT(pool_.Statistics().hits, 0u);
}

TEST_F(BufferPoolTest, EvictedPageIsAMissAndIsReadBackFromDisk) {
    const PageId first = MakePage(0x1111);
    // Touch more pages than there are frames, forcing `first` out.
    for (std::size_t i = 0; i < kFrames + 2; ++i) {
        (void)MakePage(static_cast<std::uint32_t>(0x2000 + i));
    }
    const auto misses_before = pool_.Statistics().misses;

    EXPECT_EQ(ReadMarker(first), 0x1111u);

    EXPECT_GT(pool_.Statistics().misses, misses_before);
    EXPECT_GT(pool_.Statistics().evictions, 0u);
}

TEST_F(BufferPoolTest, PinCountRisesAndFalls) {
    const PageId page_id = MakePage(1);

    (void)pool_.FetchPage(page_id);
    EXPECT_EQ(pool_.GetPinCount(page_id), 1u);
    (void)pool_.FetchPage(page_id);
    EXPECT_EQ(pool_.GetPinCount(page_id), 2u);

    EXPECT_TRUE(pool_.UnpinPage(page_id, false));
    EXPECT_EQ(pool_.GetPinCount(page_id), 1u);
    EXPECT_TRUE(pool_.UnpinPage(page_id, false));
    EXPECT_EQ(pool_.GetPinCount(page_id), 0u);
    EXPECT_TRUE(pool_.AllPagesUnpinned());
}

TEST_F(BufferPoolTest, UnpinningAnUnpinnedPageThrows) {
    const PageId page_id = MakePage(1);
    EXPECT_FALSE(pool_.UnpinPage(9999, false));  // not resident: reported, not fatal
    EXPECT_THROW(pool_.UnpinPage(page_id, false), StorageError);
}

// The core guarantee of the criterion: a modified page must reach the disk
// before its frame is handed to somebody else.
TEST_F(BufferPoolTest, DirtyPageIsWrittenBeforeEviction) {
    const PageId target = MakePage(0xCAFE);
    const auto writes_before = pool_.Statistics().disk_writes;

    // Fill the pool with other pages so `target` has to be evicted.
    std::vector<PageId> others;
    for (std::size_t i = 0; i < kFrames; ++i) {
        others.push_back(MakePage(static_cast<std::uint32_t>(i)));
    }

    EXPECT_GT(pool_.Statistics().disk_writes, writes_before);
    EXPECT_EQ(pool_.GetPinCount(target), 0u);

    // Read it back: the value survived only because the eviction wrote it out.
    EXPECT_EQ(ReadMarker(target), 0xCAFEu);
}

TEST_F(BufferPoolTest, DirtyFlagIsNotLostWhenAnotherReaderUnpinsClean) {
    const PageId page_id = MakePage(1);

    auto writer = pool_.FetchPage(page_id);
    serialization::WriteU32(writer, 0, 0xBEEF);
    (void)pool_.FetchPage(page_id);

    pool_.UnpinPage(page_id, true);   // the writer reports the change
    pool_.UnpinPage(page_id, false);  // a reader unpins cleanly

    pool_.FlushAllPages();
    // Force the page out of RAM and read it from disk.
    for (std::size_t i = 0; i < kFrames + 1; ++i) {
        (void)MakePage(static_cast<std::uint32_t>(i));
    }
    EXPECT_EQ(ReadMarker(page_id), 0xBEEFu);
}

TEST_F(BufferPoolTest, PinnedPageIsNeverEvicted) {
    std::vector<PageId> pinned;
    for (std::size_t i = 0; i < kFrames; ++i) {
        PageId page_id = kInvalidPageId;
        (void)pool_.NewPage(page_id);  // deliberately left pinned
        pinned.push_back(page_id);
    }

    // Every frame is pinned, so there is no victim and the pool must say so
    // rather than evict something it promised to keep.
    PageId overflow = kInvalidPageId;
    EXPECT_THROW((void)pool_.NewPage(overflow), StorageError);
    EXPECT_THROW((void)pool_.FetchPage(pinned[0] + 100), StorageError);

    for (PageId page_id : pinned) {
        EXPECT_EQ(pool_.GetPinCount(page_id), 1u);
        pool_.UnpinPage(page_id, false);
    }
}

TEST_F(BufferPoolTest, EvictionFollowsLruOrder) {
    std::vector<PageId> pages;
    for (std::size_t i = 0; i < kFrames; ++i) {
        pages.push_back(MakePage(static_cast<std::uint32_t>(0x100 + i)));
    }
    // Touch every page except the first, making it the least recently used.
    for (std::size_t i = 1; i < pages.size(); ++i) {
        EXPECT_EQ(ReadMarker(pages[i]), 0x100u + i);
    }

    const PageId newcomer = MakePage(0x999);
    EXPECT_EQ(pool_.GetPinCount(pages[0]), 0u);

    // The oldest page left RAM; the others are still resident.
    const auto reads_before = pool_.Statistics().disk_reads;
    for (std::size_t i = 1; i < pages.size(); ++i) {
        (void)ReadMarker(pages[i]);
    }
    EXPECT_EQ(pool_.Statistics().disk_reads, reads_before);
    EXPECT_EQ(ReadMarker(newcomer), 0x999u);
}

TEST_F(BufferPoolTest, FlushPageWritesWithoutEvicting) {
    PageId page_id = kInvalidPageId;
    auto data = pool_.NewPage(page_id);
    serialization::WriteU32(data, 0, 0x5555);
    pool_.UnpinPage(page_id, true);

    EXPECT_TRUE(pool_.FlushPage(page_id));
    EXPECT_FALSE(pool_.FlushPage(4242));  // not resident

    // Still in RAM: reading it causes no disk read.
    const auto reads_before = pool_.Statistics().disk_reads;
    EXPECT_EQ(ReadMarker(page_id), 0x5555u);
    EXPECT_EQ(pool_.Statistics().disk_reads, reads_before);
}

TEST_F(BufferPoolTest, FlushAllPagesPersistsEverything) {
    std::vector<PageId> pages;
    for (std::size_t i = 0; i < kFrames; ++i) {
        pages.push_back(MakePage(static_cast<std::uint32_t>(0x70 + i)));
    }
    pool_.FlushAllPages();

    // Read the pages straight off the disk, bypassing the pool entirely.
    std::array<std::byte, kPageSize> raw{};
    for (std::size_t i = 0; i < pages.size(); ++i) {
        disk_.ReadPage(pages[i], raw);
        EXPECT_EQ(serialization::ReadU32(raw, 0), 0x70u + i);
    }
}

TEST_F(BufferPoolTest, DeletePageFreesTheFrameAndTheDiskPage) {
    const PageId page_id = MakePage(1);
    const PageId freed = page_id;

    EXPECT_TRUE(pool_.DeletePage(freed));
    EXPECT_EQ(pool_.GetPinCount(freed), 0u);
    EXPECT_EQ(disk_.FreePageHead(), freed);

    // The page comes straight back from the free list.
    PageId reused = kInvalidPageId;
    (void)pool_.NewPage(reused);
    EXPECT_EQ(reused, freed);
    pool_.UnpinPage(reused, false);
}

TEST_F(BufferPoolTest, DeletePageRefusesWhilePinned) {
    PageId page_id = kInvalidPageId;
    (void)pool_.NewPage(page_id);

    EXPECT_FALSE(pool_.DeletePage(page_id));
    pool_.UnpinPage(page_id, false);
    EXPECT_TRUE(pool_.DeletePage(page_id));
}

// A workload larger than the pool: every page must still read back correctly,
// which only happens if eviction and write-back are both right.
TEST_F(BufferPoolTest, WorkloadLargerThanThePoolStaysCorrect) {
    constexpr std::uint32_t kPages = 60;
    std::vector<PageId> pages;
    for (std::uint32_t i = 0; i < kPages; ++i) {
        pages.push_back(MakePage(0xD000 + i));
    }

    for (std::uint32_t i = 0; i < kPages; ++i) {
        EXPECT_EQ(ReadMarker(pages[i]), 0xD000u + i);
    }
    EXPECT_GT(pool_.Statistics().evictions, 0u);
    EXPECT_TRUE(pool_.AllPagesUnpinned());
}

TEST_F(BufferPoolTest, DataSurvivesReopeningThroughANewPool) {
    std::vector<PageId> pages;
    for (std::uint32_t i = 0; i < 10; ++i) {
        pages.push_back(MakePage(0xE000 + i));
    }
    pool_.FlushAllPages();

    BufferPoolManager reopened(disk_, kFrames);
    for (std::uint32_t i = 0; i < pages.size(); ++i) {
        auto data = reopened.FetchPage(pages[i]);
        EXPECT_EQ(serialization::ReadU32(data, 0), 0xE000u + i);
        reopened.UnpinPage(pages[i], false);
    }
}

TEST_F(BufferPoolTest, RejectsAZeroFramePool) {
    EXPECT_THROW(BufferPoolManager(disk_, 0), StorageError);
}

// --- PageGuard -----------------------------------------------------------

TEST_F(BufferPoolTest, GuardUnpinsWhenItGoesOutOfScope) {
    PageId page_id = kInvalidPageId;
    {
        PageGuard guard = NewGuarded(pool_, page_id);
        EXPECT_EQ(pool_.GetPinCount(page_id), 1u);
        guard.MarkDirty();
    }
    EXPECT_EQ(pool_.GetPinCount(page_id), 0u);
    EXPECT_TRUE(pool_.AllPagesUnpinned());
}

TEST_F(BufferPoolTest, GuardUnpinsWhenAnExceptionUnwindsTheStack) {
    const PageId page_id = MakePage(0x1234);

    try {
        PageGuard guard = FetchGuarded(pool_, page_id);
        EXPECT_EQ(pool_.GetPinCount(page_id), 1u);
        throw QueryError("fallo simulado a mitad de una operación");
    } catch (const QueryError&) {
    }

    EXPECT_EQ(pool_.GetPinCount(page_id), 0u);
    EXPECT_TRUE(pool_.AllPagesUnpinned());
}

TEST_F(BufferPoolTest, GuardMarksThePageDirtySoChangesSurvive) {
    PageId page_id = kInvalidPageId;
    {
        PageGuard guard = NewGuarded(pool_, page_id);
        serialization::WriteU32(guard.Data(), 0, 0xABCD);
        guard.MarkDirty();
    }
    for (std::size_t i = 0; i < kFrames + 1; ++i) {
        (void)MakePage(static_cast<std::uint32_t>(i));
    }
    EXPECT_EQ(ReadMarker(page_id), 0xABCDu);
}

TEST_F(BufferPoolTest, MovedGuardUnpinsExactlyOnce) {
    PageId page_id = kInvalidPageId;
    {
        PageGuard first = NewGuarded(pool_, page_id);
        PageGuard second = std::move(first);
        EXPECT_EQ(pool_.GetPinCount(page_id), 1u);
    }
    EXPECT_EQ(pool_.GetPinCount(page_id), 0u);
}

}  // namespace
}  // namespace minidb
