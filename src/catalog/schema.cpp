#include "minidb/catalog/schema.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

#include "minidb/common/serialization.hpp"

namespace minidb {
namespace {

/// SQL identifiers are compared without regard to case.
bool EqualsIgnoreCase(const std::string& left, const std::string& right) {
    return std::ranges::equal(left, right, [](unsigned char a, unsigned char b) {
        return std::tolower(a) == std::tolower(b);
    });
}

}  // namespace

Schema::Schema(std::vector<Column> columns) : columns_(std::move(columns)) {
    if (columns_.empty()) {
        throw QueryError("La tabla debe tener al menos una columna");
    }
    if (columns_.size() > kMaxColumns) {
        throw QueryError("La tabla no puede tener más de " + std::to_string(kMaxColumns) +
                         " columnas");
    }

    std::size_t primary_keys = 0;
    for (std::size_t i = 0; i < columns_.size(); ++i) {
        const Column& column = columns_[i];

        if (column.name.empty() || column.name.size() > kMaxIdentifierLength) {
            throw QueryError("Nombre de columna inválido: '" + column.name + "'");
        }
        if (column.type == ColumnType::kVarchar &&
            (column.max_length == 0 || column.max_length > kMaxVarcharLength)) {
            throw QueryError("VARCHAR(" + std::to_string(column.max_length) + ") en la columna '" +
                             column.name + "': la longitud debe estar entre 1 y " +
                             std::to_string(kMaxVarcharLength));
        }
        if (column.type == ColumnType::kVector &&
            (column.max_length == 0 || column.max_length > kMaxVectorDimension)) {
            throw QueryError("VECTOR(" + std::to_string(column.max_length) + ") en la columna '" +
                             column.name + "': la dimensión debe estar entre 1 y " +
                             std::to_string(kMaxVectorDimension));
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (EqualsIgnoreCase(columns_[j].name, column.name)) {
                throw QueryError("Columna duplicada: '" + column.name + "'");
            }
        }
        if (column.is_primary_key) {
            ++primary_keys;
            primary_key_index_ = i;
        }
    }

    if (primary_keys != 1) {
        throw QueryError("La tabla debe declarar exactamente una PRIMARY KEY");
    }
    // The hash index maps int32 keys to RecordIds, so the primary key must be
    // an INT. Supporting string keys would mean a second index implementation.
    if (columns_[primary_key_index_].type != ColumnType::kInteger) {
        throw QueryError("La PRIMARY KEY debe ser de tipo INT");
    }

    // With INT and VARCHAR alone the bounds in constants.hpp already guaranteed
    // that no valid record could exceed a page. A VECTOR column is large enough to
    // break that by itself, so the guarantee is restored here by checking the
    // widest possible record against the page's capacity. Without this an INSERT
    // could be accepted by the schema and then fail to fit in any page, which is a
    // much worse place to find out.
    const std::size_t widest_record = MaxSerializedSize();
    if (widest_record > kMaxRecordSize) {
        throw QueryError("El registro más ancho posible de esta tabla ocuparía " +
                         std::to_string(widest_record) + " bytes y una página admite como máximo " +
                         std::to_string(kMaxRecordSize) + "; reduzca la dimensión del VECTOR o el "
                         "número de columnas");
    }
}

const Column& Schema::GetColumn(std::size_t index) const {
    if (index >= columns_.size()) {
        throw StorageError("Índice de columna fuera de rango: " + std::to_string(index));
    }
    return columns_[index];
}

std::optional<std::size_t> Schema::FindColumn(const std::string& name) const {
    for (std::size_t i = 0; i < columns_.size(); ++i) {
        if (EqualsIgnoreCase(columns_[i].name, name)) {
            return i;
        }
    }
    return std::nullopt;
}

std::size_t Schema::MaxSerializedSize() const {
    std::size_t total = 0;
    for (const Column& column : columns_) {
        switch (column.type) {
            case ColumnType::kInteger:
                total += sizeof(std::int32_t);
                break;
            case ColumnType::kVector:
                total += serialization::VectorSize(column.max_length);
                break;
            case ColumnType::kVarchar:
                total += sizeof(std::uint16_t) + column.max_length;
                break;
        }
    }
    return total;
}

}  // namespace minidb
