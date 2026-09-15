#include "minidb/catalog/catalog.hpp"

#include <algorithm>
#include <cctype>
#include <vector>

#include "minidb/common/serialization.hpp"

namespace minidb {
namespace {

// Field offsets inside page 1. See the layout table in catalog.hpp.
constexpr std::size_t kOffsetPageType = 0;
constexpr std::size_t kOffsetTableCount = 1;
constexpr std::size_t kOffsetColumnCount = 2;
constexpr std::size_t kOffsetPrimaryKeyIndex = 4;
constexpr std::size_t kOffsetFirstTablePage = 8;
constexpr std::size_t kOffsetLastTablePage = 12;
constexpr std::size_t kOffsetIndexHeaderPage = 16;
constexpr std::size_t kOffsetRecordCount = 20;
constexpr std::size_t kOffsetTableName = 28;

/// Where the column descriptors start: right after the fixed-width table name
/// field, whose size is fixed regardless of the actual name length so the
/// column region always begins at the same offset.
constexpr std::size_t kTableNameFieldSize = sizeof(std::uint16_t) + kMaxIdentifierLength;
constexpr std::size_t kOffsetColumns = kOffsetTableName + kTableNameFieldSize;

/// Each column: 2-byte name length + up to 32 name bytes + type + max_length +
/// flags, laid out at a fixed stride so columns can be indexed directly.
constexpr std::size_t kColumnStride =
    sizeof(std::uint16_t) + kMaxIdentifierLength + 1 + sizeof(std::uint16_t) + 1;

constexpr std::uint8_t kFlagPrimaryKey = 0x01;

bool EqualsIgnoreCase(const std::string& left, const std::string& right) {
    return std::ranges::equal(left, right, [](unsigned char a, unsigned char b) {
        return std::tolower(a) == std::tolower(b);
    });
}

}  // namespace

Catalog::Catalog(BufferPoolManager& pool, bool create) : pool_(pool) {
    if (create) {
        PageId page_id = kInvalidPageId;
        PageGuard guard = NewGuarded(pool_, page_id);
        if (page_id != kCatalogPageId) {
            throw StorageError("El catálogo debe ocupar la página " +
                               std::to_string(kCatalogPageId) + " y se asignó la " +
                               std::to_string(page_id));
        }
        serialization::WriteU8(guard.Data(), kOffsetPageType,
                               static_cast<std::uint8_t>(PageType::kCatalog));
        serialization::WriteU8(guard.Data(), kOffsetTableCount, 0);
        guard.MarkDirty();
        return;
    }
    Load();
}

void Catalog::Load() {
    PageGuard guard = FetchGuarded(pool_, kCatalogPageId);
    const auto page = std::span<const std::byte>(guard.Data());

    if (static_cast<PageType>(serialization::ReadU8(page, kOffsetPageType)) != PageType::kCatalog) {
        throw StorageError("La página 1 no contiene un catálogo válido");
    }

    has_table_ = serialization::ReadU8(page, kOffsetTableCount) != 0;
    if (!has_table_) {
        return;
    }

    const std::uint16_t column_count = serialization::ReadU16(page, kOffsetColumnCount);
    if (column_count == 0 || column_count > kMaxColumns) {
        throw StorageError("Catálogo corrupto: " + std::to_string(column_count) + " columnas");
    }

    first_table_page_id_ = serialization::ReadU32(page, kOffsetFirstTablePage);
    last_table_page_id_ = serialization::ReadU32(page, kOffsetLastTablePage);
    index_header_page_id_ = serialization::ReadU32(page, kOffsetIndexHeaderPage);
    record_count_ = serialization::ReadU64(page, kOffsetRecordCount);
    table_name_ = serialization::ReadString(page, kOffsetTableName);

    std::vector<Column> columns;
    columns.reserve(column_count);
    for (std::uint16_t i = 0; i < column_count; ++i) {
        const std::size_t base = kOffsetColumns + kColumnStride * i;
        Column column;
        column.name = serialization::ReadString(page, base);

        // The type byte is validated here rather than trusted: casting an
        // unknown value into the enum would produce a Column that every switch
        // in the system would silently mishandle.
        const std::uint8_t type_byte = serialization::ReadU8(page, base + kTableNameFieldSize);
        if (type_byte != static_cast<std::uint8_t>(ColumnType::kInteger) &&
            type_byte != static_cast<std::uint8_t>(ColumnType::kVarchar) &&
            type_byte != static_cast<std::uint8_t>(ColumnType::kVector)) {
            throw StorageError("El catálogo declara un tipo de columna desconocido (" +
                               std::to_string(type_byte) + ") en la columna '" + column.name + "'");
        }
        column.type = static_cast<ColumnType>(type_byte);
        column.max_length = serialization::ReadU16(page, base + kTableNameFieldSize + 1);
        column.is_primary_key =
            (serialization::ReadU8(page, base + kTableNameFieldSize + 3) & kFlagPrimaryKey) != 0;
        columns.push_back(std::move(column));
    }

    // Reconstructing through the Schema constructor re-runs every validation,
    // so a corrupt catalog is rejected here rather than much later.
    schema_ = Schema(std::move(columns));
}

const Schema& Catalog::GetSchema() const {
    if (!has_table_) {
        throw QueryError("No existe ninguna tabla; use CREATE TABLE primero");
    }
    return schema_;
}

void Catalog::CreateTable(const std::string& name, const Schema& schema, PageId first_page_id,
                          PageId index_header_page_id) {
    if (has_table_) {
        throw QueryError("Ya existe la tabla '" + table_name_ +
                         "'; este sistema admite una sola tabla");
    }
    if (name.empty() || name.size() > kMaxIdentifierLength) {
        throw QueryError("Nombre de tabla inválido: '" + name + "'");
    }

    has_table_ = true;
    table_name_ = name;
    schema_ = schema;
    first_table_page_id_ = first_page_id;
    last_table_page_id_ = first_page_id;
    index_header_page_id_ = index_header_page_id;
    record_count_ = 0;
    dirty_ = true;
    Flush();
}

void Catalog::RequireTable(const std::string& name) const {
    if (!has_table_) {
        throw QueryError("No existe la tabla '" + name + "'");
    }
    if (!EqualsIgnoreCase(name, table_name_)) {
        throw QueryError("No existe la tabla '" + name + "'; la tabla definida es '" +
                         table_name_ + "'");
    }
}

void Catalog::SetLastTablePageId(PageId page_id) {
    last_table_page_id_ = page_id;
    dirty_ = true;
}

void Catalog::IncrementRecordCount() {
    ++record_count_;
    dirty_ = true;
}

void Catalog::DecrementRecordCount() {
    if (record_count_ == 0) {
        throw StorageError("El contador de registros del catálogo bajaría de cero");
    }
    --record_count_;
    dirty_ = true;
}

void Catalog::Flush() {
    if (!dirty_) {
        return;
    }

    PageGuard guard = FetchGuarded(pool_, kCatalogPageId);
    auto page = guard.Data();

    serialization::WriteU8(page, kOffsetPageType, static_cast<std::uint8_t>(PageType::kCatalog));
    serialization::WriteU8(page, kOffsetTableCount, has_table_ ? 1 : 0);

    if (has_table_) {
        serialization::WriteU16(page, kOffsetColumnCount,
                                static_cast<std::uint16_t>(schema_.ColumnCount()));
        serialization::WriteU16(page, kOffsetPrimaryKeyIndex,
                                static_cast<std::uint16_t>(schema_.PrimaryKeyIndex()));
        serialization::WriteU32(page, kOffsetFirstTablePage, first_table_page_id_);
        serialization::WriteU32(page, kOffsetLastTablePage, last_table_page_id_);
        serialization::WriteU32(page, kOffsetIndexHeaderPage, index_header_page_id_);
        serialization::WriteU64(page, kOffsetRecordCount, record_count_);
        serialization::WriteString(page, kOffsetTableName, table_name_);

        for (std::size_t i = 0; i < schema_.ColumnCount(); ++i) {
            const Column& column = schema_.GetColumn(i);
            const std::size_t base = kOffsetColumns + kColumnStride * i;
            serialization::WriteString(page, base, column.name);
            serialization::WriteU8(page, base + kTableNameFieldSize,
                                   static_cast<std::uint8_t>(column.type));
            serialization::WriteU16(page, base + kTableNameFieldSize + 1, column.max_length);
            serialization::WriteU8(page, base + kTableNameFieldSize + 3,
                                   column.is_primary_key ? kFlagPrimaryKey : 0);
        }
    }

    guard.MarkDirty();
    dirty_ = false;
}

}  // namespace minidb
