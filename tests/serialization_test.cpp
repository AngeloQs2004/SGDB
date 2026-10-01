#include "minidb/common/serialization.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <limits>
#include <string>

#include "minidb/common/constants.hpp"
#include "minidb/common/types.hpp"

namespace minidb {
namespace {

using serialization::ReadI32;
using serialization::ReadString;
using serialization::ReadU16;
using serialization::ReadU32;
using serialization::ReadU64;
using serialization::ReadU8;
using serialization::WriteI32;
using serialization::WriteString;
using serialization::WriteU16;
using serialization::WriteU32;
using serialization::WriteU64;
using serialization::WriteU8;

class SerializationTest : public ::testing::Test {
protected:
    std::array<std::byte, kPageSize> page_{};
    std::span<std::byte> buffer_{page_};
    std::span<const std::byte> const_buffer_{page_};
};

TEST_F(SerializationTest, UnsignedRoundTrip) {
    WriteU8(buffer_, 0, 0xAB);
    WriteU16(buffer_, 1, 0xBEEF);
    WriteU32(buffer_, 3, 0xDEADBEEF);
    WriteU64(buffer_, 7, 0x0123456789ABCDEFull);

    EXPECT_EQ(ReadU8(const_buffer_, 0), 0xAB);
    EXPECT_EQ(ReadU16(const_buffer_, 1), 0xBEEF);
    EXPECT_EQ(ReadU32(const_buffer_, 3), 0xDEADBEEFu);
    EXPECT_EQ(ReadU64(const_buffer_, 7), 0x0123456789ABCDEFull);
}

TEST_F(SerializationTest, SignedRoundTripCoversNegativeAndExtremes) {
    WriteI32(buffer_, 0, -1);
    WriteI32(buffer_, 4, std::numeric_limits<std::int32_t>::min());
    WriteI32(buffer_, 8, std::numeric_limits<std::int32_t>::max());
    WriteI32(buffer_, 12, 0);

    EXPECT_EQ(ReadI32(const_buffer_, 0), -1);
    EXPECT_EQ(ReadI32(const_buffer_, 4), std::numeric_limits<std::int32_t>::min());
    EXPECT_EQ(ReadI32(const_buffer_, 8), std::numeric_limits<std::int32_t>::max());
    EXPECT_EQ(ReadI32(const_buffer_, 12), 0);
}

TEST_F(SerializationTest, ByteOrderIsLittleEndian) {
    WriteU32(buffer_, 0, 0x44494E4D);  // "MIND" reversed, so bytes read M I N D

    EXPECT_EQ(std::to_integer<std::uint8_t>(page_[0]), 0x4D);  // 'M'
    EXPECT_EQ(std::to_integer<std::uint8_t>(page_[1]), 0x4E);  // 'N'
    EXPECT_EQ(std::to_integer<std::uint8_t>(page_[2]), 0x49);  // 'I'
    EXPECT_EQ(std::to_integer<std::uint8_t>(page_[3]), 0x44);  // 'D'
}

TEST_F(SerializationTest, WritesAtArbitraryOffsetsDoNotOverlap) {
    WriteU32(buffer_, 100, 0x11111111);
    WriteU32(buffer_, 104, 0x22222222);

    EXPECT_EQ(ReadU32(const_buffer_, 100), 0x11111111u);
    EXPECT_EQ(ReadU32(const_buffer_, 104), 0x22222222u);
}

TEST_F(SerializationTest, StringRoundTrip) {
    std::size_t written = WriteString(buffer_, 10, "Ana");
    EXPECT_EQ(written, 2u + 3u);

    std::size_t read = 0;
    EXPECT_EQ(ReadString(const_buffer_, 10, &read), "Ana");
    EXPECT_EQ(read, written);
}

TEST_F(SerializationTest, EmptyStringRoundTrip) {
    std::size_t written = WriteString(buffer_, 0, "");
    EXPECT_EQ(written, 2u);
    EXPECT_EQ(ReadString(const_buffer_, 0), "");
}

TEST_F(SerializationTest, MaximumVarcharRoundTrip) {
    const std::string value(kMaxVarcharLength, 'x');
    const std::size_t written = WriteString(buffer_, 0, value);

    EXPECT_EQ(written, 2u + kMaxVarcharLength);
    EXPECT_EQ(ReadString(const_buffer_, 0), value);
}

// VARCHAR(n) counts UTF-8 bytes, not characters. The demo data is accented
// Spanish, so this distinction is load-bearing.
TEST_F(SerializationTest, AccentedStringIsMeasuredInBytes) {
    const std::string career = "Ciencia de la Computación";
    ASSERT_EQ(career.size(), 26u);  // 25 characters, 26 bytes

    WriteString(buffer_, 0, career);
    EXPECT_EQ(ReadU16(const_buffer_, 0), 26u);
    EXPECT_EQ(ReadString(const_buffer_, 0), career);
}

TEST_F(SerializationTest, ConsecutiveStringsCanBeWalked) {
    std::size_t offset = 0;
    offset += WriteString(buffer_, offset, "María");
    offset += WriteString(buffer_, offset, "Ingeniería de Software");

    std::size_t cursor = 0;
    std::size_t consumed = 0;
    EXPECT_EQ(ReadString(const_buffer_, cursor, &consumed), "María");
    cursor += consumed;
    EXPECT_EQ(ReadString(const_buffer_, cursor, &consumed), "Ingeniería de Software");
    cursor += consumed;
    EXPECT_EQ(cursor, offset);
}

TEST_F(SerializationTest, WritingPastTheEndThrows) {
    EXPECT_THROW(WriteU32(buffer_, kPageSize - 3, 1), StorageError);
    EXPECT_THROW(WriteU64(buffer_, kPageSize, 1), StorageError);
    EXPECT_THROW(WriteString(buffer_, kPageSize - 4, "demasiado largo"), StorageError);
}

TEST_F(SerializationTest, ReadingPastTheEndThrows) {
    EXPECT_THROW((void)ReadU32(const_buffer_, kPageSize - 3), StorageError);
    EXPECT_THROW((void)ReadU16(const_buffer_, kPageSize), StorageError);
}

// A length prefix that claims more bytes than the page holds must be rejected
// rather than read out of bounds. This is the shape a corrupt page takes.
TEST_F(SerializationTest, CorruptLengthPrefixThrows) {
    WriteU16(buffer_, kPageSize - 4, 0xFFFF);
    EXPECT_THROW((void)ReadString(const_buffer_, kPageSize - 4), StorageError);
}

TEST_F(SerializationTest, WriteAtLastValidOffsetSucceeds) {
    WriteU32(buffer_, kPageSize - 4, 0x12345678);
    EXPECT_EQ(ReadU32(const_buffer_, kPageSize - 4), 0x12345678u);
}

}  // namespace
}  // namespace minidb
