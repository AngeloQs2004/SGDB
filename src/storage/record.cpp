#include "minidb/storage/record.hpp"

#include <utility>

#include "minidb/common/serialization.hpp"

namespace minidb {

const Value& Record::GetValue(std::size_t index) const {
    if (index >= values_.size()) {
        throw StorageError("Índice de valor fuera de rango: " + std::to_string(index));
    }
    return values_[index];
}

void Record::SetValue(std::size_t index, Value value) {
    if (index >= values_.size()) {
        throw StorageError("Índice de valor fuera de rango: " + std::to_string(index));
    }
    values_[index] = std::move(value);
}

void Record::Validate(const Schema& schema) const {
    if (values_.size() != schema.ColumnCount()) {
        throw QueryError("El registro tiene " + std::to_string(values_.size()) +
                         " valores pero la tabla tiene " + std::to_string(schema.ColumnCount()) +
                         " columnas");
    }

    for (std::size_t i = 0; i < values_.size(); ++i) {
        const Column& column = schema.GetColumn(i);

        // No column type maps to a float: it only ever carries a computed
        // distance. Rejecting it here is what keeps it out of the file.
        if (std::holds_alternative<float>(values_[i])) {
            throw QueryError("La columna '" + column.name +
                             "' recibió un valor de coma flotante, que no es un tipo almacenable");
        }

        if (column.type == ColumnType::kInteger) {
            if (!std::holds_alternative<std::int32_t>(values_[i])) {
                throw QueryError("La columna '" + column.name + "' es INT y recibió otro tipo");
            }
        } else if (column.type == ColumnType::kVector) {
            if (!std::holds_alternative<Vector>(values_[i])) {
                throw QueryError("La columna '" + column.name + "' es VECTOR y recibió otro tipo");
            }
            // The dimension is fixed by the schema: a table of vectors of
            // different lengths could not be searched, because a distance is only
            // defined between vectors of equal dimension.
            const Vector& vector = std::get<Vector>(values_[i]);
            if (vector.size() != column.max_length) {
                throw QueryError("La columna '" + column.name + "' es VECTOR(" +
                                 std::to_string(column.max_length) + ") y recibió " +
                                 std::to_string(vector.size()) + " componentes");
            }
        } else {
            if (!std::holds_alternative<std::string>(values_[i])) {
                throw QueryError("La columna '" + column.name +
                                 "' es VARCHAR y recibió otro tipo");
            }
            // The limit is in UTF-8 bytes, not characters: an accented Spanish
            // string occupies more bytes than it has characters.
            const std::string& text = std::get<std::string>(values_[i]);
            if (text.size() > column.max_length) {
                throw QueryError("El valor de la columna '" + column.name + "' ocupa " +
                                 std::to_string(text.size()) + " bytes y el máximo es " +
                                 std::to_string(column.max_length));
            }
        }
    }
}

std::size_t Record::SerializedSize(const Schema& schema) const {
    std::size_t total = 0;
    for (std::size_t i = 0; i < values_.size(); ++i) {
        switch (schema.GetColumn(i).type) {
            case ColumnType::kInteger:
                total += sizeof(std::int32_t);
                break;
            case ColumnType::kVector:
                total += serialization::VectorSize(std::get<Vector>(values_[i]).size());
                break;
            case ColumnType::kVarchar:
                total += serialization::StringSize(std::get<std::string>(values_[i]));
                break;
        }
    }
    return total;
}

void Record::SerializeTo(const Schema& schema, std::span<std::byte> destination) const {
    std::size_t offset = 0;
    for (std::size_t i = 0; i < values_.size(); ++i) {
        switch (schema.GetColumn(i).type) {
            case ColumnType::kInteger:
                serialization::WriteI32(destination, offset, std::get<std::int32_t>(values_[i]));
                offset += sizeof(std::int32_t);
                break;
            case ColumnType::kVector: {
                // A uint16 dimension followed by that many little-endian floats.
                // The dimension is written even though the schema fixes it, so a
                // record can be read back without consulting the catalog and a
                // corrupted length is detectable.
                const Vector& vector = std::get<Vector>(values_[i]);
                serialization::WriteU16(destination, offset,
                                        static_cast<std::uint16_t>(vector.size()));
                offset += sizeof(std::uint16_t);
                for (float component : vector) {
                    serialization::WriteF32(destination, offset, component);
                    offset += sizeof(float);
                }
                break;
            }
            case ColumnType::kVarchar:
                offset += serialization::WriteString(destination, offset,
                                                    std::get<std::string>(values_[i]));
                break;
        }
    }
}

Record Record::DeserializeFrom(const Schema& schema, std::span<const std::byte> source) {
    std::vector<Value> values;
    values.reserve(schema.ColumnCount());

    std::size_t offset = 0;
    for (const Column& column : schema.Columns()) {
        switch (column.type) {
            case ColumnType::kInteger:
                values.emplace_back(serialization::ReadI32(source, offset));
                offset += sizeof(std::int32_t);
                break;
            case ColumnType::kVector: {
                const std::uint16_t dimension = serialization::ReadU16(source, offset);
                offset += sizeof(std::uint16_t);
                if (dimension != column.max_length) {
                    throw StorageError("La columna '" + column.name + "' es VECTOR(" +
                                       std::to_string(column.max_length) +
                                       ") pero en disco hay un vector de " +
                                       std::to_string(dimension) + " componentes");
                }
                Vector vector;
                vector.reserve(dimension);
                for (std::uint16_t i = 0; i < dimension; ++i) {
                    vector.push_back(serialization::ReadF32(source, offset));
                    offset += sizeof(float);
                }
                values.emplace_back(std::move(vector));
                break;
            }
            case ColumnType::kVarchar: {
                std::size_t consumed = 0;
                values.emplace_back(serialization::ReadString(source, offset, &consumed));
                offset += consumed;
                break;
            }
        }
    }
    return Record(std::move(values));
}

}  // namespace minidb
