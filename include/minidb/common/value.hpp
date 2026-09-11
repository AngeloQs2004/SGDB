#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "minidb/common/types.hpp"

namespace minidb {

/// A fixed-dimension array of 32-bit floats, the payload of a VECTOR column.
///
/// float rather than double: it halves the bytes per vector, and the precision of
/// a similarity ranking is limited by the embedding itself long before it is
/// limited by 24 bits of mantissa. Every well-known vector store defaults to
/// 32-bit floats for the same reason.
using Vector = std::vector<float>;

/// A single column value.
///
/// The `float` alternative is never persisted: no column type maps to it. It
/// exists so that a computed column — the distance produced by a nearest
/// neighbour scan — can travel through the plan like any other value.
/// Record::Validate rejects it, which is what keeps it out of the file.
///
/// This lives in common/ rather than next to Record because the parser needs it
/// to represent literals, and the parsing layer must not depend on storage.
using Value = std::variant<std::int32_t, std::string, Vector, float>;

/// Human-readable form. A vector is abbreviated to its first components followed
/// by its dimension, because a 128-dimension embedding printed in full would make
/// the result table unreadable.
[[nodiscard]] std::string ValueToString(const Value& value);

/// Compares two values of the same type. Strings compare byte by byte, so the
/// ordering is UTF-8 code-unit order rather than a linguistic collation.
/// Throws QueryError when the types differ.
[[nodiscard]] bool CompareValues(const Value& left, CompareOperator op, const Value& right);

/// Strict weak ordering over values of the same type, for std::sort and std::map.
///
/// It exists so the sort and the aggregate operators share one definition of
/// "less than" instead of each writing its own comparator.
struct ValueLess {
    [[nodiscard]] bool operator()(const Value& left, const Value& right) const {
        return CompareValues(left, CompareOperator::kLess, right);
    }
};

}  // namespace minidb
