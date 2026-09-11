#pragma once

#include <cstddef>
#include <cstdint>

namespace minidb {

/// Every page in the database file is exactly this many bytes. The whole
/// physical design in ARCHITECTURE.md is derived from this constant.
inline constexpr std::size_t kPageSize = 4096;

/// Magic number stored at offset 0 of page 0. The value is chosen so that,
/// written little-endian like everything else, the first four bytes of the file
/// read as the ASCII characters 'M' 'I' 'N' 'D'. Page 0 is the only page that
/// does not begin with a page_type byte, precisely so that `file` and `xxd` can
/// identify the database at a glance.
inline constexpr std::uint32_t kMagicNumber = 0x444E494D;

/// Bumped whenever the on-disk layout changes incompatibly.
inline constexpr std::uint16_t kFormatVersion = 1;

/// Page 0 holds the file header, page 1 holds the catalog. Both are created
/// when the database file is initialised.
inline constexpr std::uint32_t kFileHeaderPageId = 0;
inline constexpr std::uint32_t kCatalogPageId = 1;

/// Largest record the slotted page layout can hold: a whole page minus its
/// 12-byte header and one 4-byte slot. TablePage re-exports this value and
/// static_asserts that its own layout constants agree with it.
inline constexpr std::size_t kMaxRecordSize = kPageSize - 12 - 4;

/// Schema bounds. For INT and VARCHAR alone they prove that a valid record
/// always fits in an empty page: 8 * (2 + 255) = 2056 bytes, well under
/// kMaxRecordSize.
inline constexpr std::size_t kMaxColumns = 8;
inline constexpr std::size_t kMaxVarcharLength = 255;
inline constexpr std::size_t kMaxIdentifierLength = 32;

/// Largest dimension a VECTOR column may declare. A vector costs
/// 2 + 4 * dimension bytes, so 1000 dimensions is 4002 bytes and still fits in
/// an empty page on its own.
///
/// Unlike VARCHAR, this bound is not enough by itself: two VECTOR(1000) columns
/// in the same table would not fit. That is why the Schema constructor also
/// checks the total against kMaxRecordSize, which restores the invariant that no
/// valid record can ever fail to fit in an empty page.
inline constexpr std::size_t kMaxVectorDimension = 1000;

/// Default number of frames held in RAM by the buffer pool. Overridable from
/// minidb.conf. The worst-case operation (an UPDATE that relocates a record)
/// pins 6 pages simultaneously, so 8 leaves two frames of headroom.
inline constexpr std::size_t kDefaultBufferPoolFrames = 8;

/// Hash index geometry. A bucket page holds (4096 - 8) / 10 = 408 entries.
inline constexpr std::uint16_t kDefaultBucketCount = 16;

}  // namespace minidb
