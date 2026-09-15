#include "minidb/storage/table_heap.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "minidb/catalog/catalog.hpp"
#include "test_helpers.hpp"

namespace minidb {
namespace {

using testing::TempDatabase;

Schema StudentsSchema() {
    return Schema({{"id", ColumnType::kInteger, 0, true},
                   {"name", ColumnType::kVarchar, 50, false},
                   {"age", ColumnType::kInteger, 0, false},
                   {"career", ColumnType::kVarchar, 50, false}});
}

Record Student(std::int32_t id, const std::string& name, std::int32_t age,
               const std::string& career) {
    return Record({id, name, age, career});
}

/// Assembles the whole stack over one file, mirroring how Database will wire it
/// together: disk, then pool, then catalog on page 1, then the heap.
class Stack {
public:
    explicit Stack(const std::filesystem::path& path, std::size_t frames = 8) {
        disk_ = std::make_unique<DiskManager>(path);
        pool_ = std::make_unique<BufferPoolManager>(*disk_, frames);
        const bool creating = disk_->WasCreated();
        catalog_ = std::make_unique<Catalog>(*pool_, creating);
        if (creating) {
            const PageId first = TableHeap::CreateFirstPage(*pool_);
            catalog_->CreateTable("students", StudentsSchema(), first, kInvalidPageId);
        }
        heap_ = std::make_unique<TableHeap>(*pool_, *catalog_);
    }

    void Close() {
        catalog_->Flush();
        pool_->FlushAllPages();
    }

    DiskManager& Disk() { return *disk_; }
    BufferPoolManager& Pool() { return *pool_; }
    Catalog& GetCatalog() { return *catalog_; }
    TableHeap& Heap() { return *heap_; }

private:
    std::unique_ptr<DiskManager> disk_;
    std::unique_ptr<BufferPoolManager> pool_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<TableHeap> heap_;
};

// --- Catalog -------------------------------------------------------------

TEST(CatalogTest, SchemaAndPointersSurviveReopen) {
    TempDatabase temp("catalog");
    RecordId rid{};

    {
        Stack stack(temp.Path());
        rid = stack.Heap().InsertRecord(Student(1, "Ana", 20, "Ciencia de la Computación"));
        stack.GetCatalog().IncrementRecordCount();
        stack.Close();
    }

    {
        Stack stack(temp.Path());
        const Catalog& catalog = stack.GetCatalog();

        EXPECT_TRUE(catalog.HasTable());
        EXPECT_EQ(catalog.TableName(), "students");
        EXPECT_EQ(catalog.RecordCount(), 1u);

        const Schema& schema = catalog.GetSchema();
        ASSERT_EQ(schema.ColumnCount(), 4u);
        EXPECT_EQ(schema.GetColumn(0).name, "id");
        EXPECT_TRUE(schema.GetColumn(0).is_primary_key);
        EXPECT_EQ(schema.GetColumn(0).type, ColumnType::kInteger);
        EXPECT_EQ(schema.GetColumn(1).name, "name");
        EXPECT_EQ(schema.GetColumn(1).type, ColumnType::kVarchar);
        EXPECT_EQ(schema.GetColumn(1).max_length, 50u);
        EXPECT_EQ(schema.GetColumn(3).name, "career");
        EXPECT_EQ(schema.PrimaryKeyIndex(), 0u);

        const auto record = stack.Heap().GetRecord(rid);
        ASSERT_TRUE(record.has_value());
        EXPECT_EQ(ValueToString(record->GetValue(3)), "Ciencia de la Computación");
    }
}

TEST(CatalogTest, CatalogAlwaysLandsOnPageOne) {
    TempDatabase temp("page1");
    Stack stack(temp.Path());
    EXPECT_EQ(kCatalogPageId, 1u);
    EXPECT_EQ(stack.GetCatalog().FirstTablePageId(), 2u);  // right after the catalog
}

TEST(CatalogTest, RejectsASecondTableAndUnknownNames) {
    TempDatabase temp("secondtable");
    Stack stack(temp.Path());

    EXPECT_THROW(stack.GetCatalog().CreateTable("otra", StudentsSchema(), 5, 6), QueryError);
    EXPECT_NO_THROW(stack.GetCatalog().RequireTable("students"));
    EXPECT_NO_THROW(stack.GetCatalog().RequireTable("STUDENTS"));  // case-insensitive
    EXPECT_THROW(stack.GetCatalog().RequireTable("profesores"), QueryError);
}

TEST(CatalogTest, RecordCountCannotGoNegative) {
    TempDatabase temp("counter");
    Stack stack(temp.Path());
    EXPECT_THROW(stack.GetCatalog().DecrementRecordCount(), StorageError);
}

// --- TableHeap -----------------------------------------------------------

TEST(TableHeapTest, InsertAndReadBack) {
    TempDatabase temp("insert");
    Stack stack(temp.Path());

    const Record ana = Student(1, "Ana", 20, "Ciencia de la Computación");
    const RecordId rid = stack.Heap().InsertRecord(ana);

    const auto stored = stack.Heap().GetRecord(rid);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(*stored, ana);
    EXPECT_TRUE(stack.Pool().AllPagesUnpinned());
}

TEST(TableHeapTest, InsertManyRecordsSpansSeveralPages) {
    TempDatabase temp("manypages");
    Stack stack(temp.Path());

    constexpr int kRecords = 500;
    for (int i = 0; i < kRecords; ++i) {
        (void)stack.Heap().InsertRecord(
            Student(i, "Estudiante " + std::to_string(i), 18 + i % 10, "Ingeniería de Sistemas"));
    }

    EXPECT_GT(stack.Heap().PageCountInChain(), 1u);
    EXPECT_EQ(stack.Heap().CountRecordsByScan(), kRecords);
    EXPECT_TRUE(stack.Pool().AllPagesUnpinned());
}

TEST(TableHeapTest, ScanVisitsEveryLiveRecordExactlyOnce) {
    TempDatabase temp("scan");
    Stack stack(temp.Path());

    std::set<std::int32_t> expected;
    for (std::int32_t i = 0; i < 300; ++i) {
        (void)stack.Heap().InsertRecord(Student(i, "N" + std::to_string(i), 20, "C"));
        expected.insert(i);
    }

    std::set<std::int32_t> seen;
    auto it = stack.Heap().Begin();
    while (const auto row = it.Next()) {
        const auto id = std::get<std::int32_t>(row->second.GetValue(0));
        EXPECT_TRUE(seen.insert(id).second) << "el registro " << id << " apareció dos veces";
    }
    EXPECT_EQ(seen, expected);
}

TEST(TableHeapTest, DeletedRecordsDisappearFromScansAndReads) {
    TempDatabase temp("delete");
    Stack stack(temp.Path());

    std::vector<RecordId> rids;
    for (std::int32_t i = 0; i < 100; ++i) {
        rids.push_back(stack.Heap().InsertRecord(Student(i, "N", 20, "C")));
    }
    for (std::size_t i = 0; i < rids.size(); i += 2) {
        EXPECT_TRUE(stack.Heap().DeleteRecord(rids[i]));
    }

    EXPECT_EQ(stack.Heap().CountRecordsByScan(), 50u);
    EXPECT_FALSE(stack.Heap().GetRecord(rids[0]).has_value());
    EXPECT_TRUE(stack.Heap().GetRecord(rids[1]).has_value());
    EXPECT_FALSE(stack.Heap().DeleteRecord(rids[0]));  // already gone
}

TEST(TableHeapTest, UpdateInPlaceKeepsTheSameRecordId) {
    TempDatabase temp("updateinplace");
    Stack stack(temp.Path());

    const RecordId rid =
        stack.Heap().InsertRecord(Student(1, "Ana", 20, "Ciencia de la Computación"));
    const Record updated = Student(1, "Ana", 21, "Ciencia de la Computación");

    const RecordId after = stack.Heap().UpdateRecord(rid, updated);

    EXPECT_EQ(after, rid);
    EXPECT_EQ(stack.Heap().GetRecord(rid), updated);
}

TEST(TableHeapTest, UpdateToAShorterRecordKeepsTheSameRecordId) {
    TempDatabase temp("updateshorter");
    Stack stack(temp.Path());

    const RecordId rid = stack.Heap().InsertRecord(Student(1, std::string(40, 'x'), 20,
                                                           std::string(40, 'y')));
    const RecordId after = stack.Heap().UpdateRecord(rid, Student(1, "A", 20, "B"));

    EXPECT_EQ(after, rid);
    const auto stored = stack.Heap().GetRecord(rid);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(ValueToString(stored->GetValue(1)), "A");
}

// When a record grows past what its page can hold it must move, and the caller
// has to learn about the new location or the index would be left dangling.
TEST(TableHeapTest, UpdateRelocatesAndReportsANewRecordId) {
    TempDatabase temp("relocate");
    Stack stack(temp.Path());

    // Keep inserting short records until the table spills onto a second page.
    // The first page is then genuinely full and holds no holes, so a record on
    // it has nowhere to grow.
    std::vector<RecordId> rids;
    std::int32_t next_id = 0;
    while (stack.Heap().PageCountInChain() < 2) {
        rids.push_back(stack.Heap().InsertRecord(Student(next_id++, "n", 20, "c")));
    }
    ASSERT_GT(rids.size(), 100u);

    const RecordId target = rids.front();
    ASSERT_EQ(target.page_id, stack.GetCatalog().FirstTablePageId());
    const Record grown =
        Student(std::get<std::int32_t>(stack.Heap().GetRecord(target)->GetValue(0)),
                std::string(50, 'A'), 20, std::string(50, 'B'));
    const RecordId after = stack.Heap().UpdateRecord(target, grown);

    EXPECT_NE(after, target) << "el registro debería haberse reubicado";
    EXPECT_FALSE(stack.Heap().GetRecord(target).has_value()) << "quedó una copia antigua";
    EXPECT_EQ(stack.Heap().GetRecord(after), grown);
    EXPECT_EQ(stack.Heap().CountRecordsByScan(), rids.size());
}

TEST(TableHeapTest, UpdateRejectsAValueThatBreaksTheSchema) {
    TempDatabase temp("badupdate");
    Stack stack(temp.Path());

    const RecordId rid = stack.Heap().InsertRecord(Student(1, "Ana", 20, "C"));
    EXPECT_THROW((void)stack.Heap().UpdateRecord(rid, Record({std::int32_t{1}})), QueryError);
    EXPECT_THROW((void)stack.Heap().InsertRecord(Student(2, std::string(51, 'x'), 20, "C")),
                 QueryError);
}

// Space that DELETE frees must come back into use. If it did not, the file
// would grow without bound across delete/insert cycles.
TEST(TableHeapTest, FileDoesNotGrowAfterDeleteAndReinsert) {
    TempDatabase temp("nogrowth");
    Stack stack(temp.Path());

    constexpr int kRecords = 200;
    std::vector<RecordId> rids;
    for (std::int32_t i = 0; i < kRecords; ++i) {
        rids.push_back(stack.Heap().InsertRecord(
            Student(i, "Estudiante " + std::to_string(i), 20, "Ingeniería de Software")));
    }
    stack.Close();
    const std::uint64_t size_after_first_load = stack.Disk().FileSize();
    const std::uint32_t pages_after_first_load = stack.Disk().PageCount();
    ASSERT_GT(pages_after_first_load, 3u);

    for (int round = 0; round < 3; ++round) {
        for (RecordId rid : rids) {
            EXPECT_TRUE(stack.Heap().DeleteRecord(rid));
        }
        EXPECT_EQ(stack.Heap().CountRecordsByScan(), 0u);

        rids.clear();
        for (std::int32_t i = 0; i < kRecords; ++i) {
            rids.push_back(stack.Heap().InsertRecord(
                Student(i, "Estudiante " + std::to_string(i), 20, "Ingeniería de Software")));
        }
        EXPECT_EQ(stack.Heap().CountRecordsByScan(), kRecords);
    }
    stack.Close();

    EXPECT_EQ(stack.Disk().PageCount(), pages_after_first_load);
    EXPECT_EQ(stack.Disk().FileSize(), size_after_first_load);
}

// Deleting everything and inserting a different, larger workload must reuse the
// emptied pages rather than append new ones.
TEST(TableHeapTest, EmptiedPagesAreReusedByLaterInserts) {
    TempDatabase temp("reusepages");
    Stack stack(temp.Path());

    std::vector<RecordId> rids;
    for (std::int32_t i = 0; i < 300; ++i) {
        rids.push_back(stack.Heap().InsertRecord(Student(i, "n", 20, "c")));
    }
    const std::uint32_t pages_before = stack.Heap().PageCountInChain();

    for (RecordId rid : rids) {
        EXPECT_TRUE(stack.Heap().DeleteRecord(rid));
    }
    for (std::int32_t i = 0; i < 300; ++i) {
        (void)stack.Heap().InsertRecord(Student(i, "n", 20, "c"));
    }

    EXPECT_EQ(stack.Heap().PageCountInChain(), pages_before);
    EXPECT_EQ(stack.Heap().CountRecordsByScan(), 300u);
}

TEST(TableHeapTest, CatalogCounterAgreesWithAFullScan) {
    TempDatabase temp("counteragrees");
    Stack stack(temp.Path());

    std::vector<RecordId> rids;
    for (std::int32_t i = 0; i < 120; ++i) {
        rids.push_back(stack.Heap().InsertRecord(Student(i, "n", 20, "c")));
        stack.GetCatalog().IncrementRecordCount();
    }
    for (std::size_t i = 0; i < 40; ++i) {
        EXPECT_TRUE(stack.Heap().DeleteRecord(rids[i]));
        stack.GetCatalog().DecrementRecordCount();
    }

    EXPECT_EQ(stack.GetCatalog().RecordCount(), stack.Heap().CountRecordsByScan());
    EXPECT_EQ(stack.GetCatalog().RecordCount(), 80u);
}

TEST(TableHeapTest, RecordsSurviveCloseAndReopen) {
    TempDatabase temp("persist");
    std::vector<RecordId> rids;

    {
        Stack stack(temp.Path());
        for (std::int32_t i = 0; i < 250; ++i) {
            rids.push_back(stack.Heap().InsertRecord(
                Student(i, "Estudiante " + std::to_string(i), 18 + i % 12, "Ingeniería")));
            stack.GetCatalog().IncrementRecordCount();
        }
        stack.Close();
    }

    {
        Stack stack(temp.Path());
        EXPECT_EQ(stack.Heap().CountRecordsByScan(), 250u);
        EXPECT_EQ(stack.GetCatalog().RecordCount(), 250u);

        for (std::int32_t i = 0; i < 250; ++i) {
            const auto stored = stack.Heap().GetRecord(rids[static_cast<std::size_t>(i)]);
            ASSERT_TRUE(stored.has_value()) << "se perdió el registro " << i;
            EXPECT_EQ(std::get<std::int32_t>(stored->GetValue(0)), i);
            EXPECT_EQ(ValueToString(stored->GetValue(1)), "Estudiante " + std::to_string(i));
        }
    }
}

// The heap must work with a pool far smaller than the table, since that is the
// whole point of having a buffer pool.
TEST(TableHeapTest, WorksWithAPoolMuchSmallerThanTheTable) {
    TempDatabase temp("smallpool");
    Stack stack(temp.Path(), /*frames=*/3);

    for (std::int32_t i = 0; i < 400; ++i) {
        (void)stack.Heap().InsertRecord(Student(i, "Estudiante " + std::to_string(i), 20,
                                                "Ciencia de la Computación"));
    }

    EXPECT_EQ(stack.Heap().CountRecordsByScan(), 400u);
    EXPECT_GT(stack.Pool().Statistics().evictions, 0u);
    EXPECT_TRUE(stack.Pool().AllPagesUnpinned());
}

TEST(TableHeapTest, ReadingAnInvalidRecordIdIsSafe) {
    TempDatabase temp("badrid");
    Stack stack(temp.Path());

    EXPECT_FALSE(stack.Heap().GetRecord(RecordId{}).has_value());
    EXPECT_FALSE(stack.Heap().GetRecord(RecordId{stack.GetCatalog().FirstTablePageId(), 999})
                     .has_value());
    EXPECT_FALSE(stack.Heap().DeleteRecord(RecordId{}));
}

// Every operation must leave the pool with nothing pinned; a leak here would
// exhaust the pool a few statements later.
TEST(TableHeapTest, NoOperationLeaksAPin) {
    TempDatabase temp("pins");
    Stack stack(temp.Path(), /*frames=*/4);

    const RecordId rid = stack.Heap().InsertRecord(Student(1, "Ana", 20, "C"));
    EXPECT_TRUE(stack.Pool().AllPagesUnpinned());

    (void)stack.Heap().GetRecord(rid);
    EXPECT_TRUE(stack.Pool().AllPagesUnpinned());

    (void)stack.Heap().UpdateRecord(rid, Student(1, "Ana María", 21, "C"));
    EXPECT_TRUE(stack.Pool().AllPagesUnpinned());

    (void)stack.Heap().CountRecordsByScan();
    EXPECT_TRUE(stack.Pool().AllPagesUnpinned());

    EXPECT_TRUE(stack.Heap().DeleteRecord(RecordId{rid.page_id, rid.slot_id}) ||
                stack.Heap().CountRecordsByScan() == 1u);
    EXPECT_TRUE(stack.Pool().AllPagesUnpinned());
}

}  // namespace
}  // namespace minidb
