#pragma once

#include <cstdint>
#include <string>

#include "minidb/buffer/buffer_pool_manager.hpp"
#include "minidb/catalog/schema.hpp"
#include "minidb/common/types.hpp"

namespace minidb {

/// Persistent metadata for the single user table, stored on page 1.
///
/// The system supports one table on purpose: a table list, a heap and an index
/// per table, and name resolution in the engine would add real surface area
/// without changing what the storage, buffer and index layers demonstrate.
/// Because there is exactly one schema, records need no per-row type tags.
///
/// Page 1 layout:
///
///   offset  size    field
///   ------  ------  --------------------------------------------------
///        0       1  page_type            kCatalog
///        1       1  table_count          0 or 1
///        2       2  column_count
///        4       2  primary_key_index
///        6       2  reserved
///        8       4  first_table_page_id
///       12       4  last_table_page_id
///       16       4  index_header_page_id
///       20       8  record_count
///       28  2 + 32  table_name           length-prefixed, <= 32 bytes
///       62   <= 304 columns              8 x (2 + 32 name, 1 type,
///                                             2 max_length, 1 flags)
///
/// Worst case 366 bytes, comfortably inside one 4096-byte page.
class Catalog {
public:
    /// Reads page 1. When `create` is true the page is allocated and
    /// initialised empty; otherwise it must already exist.
    Catalog(BufferPoolManager& pool, bool create);

    [[nodiscard]] bool HasTable() const { return has_table_; }
    [[nodiscard]] const std::string& TableName() const { return table_name_; }
    [[nodiscard]] const Schema& GetSchema() const;

    /// Registers the table. Throws QueryError when one already exists.
    void CreateTable(const std::string& name, const Schema& schema, PageId first_page_id,
                     PageId index_header_page_id);

    /// Throws QueryError when `name` is not the table that exists.
    void RequireTable(const std::string& name) const;

    [[nodiscard]] PageId FirstTablePageId() const { return first_table_page_id_; }
    [[nodiscard]] PageId LastTablePageId() const { return last_table_page_id_; }
    [[nodiscard]] PageId IndexHeaderPageId() const { return index_header_page_id_; }
    [[nodiscard]] std::uint64_t RecordCount() const { return record_count_; }

    void SetLastTablePageId(PageId page_id);
    void IncrementRecordCount();
    void DecrementRecordCount();

    /// Writes the in-memory metadata back to page 1.
    void Flush();

private:
    void Load();

    BufferPoolManager& pool_;
    bool has_table_ = false;
    std::string table_name_;
    Schema schema_;
    PageId first_table_page_id_ = kInvalidPageId;
    PageId last_table_page_id_ = kInvalidPageId;
    PageId index_header_page_id_ = kInvalidPageId;
    std::uint64_t record_count_ = 0;
    bool dirty_ = false;
};

}  // namespace minidb
