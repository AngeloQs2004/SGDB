#include "minidb/storage/table_page.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include "minidb/catalog/schema.hpp"
#include "minidb/storage/record.hpp"

namespace minidb {
namespace {

std::vector<std::byte> MakeBytes(std::size_t size, std::uint8_t fill) {
    return std::vector<std::byte>(size, static_cast<std::byte>(fill));
}

class TablePageTest : public ::testing::Test {
protected:
    void SetUp() override { page_.Initialize(); }

    void TearDown() override { EXPECT_NO_THROW(page_.CheckInvariants()); }

    std::array<std::byte, kPageSize> buffer_{};
    TablePage page_{buffer_};
};

TEST_F(TablePageTest, InitializeProducesAnEmptyPage) {
    EXPECT_EQ(page_.GetPageType(), PageType::kTableData);
    EXPECT_EQ(page_.SlotCount(), 0u);
    EXPECT_EQ(page_.RecordCount(), 0u);
    EXPECT_EQ(page_.NextPageId(), kInvalidPageId);
    EXPECT_EQ(page_.FreeSpace(), kPageSize - TablePage::kHeaderSize);
}

TEST_F(TablePageTest, InsertAndReadBack) {
    const auto record = MakeBytes(40, 0xAB);
    const auto slot = page_.InsertRecord(record);

    ASSERT_TRUE(slot.has_value());
    EXPECT_EQ(*slot, 0u);
    EXPECT_EQ(page_.RecordCount(), 1u);

    const auto stored = page_.GetRecord(*slot);
    ASSERT_TRUE(stored.has_value());
    ASSERT_EQ(stored->size(), record.size());
    EXPECT_TRUE(std::equal(stored->begin(), stored->end(), record.begin()));
}

TEST_F(TablePageTest, SlotsAreNumberedInInsertionOrder) {
    for (std::uint8_t i = 0; i < 5; ++i) {
        const auto slot = page_.InsertRecord(MakeBytes(20, i));
        ASSERT_TRUE(slot.has_value());
        EXPECT_EQ(*slot, i);
    }
    EXPECT_EQ(page_.SlotCount(), 5u);
    EXPECT_EQ(page_.RecordCount(), 5u);
}

TEST_F(TablePageTest, ReadingAFreeOrOutOfRangeSlotReturnsNullopt) {
    const auto slot = page_.InsertRecord(MakeBytes(10, 1));
    ASSERT_TRUE(slot.has_value());

    EXPECT_TRUE(page_.DeleteRecord(*slot));
    EXPECT_FALSE(page_.GetRecord(*slot).has_value());
    EXPECT_FALSE(page_.GetRecord(99).has_value());
}

TEST_F(TablePageTest, DeleteIsIdempotentAndReported) {
    const auto slot = page_.InsertRecord(MakeBytes(10, 1));
    ASSERT_TRUE(slot.has_value());

    EXPECT_TRUE(page_.DeleteRecord(*slot));
    EXPECT_FALSE(page_.DeleteRecord(*slot));
    EXPECT_EQ(page_.RecordCount(), 0u);
}

// Space reuse is the difference between "Excelente" and "Bueno" for the storage
// criterion: a deleted slot must be handed to the next insert.
TEST_F(TablePageTest, DeletedSlotIsReusedByTheNextInsert) {
    const auto first = page_.InsertRecord(MakeBytes(30, 1));
    const auto second = page_.InsertRecord(MakeBytes(30, 2));
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());

    ASSERT_TRUE(page_.DeleteRecord(*first));
    const SlotId slots_before = page_.SlotCount();

    const auto reused = page_.InsertRecord(MakeBytes(30, 3));
    ASSERT_TRUE(reused.has_value());
    EXPECT_EQ(*reused, *first);                 // the hole was filled
    EXPECT_EQ(page_.SlotCount(), slots_before);  // the directory did not grow
}

TEST_F(TablePageTest, UpdateToASmallerRecordStaysInPlace) {
    const auto slot = page_.InsertRecord(MakeBytes(100, 1));
    ASSERT_TRUE(slot.has_value());

    EXPECT_TRUE(page_.UpdateRecord(*slot, MakeBytes(40, 2)));

    const auto stored = page_.GetRecord(*slot);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->size(), 40u);
    EXPECT_EQ((*stored)[0], std::byte{2});
}

TEST_F(TablePageTest, UpdateToALargerRecordSucceedsAndKeepsTheSlotId) {
    const auto slot = page_.InsertRecord(MakeBytes(40, 1));
    const auto neighbour = page_.InsertRecord(MakeBytes(40, 9));
    ASSERT_TRUE(slot.has_value());
    ASSERT_TRUE(neighbour.has_value());

    EXPECT_TRUE(page_.UpdateRecord(*slot, MakeBytes(300, 2)));

    const auto stored = page_.GetRecord(*slot);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->size(), 300u);
    EXPECT_EQ((*stored)[0], std::byte{2});

    // The neighbouring record is untouched.
    const auto other = page_.GetRecord(*neighbour);
    ASSERT_TRUE(other.has_value());
    EXPECT_EQ(other->size(), 40u);
    EXPECT_EQ((*other)[0], std::byte{9});
}

// A growing update that no longer fits in the gap must trigger compaction
// rather than fail, as long as the holes hold enough space.
TEST_F(TablePageTest, UpdateCompactsWhenTheGapIsExhausted) {
    std::vector<SlotId> slots;
    for (std::uint8_t i = 0; i < 20; ++i) {
        const auto slot = page_.InsertRecord(MakeBytes(200, i));
        ASSERT_TRUE(slot.has_value());
        slots.push_back(*slot);
    }
    // Free most of the page as holes, leaving the contiguous gap tiny.
    for (std::size_t i = 1; i < slots.size(); ++i) {
        ASSERT_TRUE(page_.DeleteRecord(slots[i]));
    }

    // Only reachable by reclaiming the holes.
    EXPECT_TRUE(page_.UpdateRecord(slots[0], MakeBytes(3000, 7)));

    const auto stored = page_.GetRecord(slots[0]);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->size(), 3000u);
    EXPECT_EQ((*stored)[0], std::byte{7});
}

// When the record truly does not fit, the page must be left exactly as it was
// so the caller can relocate the record to another page.
TEST_F(TablePageTest, FailedUpdateLeavesTheOriginalRecordIntact) {
    const auto slot = page_.InsertRecord(MakeBytes(50, 1));
    const auto filler = page_.InsertRecord(MakeBytes(3900, 2));
    ASSERT_TRUE(slot.has_value());
    ASSERT_TRUE(filler.has_value());

    EXPECT_FALSE(page_.UpdateRecord(*slot, MakeBytes(2000, 3)));

    const auto stored = page_.GetRecord(*slot);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->size(), 50u);
    EXPECT_EQ((*stored)[0], std::byte{1});
    EXPECT_EQ(page_.RecordCount(), 2u);

    const auto other = page_.GetRecord(*filler);
    ASSERT_TRUE(other.has_value());
    EXPECT_EQ(other->size(), 3900u);
}

TEST_F(TablePageTest, UpdatingAFreeSlotThrows) {
    const auto slot = page_.InsertRecord(MakeBytes(10, 1));
    ASSERT_TRUE(slot.has_value());
    ASSERT_TRUE(page_.DeleteRecord(*slot));

    EXPECT_THROW((void)page_.UpdateRecord(*slot, MakeBytes(10, 2)), StorageError);
    EXPECT_THROW((void)page_.UpdateRecord(42, MakeBytes(10, 2)), StorageError);
}

// The single most important property of Compact: the hash index maps primary
// keys to (page_id, slot_id), so renumbering slots would orphan every entry.
TEST_F(TablePageTest, CompactPreservesSlotIds) {
    std::vector<SlotId> slots;
    for (std::uint8_t i = 0; i < 10; ++i) {
        const auto slot = page_.InsertRecord(MakeBytes(100, i));
        ASSERT_TRUE(slot.has_value());
        slots.push_back(*slot);
    }
    // Punch holes at 1, 3, 5, 7, 9.
    for (std::size_t i = 1; i < slots.size(); i += 2) {
        ASSERT_TRUE(page_.DeleteRecord(slots[i]));
    }

    page_.Compact();

    for (std::size_t i = 0; i < slots.size(); i += 2) {
        const auto stored = page_.GetRecord(slots[i]);
        ASSERT_TRUE(stored.has_value()) << "el slot " << slots[i] << " se perdió al compactar";
        EXPECT_EQ(stored->size(), 100u);
        EXPECT_EQ((*stored)[0], static_cast<std::byte>(i));
    }
    for (std::size_t i = 1; i < slots.size(); i += 2) {
        EXPECT_FALSE(page_.GetRecord(slots[i]).has_value());
    }
    EXPECT_EQ(page_.RecordCount(), 5u);
}

TEST_F(TablePageTest, CompactReclaimsEveryHole) {
    for (std::uint8_t i = 0; i < 20; ++i) {
        ASSERT_TRUE(page_.InsertRecord(MakeBytes(150, i)).has_value());
    }
    for (SlotId i = 0; i < 19; ++i) {
        ASSERT_TRUE(page_.DeleteRecord(i));
    }
    const std::size_t free_before = page_.FreeSpace();

    page_.Compact();

    // 19 records of 150 bytes come back.
    EXPECT_EQ(page_.FreeSpace(), free_before + 19 * 150);
    EXPECT_EQ(page_.RecordCount(), 1u);
}

TEST_F(TablePageTest, CompactOnAnEmptyPageIsHarmless) {
    page_.Compact();
    EXPECT_EQ(page_.RecordCount(), 0u);
    EXPECT_EQ(page_.FreeSpace(), kPageSize - TablePage::kHeaderSize);
}

// The arithmetic from the physical design: with a 12-byte header, 4-byte slots
// and the 112-byte worst-case students record,
//   35 records -> 12 + 140 + 3920 = 4072 <= 4096   (fits)
//   36 records -> 12 + 144 + 4032 = 4188 >  4096   (does not)
TEST_F(TablePageTest, ExactlyThirtyFiveWorstCaseRecordsFit) {
    constexpr std::size_t kWorstCaseRecord = 112;

    int inserted = 0;
    while (page_.InsertRecord(MakeBytes(kWorstCaseRecord, 1)).has_value()) {
        ++inserted;
    }

    EXPECT_EQ(inserted, 35);
    EXPECT_EQ(page_.RecordCount(), 35u);
    EXPECT_EQ(TablePage::kHeaderSize + TablePage::kSlotSize * 35 + kWorstCaseRecord * 35, 4072u);
}

TEST_F(TablePageTest, ARecordOfMaximumSizeFitsInAnEmptyPage) {
    const auto slot = page_.InsertRecord(MakeBytes(TablePage::kMaxRecordSize, 1));
    ASSERT_TRUE(slot.has_value());
    EXPECT_EQ(page_.FreeSpace(), 0u);
    EXPECT_EQ(TablePage::kMaxRecordSize, 4080u);
}

TEST_F(TablePageTest, ARecordLargerThanTheMaximumIsRejected) {
    EXPECT_FALSE(page_.HasSpaceFor(TablePage::kMaxRecordSize + 1));
    EXPECT_FALSE(page_.InsertRecord(MakeBytes(TablePage::kMaxRecordSize + 1, 1)).has_value());
}

// The bound that makes "record too large for an empty page" unreachable in
// practice: 8 columns of VARCHAR(255) still leave room to spare.
TEST_F(TablePageTest, TheWidestLegalSchemaAlwaysFits) {
    std::vector<Column> columns;
    columns.push_back({"id", ColumnType::kInteger, 0, true});
    for (std::size_t i = 1; i < kMaxColumns; ++i) {
        columns.push_back({"c" + std::to_string(i), ColumnType::kVarchar,
                           static_cast<std::uint16_t>(kMaxVarcharLength), false});
    }
    const Schema schema(columns);

    EXPECT_LE(schema.MaxSerializedSize(), TablePage::kMaxRecordSize);
    EXPECT_EQ(schema.MaxSerializedSize(), 4u + 7u * (2u + 255u));
}

TEST_F(TablePageTest, FullPageReportsNoSpaceButStaysConsistent) {
    while (page_.InsertRecord(MakeBytes(500, 1)).has_value()) {
    }
    EXPECT_FALSE(page_.HasSpaceFor(500));
    EXPECT_GT(page_.RecordCount(), 0u);
}

TEST_F(TablePageTest, NextOccupiedSlotSkipsHoles) {
    for (std::uint8_t i = 0; i < 6; ++i) {
        ASSERT_TRUE(page_.InsertRecord(MakeBytes(20, i)).has_value());
    }
    ASSERT_TRUE(page_.DeleteRecord(0));
    ASSERT_TRUE(page_.DeleteRecord(1));
    ASSERT_TRUE(page_.DeleteRecord(4));

    EXPECT_EQ(page_.NextOccupiedSlot(0), std::optional<SlotId>{2});
    EXPECT_EQ(page_.NextOccupiedSlot(3), std::optional<SlotId>{3});
    EXPECT_EQ(page_.NextOccupiedSlot(4), std::optional<SlotId>{5});
    EXPECT_EQ(page_.NextOccupiedSlot(6), std::nullopt);
}

TEST_F(TablePageTest, NextPageIdRoundTrips) {
    page_.SetNextPageId(77);
    EXPECT_EQ(page_.NextPageId(), 77u);
}

// A long random-ish workload: the page must stay internally consistent and
// never leak space permanently.
TEST_F(TablePageTest, MixedWorkloadKeepsThePageConsistent) {
    std::vector<SlotId> live;

    for (int round = 0; round < 200; ++round) {
        const auto size = static_cast<std::size_t>(20 + (round * 37) % 300);
        const auto slot = page_.InsertRecord(MakeBytes(size, static_cast<std::uint8_t>(round)));
        if (slot.has_value()) {
            live.push_back(*slot);
        } else {
            // Page is full: drop half the records and compact.
            for (std::size_t i = 0; i < live.size(); i += 2) {
                page_.DeleteRecord(live[i]);
            }
            live.clear();
            page_.Compact();
        }
        page_.CheckInvariants();
    }

    // After freeing everything and compacting, the page is as good as new.
    for (SlotId i = 0; i < page_.SlotCount(); ++i) {
        page_.DeleteRecord(i);
    }
    page_.Compact();
    EXPECT_EQ(page_.RecordCount(), 0u);
    EXPECT_EQ(page_.FreeSpace(), kPageSize - TablePage::kHeaderSize - page_.SlotCount() * 4);
}

// --- Record serialisation ------------------------------------------------

Schema StudentsSchema() {
    return Schema({{"id", ColumnType::kInteger, 0, true},
                   {"name", ColumnType::kVarchar, 50, false},
                   {"age", ColumnType::kInteger, 0, false},
                   {"career", ColumnType::kVarchar, 50, false}});
}

TEST(RecordTest, RoundTripThroughAPage) {
    const Schema schema = StudentsSchema();
    const Record original(
        {std::int32_t{1}, std::string("Ana"), std::int32_t{20},
         std::string("Ciencia de la Computación")});
    original.Validate(schema);

    std::array<std::byte, kPageSize> buffer{};
    TablePage page(buffer);
    page.Initialize();

    std::vector<std::byte> bytes(original.SerializedSize(schema));
    original.SerializeTo(schema, bytes);
    const auto slot = page.InsertRecord(bytes);
    ASSERT_TRUE(slot.has_value());

    const auto stored = page.GetRecord(*slot);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(Record::DeserializeFrom(schema, *stored), original);
}

TEST(RecordTest, DemoRowOccupiesTheExpectedFortyOneBytes) {
    const Schema schema = StudentsSchema();
    const Record row({std::int32_t{1}, std::string("Ana"), std::int32_t{20},
                      std::string("Ciencia de la Computación")});

    // 4 + (2+3) + 4 + (2+26) = 41
    EXPECT_EQ(row.SerializedSize(schema), 41u);
    EXPECT_EQ(schema.MaxSerializedSize(), 112u);
}

TEST(RecordTest, RejectsWrongColumnCount) {
    const Schema schema = StudentsSchema();
    EXPECT_THROW(Record({std::int32_t{1}, std::string("Ana")}).Validate(schema), QueryError);
}

TEST(RecordTest, RejectsWrongTypes) {
    const Schema schema = StudentsSchema();
    EXPECT_THROW(Record({std::string("no soy un entero"), std::string("Ana"), std::int32_t{20},
                         std::string("X")})
                     .Validate(schema),
                 QueryError);
    EXPECT_THROW(Record({std::int32_t{1}, std::int32_t{2}, std::int32_t{20}, std::string("X")})
                     .Validate(schema),
                 QueryError);
}

// VARCHAR(n) limits bytes, not characters. 26 accented bytes must be measured
// as 26, and a 51-byte value must be rejected by a VARCHAR(50).
TEST(RecordTest, VarcharLimitIsMeasuredInBytes) {
    const Schema schema = StudentsSchema();

    const Record ok({std::int32_t{1}, std::string("Ana"), std::int32_t{20},
                     std::string(50, 'x')});
    EXPECT_NO_THROW(ok.Validate(schema));

    const Record too_long({std::int32_t{1}, std::string("Ana"), std::int32_t{20},
                           std::string(51, 'x')});
    EXPECT_THROW(too_long.Validate(schema), QueryError);

    // 26 accented bytes fit in VARCHAR(50); 25 accented characters would too,
    // but it is the byte count that is checked.
    const std::string accented = "Ciencia de la Computación";
    ASSERT_EQ(accented.size(), 26u);
    EXPECT_NO_THROW(
        Record({std::int32_t{1}, std::string("Ana"), std::int32_t{20}, accented}).Validate(schema));
}

TEST(RecordTest, NegativeIntegersRoundTrip) {
    const Schema schema = StudentsSchema();
    const Record row({std::int32_t{-42}, std::string("X"), std::int32_t{-1}, std::string("Y")});

    std::vector<std::byte> bytes(row.SerializedSize(schema));
    row.SerializeTo(schema, bytes);
    EXPECT_EQ(Record::DeserializeFrom(schema, bytes), row);
}

TEST(SchemaTest, RequiresExactlyOneIntegerPrimaryKey) {
    EXPECT_THROW(Schema({{"a", ColumnType::kInteger, 0, false}}), QueryError);
    EXPECT_THROW(Schema({{"a", ColumnType::kInteger, 0, true},
                         {"b", ColumnType::kInteger, 0, true}}),
                 QueryError);
    EXPECT_THROW(Schema({{"a", ColumnType::kVarchar, 10, true}}), QueryError);
    EXPECT_NO_THROW(Schema({{"a", ColumnType::kInteger, 0, true}}));
}

TEST(SchemaTest, RejectsDuplicateColumnsIgnoringCase) {
    EXPECT_THROW(Schema({{"id", ColumnType::kInteger, 0, true},
                         {"ID", ColumnType::kInteger, 0, false}}),
                 QueryError);
}

TEST(SchemaTest, FindColumnIsCaseInsensitive) {
    const Schema schema = StudentsSchema();
    EXPECT_EQ(schema.FindColumn("NAME"), std::optional<std::size_t>{1});
    EXPECT_EQ(schema.FindColumn("Career"), std::optional<std::size_t>{3});
    EXPECT_EQ(schema.FindColumn("inexistente"), std::nullopt);
    EXPECT_EQ(schema.PrimaryKeyIndex(), 0u);
}

TEST(SchemaTest, RejectsTooManyColumnsAndBadVarcharLengths) {
    std::vector<Column> too_many;
    too_many.push_back({"id", ColumnType::kInteger, 0, true});
    for (std::size_t i = 0; i <= kMaxColumns; ++i) {
        too_many.push_back({"c" + std::to_string(i), ColumnType::kInteger, 0, false});
    }
    EXPECT_THROW(Schema{too_many}, QueryError);

    EXPECT_THROW(Schema({{"id", ColumnType::kInteger, 0, true},
                         {"s", ColumnType::kVarchar, 0, false}}),
                 QueryError);
    EXPECT_THROW(Schema({{"id", ColumnType::kInteger, 0, true},
                         {"s", ColumnType::kVarchar, kMaxVarcharLength + 1, false}}),
                 QueryError);
}

TEST(ValueTest, ComparisonCoversEveryOperator) {
    const Value ten{std::int32_t{10}};
    const Value twenty{std::int32_t{20}};

    EXPECT_TRUE(CompareValues(ten, CompareOperator::kEqual, ten));
    EXPECT_TRUE(CompareValues(ten, CompareOperator::kNotEqual, twenty));
    EXPECT_TRUE(CompareValues(ten, CompareOperator::kLess, twenty));
    EXPECT_TRUE(CompareValues(ten, CompareOperator::kLessEqual, ten));
    EXPECT_TRUE(CompareValues(twenty, CompareOperator::kGreater, ten));
    EXPECT_TRUE(CompareValues(twenty, CompareOperator::kGreaterEqual, twenty));
    EXPECT_FALSE(CompareValues(ten, CompareOperator::kGreater, twenty));
}

TEST(ValueTest, StringsCompareByBytesAndMixedTypesThrow) {
    EXPECT_TRUE(CompareValues(Value{std::string("Ana")}, CompareOperator::kLess,
                              Value{std::string("Luis")}));
    EXPECT_THROW((void)CompareValues(Value{std::int32_t{1}}, CompareOperator::kEqual,
                                     Value{std::string("1")}),
                 QueryError);
}

}  // namespace
}  // namespace minidb
