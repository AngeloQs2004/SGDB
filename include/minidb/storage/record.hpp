#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "minidb/catalog/schema.hpp"
#include "minidb/common/types.hpp"
#include "minidb/common/value.hpp"

namespace minidb {

/// A row: one Value per column, in schema order.
///
/// The on-disk form carries no type tags and no column count. Layout is driven
/// entirely by the Schema, which lives in the catalog:
///
///   INT         4 bytes, little-endian two's complement
///   VARCHAR(n)  2-byte length in UTF-8 bytes, then that many raw bytes
class Record {
public:
    Record() = default;
    explicit Record(std::vector<Value> values) : values_(std::move(values)) {}

    [[nodiscard]] const std::vector<Value>& Values() const { return values_; }
    [[nodiscard]] const Value& GetValue(std::size_t index) const;
    void SetValue(std::size_t index, Value value);
    [[nodiscard]] std::size_t Size() const { return values_.size(); }

    /// Throws QueryError when the row does not match the schema: wrong number of
    /// values, wrong type, or a VARCHAR longer than its declared byte limit.
    void Validate(const Schema& schema) const;

    /// Bytes this record occupies on disk under `schema`.
    [[nodiscard]] std::size_t SerializedSize(const Schema& schema) const;

    /// Writes the record into `destination`, which must be exactly
    /// SerializedSize(schema) bytes long.
    void SerializeTo(const Schema& schema, std::span<std::byte> destination) const;

    [[nodiscard]] static Record DeserializeFrom(const Schema& schema,
                                                std::span<const std::byte> source);

    bool operator==(const Record&) const = default;

private:
    std::vector<Value> values_;
};

}  // namespace minidb
