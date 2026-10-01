#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "minidb/common/constants.hpp"
#include "minidb/common/types.hpp"

namespace minidb {

/// One column of the single user table.
struct Column {
    std::string name;
    ColumnType type = ColumnType::kInteger;
    /// For VARCHAR, the maximum length in UTF-8 *bytes*. Unused for INT.
    std::uint16_t max_length = 0;
    bool is_primary_key = false;
};

/// Describes the table's columns and which one is the primary key.
///
/// The schema is the single source of truth for a record's layout: records
/// carry no per-row type tags, because duplicating the schema in every row
/// would waste space and create a second thing that can disagree with the
/// catalog.
class Schema {
public:
    Schema() = default;
    explicit Schema(std::vector<Column> columns);

    [[nodiscard]] const std::vector<Column>& Columns() const { return columns_; }
    [[nodiscard]] std::size_t ColumnCount() const { return columns_.size(); }
    [[nodiscard]] const Column& GetColumn(std::size_t index) const;

    /// Index of `name`, or std::nullopt when the column does not exist.
    /// Comparison is case-insensitive, matching SQL identifier behaviour.
    [[nodiscard]] std::optional<std::size_t> FindColumn(const std::string& name) const;

    [[nodiscard]] std::size_t PrimaryKeyIndex() const { return primary_key_index_; }
    [[nodiscard]] const Column& PrimaryKeyColumn() const { return columns_[primary_key_index_]; }

    /// Largest number of bytes a record of this schema can occupy. Used to prove
    /// a record always fits in an empty page.
    [[nodiscard]] std::size_t MaxSerializedSize() const;

private:
    std::vector<Column> columns_;
    std::size_t primary_key_index_ = 0;
};

}  // namespace minidb
