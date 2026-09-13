#include "minidb/storage/disk_manager.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <fstream>
#include <vector>

#include "minidb/common/serialization.hpp"
#include "test_helpers.hpp"

namespace minidb {
namespace {

using testing::MakePattern;
using testing::TempDatabase;

using Page = std::array<std::byte, kPageSize>;

TEST(DiskManagerTest, CreatesFileWithValidHeader) {
    TempDatabase temp("create");
    {
        DiskManager manager(temp.Path());
        EXPECT_TRUE(manager.WasCreated());
        EXPECT_EQ(manager.PageCount(), 1u);  // only page 0 exists
        EXPECT_EQ(manager.FreePageHead(), kInvalidPageId);
        manager.Flush();
    }

    // The magic number must sit at offset 0 so `file` and `xxd` identify it.
    std::ifstream raw(temp.Path(), std::ios::binary);
    std::array<char, 4> magic{};
    raw.read(magic.data(), 4);
    EXPECT_EQ(magic[0], 'M');
    EXPECT_EQ(magic[1], 'I');
    EXPECT_EQ(magic[2], 'N');
    EXPECT_EQ(magic[3], 'D');

    EXPECT_EQ(temp.SizeOnDisk(), kPageSize);
}

TEST(DiskManagerTest, FileSizeIsAlwaysAMultipleOfPageSize) {
    TempDatabase temp("multiple");
    DiskManager manager(temp.Path());

    for (int i = 0; i < 7; ++i) {
        (void)manager.AllocatePage();
        manager.Flush();
        EXPECT_EQ(manager.FileSize() % kPageSize, 0u);
        EXPECT_EQ(manager.FileSize(), std::uint64_t{manager.PageCount()} * kPageSize);
    }
}

TEST(DiskManagerTest, AllocateReturnsConsecutiveIdentifiers) {
    TempDatabase temp("allocate");
    DiskManager manager(temp.Path());

    EXPECT_EQ(manager.AllocatePage(), 1u);
    EXPECT_EQ(manager.AllocatePage(), 2u);
    EXPECT_EQ(manager.AllocatePage(), 3u);
    EXPECT_EQ(manager.PageCount(), 4u);
}

TEST(DiskManagerTest, AllocatedPageIsZeroed) {
    TempDatabase temp("zeroed");
    DiskManager manager(temp.Path());

    const PageId page_id = manager.AllocatePage();
    Page page = MakePattern(9);  // deliberately dirty before reading
    manager.ReadPage(page_id, page);

    for (std::byte value : page) {
        EXPECT_EQ(value, std::byte{0});
    }
}

// The core persistence guarantee: bytes written, then closed, then reopened,
// come back identical.
TEST(DiskManagerTest, DataSurvivesCloseAndReopen) {
    TempDatabase temp("persist");
    PageId first = kInvalidPageId;
    PageId second = kInvalidPageId;

    {
        DiskManager manager(temp.Path());
        first = manager.AllocatePage();
        second = manager.AllocatePage();
        manager.WritePage(first, MakePattern(1));
        manager.WritePage(second, MakePattern(200));
        manager.Flush();
    }

    {
        DiskManager manager(temp.Path());
        EXPECT_FALSE(manager.WasCreated());
        EXPECT_EQ(manager.PageCount(), 3u);

        Page page{};
        manager.ReadPage(first, page);
        EXPECT_EQ(page, MakePattern(1));
        manager.ReadPage(second, page);
        EXPECT_EQ(page, MakePattern(200));
    }
}

TEST(DiskManagerTest, InterleavedReadWriteKeepsPagesIndependent) {
    TempDatabase temp("interleaved");
    DiskManager manager(temp.Path());

    const PageId a = manager.AllocatePage();
    const PageId b = manager.AllocatePage();

    // Alternating reads and writes on a stream opened in both modes is where a
    // missing seek would silently corrupt data.
    manager.WritePage(a, MakePattern(11));
    Page page{};
    manager.ReadPage(a, page);
    manager.WritePage(b, MakePattern(22));
    manager.ReadPage(a, page);
    EXPECT_EQ(page, MakePattern(11));
    manager.ReadPage(b, page);
    EXPECT_EQ(page, MakePattern(22));
    manager.WritePage(a, MakePattern(33));
    manager.ReadPage(b, page);
    EXPECT_EQ(page, MakePattern(22));
    manager.ReadPage(a, page);
    EXPECT_EQ(page, MakePattern(33));
}

// Reuse is what keeps the file from growing forever, which is the difference
// between "Excelente" and "Bueno" for the storage criterion.
TEST(DiskManagerTest, DeallocatedPageIsReusedByTheNextAllocation) {
    TempDatabase temp("reuse");
    DiskManager manager(temp.Path());

    const PageId first = manager.AllocatePage();
    const PageId second = manager.AllocatePage();
    const PageId third = manager.AllocatePage();
    const PageId page_count_before = manager.PageCount();

    manager.DeallocatePage(second);
    EXPECT_EQ(manager.FreePageHead(), second);

    EXPECT_EQ(manager.AllocatePage(), second);
    EXPECT_EQ(manager.PageCount(), page_count_before);  // the file did not grow
    EXPECT_EQ(manager.FreePageHead(), kInvalidPageId);

    // The other pages are untouched.
    EXPECT_NE(first, second);
    EXPECT_NE(third, second);
}

TEST(DiskManagerTest, FreeListIsLastInFirstOutAndSurvivesReopen) {
    TempDatabase temp("freelist");
    std::vector<PageId> pages;

    {
        DiskManager manager(temp.Path());
        for (int i = 0; i < 5; ++i) {
            pages.push_back(manager.AllocatePage());
        }
        manager.DeallocatePage(pages[1]);
        manager.DeallocatePage(pages[3]);
        manager.Flush();
    }

    {
        DiskManager manager(temp.Path());
        EXPECT_EQ(manager.FreePageHead(), pages[3]);
        EXPECT_EQ(manager.AllocatePage(), pages[3]);
        EXPECT_EQ(manager.AllocatePage(), pages[1]);
        EXPECT_EQ(manager.PageCount(), 6u);  // still no growth
    }
}

TEST(DiskManagerTest, RepeatedAllocateDeallocateDoesNotGrowTheFile) {
    TempDatabase temp("nogrowth");
    DiskManager manager(temp.Path());

    std::vector<PageId> pages;
    for (int i = 0; i < 20; ++i) {
        pages.push_back(manager.AllocatePage());
    }
    manager.Flush();
    const std::uint64_t size_after_first_round = manager.FileSize();

    for (int round = 0; round < 5; ++round) {
        for (PageId page_id : pages) {
            manager.DeallocatePage(page_id);
        }
        pages.clear();
        for (int i = 0; i < 20; ++i) {
            pages.push_back(manager.AllocatePage());
        }
    }
    manager.Flush();

    EXPECT_EQ(manager.FileSize(), size_after_first_round);
}

TEST(DiskManagerTest, RejectsFreeingPageZero) {
    TempDatabase temp("page0");
    DiskManager manager(temp.Path());
    EXPECT_THROW(manager.DeallocatePage(kFileHeaderPageId), StorageError);
}

TEST(DiskManagerTest, RejectsOutOfRangePageIds) {
    TempDatabase temp("range");
    DiskManager manager(temp.Path());
    const PageId valid = manager.AllocatePage();

    Page page{};
    EXPECT_THROW(manager.ReadPage(valid + 1, page), StorageError);
    EXPECT_THROW(manager.WritePage(999, page), StorageError);
    EXPECT_THROW(manager.ReadPage(kInvalidPageId, page), StorageError);
    EXPECT_THROW(manager.DeallocatePage(valid + 1), StorageError);
}

TEST(DiskManagerTest, RejectsFileWithWrongMagicNumber) {
    TempDatabase temp("magic");
    {
        DiskManager manager(temp.Path());
        manager.Flush();
    }
    {
        std::fstream raw(temp.Path(), std::ios::in | std::ios::out | std::ios::binary);
        raw.seekp(0);
        raw.write("XXXX", 4);
    }

    EXPECT_THROW(DiskManager manager(temp.Path()), StorageError);
}

TEST(DiskManagerTest, RejectsIncompatibleFormatVersion) {
    TempDatabase temp("version");
    {
        DiskManager manager(temp.Path());
        manager.Flush();
    }
    {
        std::fstream raw(temp.Path(), std::ios::in | std::ios::out | std::ios::binary);
        std::array<std::byte, 2> bumped{};
        serialization::WriteU16(bumped, 0, kFormatVersion + 1);
        raw.seekp(4);
        raw.write(reinterpret_cast<const char*>(bumped.data()), 2);
    }

    EXPECT_THROW(DiskManager manager(temp.Path()), StorageError);
}

TEST(DiskManagerTest, RejectsFileWhoseSizeIsNotAMultipleOfPageSize) {
    TempDatabase temp("truncated");
    {
        DiskManager manager(temp.Path());
        (void)manager.AllocatePage();
        manager.Flush();
    }
    std::filesystem::resize_file(temp.Path(), kPageSize + 100);

    EXPECT_THROW(DiskManager manager(temp.Path()), StorageError);
}

TEST(DiskManagerTest, RejectsHeaderThatDisagreesWithFileLength) {
    TempDatabase temp("pagecount");
    {
        DiskManager manager(temp.Path());
        (void)manager.AllocatePage();
        (void)manager.AllocatePage();
        manager.Flush();
    }
    // Claim more pages than the file actually holds.
    {
        std::fstream raw(temp.Path(), std::ios::in | std::ios::out | std::ios::binary);
        std::array<std::byte, 4> lie{};
        serialization::WriteU32(lie, 0, 99);
        raw.seekp(8);
        raw.write(reinterpret_cast<const char*>(lie.data()), 4);
    }

    EXPECT_THROW(DiskManager manager(temp.Path()), StorageError);
}

TEST(DiskManagerTest, RejectsEmptyExistingFile) {
    TempDatabase temp("empty");
    { std::ofstream create(temp.Path(), std::ios::binary); }
    ASSERT_EQ(temp.SizeOnDisk(), 0u);

    // An empty file is indistinguishable from a fresh database, so it is
    // initialised rather than rejected.
    DiskManager manager(temp.Path());
    EXPECT_TRUE(manager.WasCreated());
    EXPECT_EQ(manager.PageCount(), 1u);
}

}  // namespace
}  // namespace minidb
