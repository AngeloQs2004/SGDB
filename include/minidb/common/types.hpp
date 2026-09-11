#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace minidb {

using PageId = std::uint32_t;
using FrameId = std::size_t;
using SlotId = std::uint16_t;

inline constexpr PageId kInvalidPageId = std::numeric_limits<PageId>::max();

/// Identifies a record by the page holding it and its position in that page's
/// slot directory. This is what the hash index maps primary keys to, which is
/// why TablePage::Compact must never renumber slots.
struct RecordId {
    PageId page_id = kInvalidPageId;
    SlotId slot_id = 0;

    [[nodiscard]] bool IsValid() const { return page_id != kInvalidPageId; }

    bool operator==(const RecordId&) const = default;
};

/// Tag stored in byte 0 of every page except page 0 (see kMagicNumber).
enum class PageType : std::uint8_t {
    kInvalid = 0,
    kFileHeader = 1,
    kCatalog = 2,
    kTableData = 3,
    kHashIndexHeader = 4,
    kHashBucket = 5,
    kHashOverflow = 6,
    kFree = 7,
};

/// The two column types the system supports.
enum class ColumnType : std::uint8_t {
    kInteger = 1,
    kVarchar = 2,
    /// Fixed-dimension array of 32-bit floats, for similarity search. The
    /// declared dimension is stored in Column::max_length, as VARCHAR stores its
    /// byte limit there.
    kVector = 3,
};

/// How the distance between two vectors is measured in a nearest neighbour query.
///
/// This lives next to CompareOperator because it belongs to the query, not to the
/// implementation: the parser needs it to represent `USING COSINE`, and the metric
/// functions themselves live in `minidb/vector/distance.hpp`.
enum class DistanceMetric : std::uint8_t {
    kEuclidean,
    kCosine,
    kDotProduct,
};

/// Comparison operators accepted in a WHERE clause.
enum class CompareOperator : std::uint8_t {
    kEqual,
    kNotEqual,
    kLess,
    kLessEqual,
    kGreater,
    kGreaterEqual,
};

/// Errors caused by a corrupt or incompatible database file, or by a violated
/// internal invariant. These are not the user's fault and are not recoverable.
class StorageError : public std::runtime_error {
public:
    explicit StorageError(const std::string& message) : std::runtime_error(message) {}
};

/// Errors the user can provoke and correct: bad SQL, duplicate key, value too
/// long, unknown table. The CLI reports these and keeps running.
class QueryError : public std::runtime_error {
public:
    explicit QueryError(const std::string& message) : std::runtime_error(message) {}
};

}  // namespace minidb
