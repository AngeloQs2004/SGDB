#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <sstream>
#include <string>
#include <random>
#include <utility>
#include <vector>

#include "minidb/buffer/buffer_pool_manager.hpp"
#include "minidb/index/bplus_tree.hpp"
#include "minidb/storage/disk_manager.hpp"

namespace minidb {
namespace {

class TempBTreeFile {
public:
    TempBTreeFile() {
        path_ = std::filesystem::temp_directory_path() /
                ("minidb_btree_" + std::to_string(std::random_device{}()) + ".db");
        std::filesystem::remove(path_);
    }
    ~TempBTreeFile() { std::filesystem::remove(path_); }
    const std::filesystem::path& Path() const { return path_; }
private:
    std::filesystem::path path_;
};

RecordId Rid(std::int32_t key) {
    const auto u = static_cast<std::uint32_t>(key);
    return RecordId{static_cast<PageId>(u % 1000003U), static_cast<SlotId>(u % 100U)};
}

void ExpectValid(const BPlusTree& tree) {
    std::string error;
    EXPECT_TRUE(tree.Validate(&error)) << error;
}

TEST(BPlusTreeTest, InsertAndSearchSingleLeaf) {
    TempBTreeFile temp;
    DiskManager disk(temp.Path());
    BufferPoolManager pool(disk, 8);
    const PageId header = BPlusTree::Create(pool, 4);
    BPlusTree tree(pool, header);

    for (std::int32_t key : {7, 1, 9, 3, 5}) tree.Insert(key, Rid(key));
    for (std::int32_t key : {1, 3, 5, 7, 9}) {
        ASSERT_TRUE(tree.Search(key).has_value());
        EXPECT_EQ(*tree.Search(key), Rid(key));
    }
    EXPECT_FALSE(tree.Search(99).has_value());
}

TEST(BPlusTreeTest, MassiveInsertionForcesLeafAndInternalSplits) {
    TempBTreeFile temp;
    DiskManager disk(temp.Path());
    BufferPoolManager pool(disk, 16);
    const PageId header = BPlusTree::Create(pool, 2);
    BPlusTree tree(pool, header);

    for (std::int32_t key = 1; key <= 500; ++key) tree.Insert(key, Rid(key));
    for (std::int32_t key = 1; key <= 500; ++key) {
        ASSERT_EQ(tree.Search(key), std::optional<RecordId>(Rid(key))) << key;
    }
    const auto stats = tree.GetStatistics();
    EXPECT_GT(stats.height, 2u);
    EXPECT_GT(stats.internal_count, 1u);
    EXPECT_GT(stats.split_count, 0u);
    EXPECT_EQ(stats.entry_count, 500u);
}

TEST(BPlusTreeTest, DescendingAndRandomOrderRemainSearchable) {
    TempBTreeFile temp;
    DiskManager disk(temp.Path());
    BufferPoolManager pool(disk, 12);
    const PageId header = BPlusTree::Create(pool, 3);
    BPlusTree tree(pool, header);

    std::vector<std::int32_t> keys;
    for (std::int32_t key = 1; key <= 300; ++key) keys.push_back(key);
    std::mt19937 rng(12345);
    std::shuffle(keys.begin(), keys.end(), rng);
    for (std::int32_t key : keys) tree.Insert(key, Rid(key));
    for (std::int32_t key = 1; key <= 300; ++key) EXPECT_EQ(tree.Search(key), Rid(key));
}

TEST(BPlusTreeTest, DuplicateKeysAreRejected) {
    TempBTreeFile temp;
    DiskManager disk(temp.Path());
    BufferPoolManager pool(disk, 8);
    BPlusTree tree(pool, BPlusTree::Create(pool, 3));
    tree.Insert(42, Rid(42));
    EXPECT_THROW(tree.Insert(42, Rid(43)), QueryError);
}

TEST(BPlusTreeTest, HeaderReopensTheSamePersistentTree) {
    TempBTreeFile temp;
    PageId header = kInvalidPageId;
    {
        DiskManager disk(temp.Path());
        BufferPoolManager pool(disk, 8);
        header = BPlusTree::Create(pool, 3);
        BPlusTree tree(pool, header);
        for (std::int32_t key = 1; key <= 100; ++key) tree.Insert(key, Rid(key));
        pool.FlushAllPages();
    }
    {
        DiskManager disk(temp.Path());
        BufferPoolManager pool(disk, 8);
        BPlusTree reopened(pool, header);
        for (std::int32_t key = 1; key <= 100; ++key) {
            EXPECT_EQ(reopened.Search(key), Rid(key));
        }
        EXPECT_EQ(reopened.GetStatistics().entry_count, 100u);
    }
}

TEST(BPlusTreeTest, BulkLoadUsesNormalBalancedInsertion) {
    TempBTreeFile temp;
    DiskManager disk(temp.Path());
    BufferPoolManager pool(disk, 8);
    BPlusTree tree(pool, BPlusTree::Create(pool, 4));
    std::vector<std::pair<std::int32_t, RecordId>> entries;
    for (std::int32_t key = 1; key <= 120; ++key) entries.emplace_back(key, Rid(key));
    tree.BulkLoad(entries);
    EXPECT_EQ(tree.GetStatistics().entry_count, entries.size());
    EXPECT_EQ(tree.Search(120), Rid(120));
}

TEST(BPlusTreeTest, EmptyTreeSearchAndValidate) {
    TempBTreeFile temp;
    DiskManager disk(temp.Path());
    BufferPoolManager pool(disk, 8);
    BPlusTree tree(pool, BPlusTree::Create(pool, 3));
    EXPECT_FALSE(tree.Search(0).has_value());
    EXPECT_FALSE(tree.Search(-1).has_value());
    const auto stats = tree.GetStatistics();
    EXPECT_EQ(stats.height, 1u);
    EXPECT_EQ(stats.entry_count, 0u);
    EXPECT_EQ(stats.split_count, 0u);
    ExpectValid(tree);
}

TEST(BPlusTreeTest, InvalidDegreeIsRejected) {
    TempBTreeFile temp;
    DiskManager disk(temp.Path());
    BufferPoolManager pool(disk, 8);
    EXPECT_THROW((void)BPlusTree::Create(pool, 0), QueryError);
    EXPECT_THROW((void)BPlusTree::Create(pool, 1), QueryError);
    EXPECT_THROW((void)BPlusTree::Create(pool, 300), QueryError);
    EXPECT_NO_THROW((void)BPlusTree::Create(pool, 200));
}

TEST(BPlusTreeTest, OpeningNonHeaderPageThrows) {
    TempBTreeFile temp;
    DiskManager disk(temp.Path());
    BufferPoolManager pool(disk, 8);
    const PageId header = BPlusTree::Create(pool, 3);
    EXPECT_THROW(BPlusTree(pool, header + 1), StorageError);
}

TEST(BPlusTreeTest, InvalidRecordIdIsRejected) {
    TempBTreeFile temp;
    DiskManager disk(temp.Path());
    BufferPoolManager pool(disk, 8);
    BPlusTree tree(pool, BPlusTree::Create(pool, 3));
    EXPECT_THROW(tree.Insert(1, RecordId{}), QueryError);
    EXPECT_EQ(tree.GetStatistics().entry_count, 0u);
}

TEST(BPlusTreeTest, NegativeAndExtremeKeys) {
    TempBTreeFile temp;
    DiskManager disk(temp.Path());
    BufferPoolManager pool(disk, 16);
    BPlusTree tree(pool, BPlusTree::Create(pool, 2));
    const std::int32_t lo = std::numeric_limits<std::int32_t>::min();
    const std::int32_t hi = std::numeric_limits<std::int32_t>::max();
    std::vector<std::int32_t> keys = {lo, -1000, -1, 0, 1, 1000, hi};
    for (std::int32_t k = -50; k <= 50; ++k) keys.push_back(k * 7 + 3);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    std::mt19937 rng(7);
    std::shuffle(keys.begin(), keys.end(), rng);
    for (std::int32_t k : keys) tree.Insert(k, Rid(k));
    for (std::int32_t k : keys) EXPECT_EQ(tree.Search(k), Rid(k)) << k;
    EXPECT_FALSE(tree.Search(2).has_value());
    ExpectValid(tree);
}

TEST(BPlusTreeTest, SplitHappensExactlyWhenLeafExceedsTwoTMinusOne) {
    TempBTreeFile temp;
    DiskManager disk(temp.Path());
    BufferPoolManager pool(disk, 8);
    const std::uint16_t t = 3;
    BPlusTree tree(pool, BPlusTree::Create(pool, t));
    for (std::int32_t k = 1; k <= 5; ++k) tree.Insert(k, Rid(k));
    EXPECT_EQ(tree.GetStatistics().split_count, 0u);
    EXPECT_EQ(tree.GetStatistics().height, 1u);
    tree.Insert(6, Rid(6));
    const auto stats = tree.GetStatistics();
    EXPECT_EQ(stats.split_count, 1u);
    EXPECT_EQ(stats.height, 2u);
    EXPECT_EQ(stats.leaf_count, 2u);
    ExpectValid(tree);
}

TEST(BPlusTreeTest, HeightGrowsLogarithmically) {
    TempBTreeFile temp;
    DiskManager disk(temp.Path());
    BufferPoolManager pool(disk, 32);
    BPlusTree tree(pool, BPlusTree::Create(pool, 4));
    std::uint32_t last_height = 1;
    for (std::int32_t k = 1; k <= 2000; ++k) {
        tree.Insert(k, Rid(k));
        const auto h = tree.GetStatistics().height;
        EXPECT_GE(h, last_height);
        EXPECT_LE(h, last_height + 1u);
        last_height = h;
    }
    EXPECT_LE(last_height, 6u);
    ExpectValid(tree);
}

TEST(BPlusTreeTest, SearchVisitsExactlyHeightNodes) {
    TempBTreeFile temp;
    DiskManager disk(temp.Path());
    BufferPoolManager pool(disk, 16);
    BPlusTree tree(pool, BPlusTree::Create(pool, 2));
    for (std::int32_t k = 1; k <= 300; ++k) tree.Insert(k, Rid(k));
    const auto height = tree.GetStatistics().height;
    for (std::int32_t k : {1, 150, 300, 999}) {
        std::uint32_t visited = 0;
        (void)tree.Search(k, &visited);
        EXPECT_EQ(visited, height) << k;
    }
}

TEST(BPlusTreeTest, TraceReportsSplitsAndNewRoot) {
    TempBTreeFile temp;
    DiskManager disk(temp.Path());
    BufferPoolManager pool(disk, 8);
    BPlusTree tree(pool, BPlusTree::Create(pool, 2));
    std::ostringstream log;
    tree.SetTrace(&log);
    for (std::int32_t k = 1; k <= 20; ++k) tree.Insert(k, Rid(k));
    EXPECT_NE(log.str().find("split hoja"), std::string::npos);
    EXPECT_NE(log.str().find("split interno"), std::string::npos);
    EXPECT_NE(log.str().find("nueva raiz"), std::string::npos);
}

TEST(BPlusTreeTest, InvariantsHoldForAllDegreesAndOrders) {
    for (const std::uint16_t t : std::initializer_list<std::uint16_t>{2, 3, 4, 8}) {
        for (int order = 0; order < 3; ++order) {
            TempBTreeFile temp;
            DiskManager disk(temp.Path());
            BufferPoolManager pool(disk, 32);
            BPlusTree tree(pool, BPlusTree::Create(pool, t));
            std::vector<std::int32_t> keys;
            for (std::int32_t k = 1; k <= 400; ++k) keys.push_back(k);
            if (order == 1) std::reverse(keys.begin(), keys.end());
            if (order == 2) { std::mt19937 rng(static_cast<std::uint32_t>(t)); std::shuffle(keys.begin(), keys.end(), rng); }
            for (std::int32_t k : keys) tree.Insert(k, Rid(k));
            std::string error;
            EXPECT_TRUE(tree.Validate(&error)) << "t=" << t << " order=" << order << ": " << error;
            EXPECT_EQ(tree.GetStatistics().entry_count, 400u);
            for (std::int32_t k = 1; k <= 400; ++k) ASSERT_EQ(tree.Search(k), Rid(k));
        }
    }
}

TEST(BPlusTreeTest, LargeRandomInsertionKeepsInvariants) {
    TempBTreeFile temp;
    DiskManager disk(temp.Path());
    BufferPoolManager pool(disk, 16);  // pool pequeño: fuerza evicciones
    BPlusTree tree(pool, BPlusTree::Create(pool, 16));
    std::vector<std::int32_t> keys;
    for (std::int32_t k = 0; k < 20000; ++k) keys.push_back(k * 3 - 5000);
    std::mt19937 rng(99);
    std::shuffle(keys.begin(), keys.end(), rng);
    for (std::int32_t k : keys) tree.Insert(k, Rid(k));
    ExpectValid(tree);
    EXPECT_EQ(tree.GetStatistics().entry_count, 20000u);
    for (std::size_t i = 0; i < keys.size(); i += 97) EXPECT_EQ(tree.Search(keys[i]), Rid(keys[i]));
    EXPECT_FALSE(tree.Search(2).has_value());
}

TEST(BPlusTreeTest, BulkLoadBuildsValidBalancedTreeForManySizes) {
    for (const std::int32_t t : {2, 3, 5}) {
        for (const std::int32_t n : {1, 2, 2 * t - 1, 2 * t, 2 * t + 1, 100, 997}) {
            TempBTreeFile temp;
            DiskManager disk(temp.Path());
            BufferPoolManager pool(disk, 32);
            BPlusTree tree(pool, BPlusTree::Create(pool, static_cast<std::uint16_t>(t)));
            std::vector<std::pair<std::int32_t, RecordId>> entries;
            for (std::int32_t k = 1; k <= n; ++k) entries.emplace_back(k, Rid(k));
            std::mt19937 rng(static_cast<std::uint32_t>(n));
            std::shuffle(entries.begin(), entries.end(), rng);
            tree.BulkLoad(entries);
            std::string error;
            EXPECT_TRUE(tree.Validate(&error)) << "t=" << t << " n=" << n << ": " << error;
            EXPECT_EQ(tree.GetStatistics().entry_count, static_cast<std::uint64_t>(n));
            for (std::int32_t k = 1; k <= n; ++k) ASSERT_EQ(tree.Search(k), Rid(k)) << t << "/" << n;
            EXPECT_FALSE(tree.Search(n + 1).has_value());
        }
    }
}

TEST(BPlusTreeTest, BulkLoadIsMoreCompactThanIncrementalInsert) {
    TempBTreeFile temp;
    DiskManager disk(temp.Path());
    BufferPoolManager pool(disk, 32);
    BPlusTree bulk(pool, BPlusTree::Create(pool, 4));
    BPlusTree incremental(pool, BPlusTree::Create(pool, 4));
    std::vector<std::pair<std::int32_t, RecordId>> entries;
    for (std::int32_t k = 1; k <= 1000; ++k) entries.emplace_back(k, Rid(k));
    bulk.BulkLoad(entries);
    for (const auto& [k, rid] : entries) incremental.Insert(k, rid);
    EXPECT_LE(bulk.GetStatistics().leaf_count, incremental.GetStatistics().leaf_count);
    EXPECT_EQ(bulk.GetStatistics().split_count, 0u);
    EXPECT_GT(incremental.GetStatistics().split_count, 0u);
}

TEST(BPlusTreeTest, BulkLoadRejectsDuplicatesAndInvalidRidsWithoutChangingTree) {
    TempBTreeFile temp;
    DiskManager disk(temp.Path());
    BufferPoolManager pool(disk, 16);
    BPlusTree tree(pool, BPlusTree::Create(pool, 3));

    std::vector<std::pair<std::int32_t, RecordId>> dup = {{1, Rid(1)}, {2, Rid(2)}, {1, Rid(3)}};
    EXPECT_THROW(tree.BulkLoad(dup), QueryError);
    std::vector<std::pair<std::int32_t, RecordId>> bad = {{1, Rid(1)}, {2, RecordId{}}};
    EXPECT_THROW(tree.BulkLoad(bad), QueryError);
    EXPECT_EQ(tree.GetStatistics().entry_count, 0u);
    EXPECT_FALSE(tree.Search(1).has_value());

    tree.Insert(10, Rid(10));
    std::vector<std::pair<std::int32_t, RecordId>> clash = {{9, Rid(9)}, {10, Rid(11)}};
    EXPECT_THROW(tree.BulkLoad(clash), QueryError);
    EXPECT_FALSE(tree.Search(9).has_value());
    ExpectValid(tree);
}

TEST(BPlusTreeTest, BulkLoadIntoNonEmptyTreeAndEmptyInput) {
    TempBTreeFile temp;
    DiskManager disk(temp.Path());
    BufferPoolManager pool(disk, 16);
    BPlusTree tree(pool, BPlusTree::Create(pool, 3));
    tree.BulkLoad({});
    EXPECT_EQ(tree.GetStatistics().entry_count, 0u);
    tree.Insert(500, Rid(500));
    std::vector<std::pair<std::int32_t, RecordId>> entries;
    for (std::int32_t k = 1; k <= 200; ++k) entries.emplace_back(k, Rid(k));
    tree.BulkLoad(entries);
    EXPECT_EQ(tree.GetStatistics().entry_count, 201u);
    EXPECT_EQ(tree.Search(500), Rid(500));
    EXPECT_EQ(tree.Search(77), Rid(77));
    ExpectValid(tree);
}

TEST(BPlusTreeTest, InsertAfterBulkLoadKeepsInvariantsAndPersists) {
    TempBTreeFile temp;
    PageId header = kInvalidPageId;
    {
        DiskManager disk(temp.Path());
        BufferPoolManager pool(disk, 16);
        header = BPlusTree::Create(pool, 3);
        BPlusTree tree(pool, header);
        std::vector<std::pair<std::int32_t, RecordId>> entries;
        for (std::int32_t k = 2; k <= 600; k += 2) entries.emplace_back(k, Rid(k));
        tree.BulkLoad(entries);
        for (std::int32_t k = 1; k <= 599; k += 2) tree.Insert(k, Rid(k));
        ExpectValid(tree);
        pool.FlushAllPages();
    }
    DiskManager disk(temp.Path());
    BufferPoolManager pool(disk, 16);
    BPlusTree reopened(pool, header);
    ExpectValid(reopened);
    EXPECT_EQ(reopened.GetStatistics().entry_count, 600u);
    for (std::int32_t k = 1; k <= 600; ++k) {
        EXPECT_EQ(reopened.Search(k), Rid(k));
    }
}

}  // namespace
}  // namespace minidb
