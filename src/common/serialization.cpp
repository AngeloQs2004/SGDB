#include "minidb/common/serialization.hpp"

#include <bit>
#include <cstring>

#include "minidb/common/types.hpp"

namespace minidb::serialization {
namespace {

/// Single place where a range is validated, so no helper can write past the end
/// of a page. Throwing here turns an offset bug into a loud failure instead of
/// silent file corruption.
void CheckRange(std::size_t buffer_size, std::size_t offset, std::size_t length) {
    if (offset > buffer_size || length > buffer_size - offset) {
        throw StorageError("Acceso fuera de rango al serializar: offset " + std::to_string(offset) +
                           ", longitud " + std::to_string(length) + ", buffer " +
                           std::to_string(buffer_size));
    }
}

/// Writes `length` low-order bytes of `value`, least significant byte first.
void WriteLittleEndian(std::span<std::byte> buffer, std::size_t offset, std::uint64_t value,
                       std::size_t length) {
    CheckRange(buffer.size(), offset, length);
    for (std::size_t i = 0; i < length; ++i) {
        buffer[offset + i] = static_cast<std::byte>((value >> (8 * i)) & 0xFFu);
    }
}

[[nodiscard]] std::uint64_t ReadLittleEndian(std::span<const std::byte> buffer, std::size_t offset,
                                             std::size_t length) {
    CheckRange(buffer.size(), offset, length);
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < length; ++i) {
        value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(buffer[offset + i]))
                 << (8 * i);
    }
    return value;
}

}  // namespace

void WriteU8(std::span<std::byte> buffer, std::size_t offset, std::uint8_t value) {
    WriteLittleEndian(buffer, offset, value, sizeof(value));
}

void WriteU16(std::span<std::byte> buffer, std::size_t offset, std::uint16_t value) {
    WriteLittleEndian(buffer, offset, value, sizeof(value));
}

void WriteU32(std::span<std::byte> buffer, std::size_t offset, std::uint32_t value) {
    WriteLittleEndian(buffer, offset, value, sizeof(value));
}

void WriteU64(std::span<std::byte> buffer, std::size_t offset, std::uint64_t value) {
    WriteLittleEndian(buffer, offset, value, sizeof(value));
}

void WriteI32(std::span<std::byte> buffer, std::size_t offset, std::int32_t value) {
    // Two's complement is preserved by reinterpreting the bit pattern as
    // unsigned; ReadI32 undoes it symmetrically.
    WriteLittleEndian(buffer, offset, static_cast<std::uint32_t>(value), sizeof(value));
}

void WriteF32(std::span<std::byte> buffer, std::size_t offset, float value) {
    static_assert(sizeof(float) == sizeof(std::uint32_t),
                  "el formato del archivo asume IEEE 754 binary32");
    WriteLittleEndian(buffer, offset, std::bit_cast<std::uint32_t>(value), sizeof(value));
}

std::uint8_t ReadU8(std::span<const std::byte> buffer, std::size_t offset) {
    return static_cast<std::uint8_t>(ReadLittleEndian(buffer, offset, sizeof(std::uint8_t)));
}

std::uint16_t ReadU16(std::span<const std::byte> buffer, std::size_t offset) {
    return static_cast<std::uint16_t>(ReadLittleEndian(buffer, offset, sizeof(std::uint16_t)));
}

std::uint32_t ReadU32(std::span<const std::byte> buffer, std::size_t offset) {
    return static_cast<std::uint32_t>(ReadLittleEndian(buffer, offset, sizeof(std::uint32_t)));
}

std::uint64_t ReadU64(std::span<const std::byte> buffer, std::size_t offset) {
    return ReadLittleEndian(buffer, offset, sizeof(std::uint64_t));
}

float ReadF32(std::span<const std::byte> buffer, std::size_t offset) {
    return std::bit_cast<float>(
        static_cast<std::uint32_t>(ReadLittleEndian(buffer, offset, sizeof(float))));
}

std::int32_t ReadI32(std::span<const std::byte> buffer, std::size_t offset) {
    return static_cast<std::int32_t>(
        static_cast<std::uint32_t>(ReadLittleEndian(buffer, offset, sizeof(std::int32_t))));
}

std::size_t WriteString(std::span<std::byte> buffer, std::size_t offset, const std::string& value) {
    if (value.size() > 0xFFFFu) {
        throw StorageError("Cadena demasiado larga para serializar: " +
                           std::to_string(value.size()) + " bytes");
    }
    const auto length = static_cast<std::uint16_t>(value.size());
    WriteU16(buffer, offset, length);
    CheckRange(buffer.size(), offset + sizeof(length), length);
    if (length > 0) {
        std::memcpy(buffer.data() + offset + sizeof(length), value.data(), length);
    }
    return sizeof(length) + length;
}

std::string ReadString(std::span<const std::byte> buffer, std::size_t offset,
                       std::size_t* bytes_read) {
    const std::uint16_t length = ReadU16(buffer, offset);
    CheckRange(buffer.size(), offset + sizeof(length), length);
    std::string value(length, '\0');
    if (length > 0) {
        std::memcpy(value.data(), buffer.data() + offset + sizeof(length), length);
    }
    if (bytes_read != nullptr) {
        *bytes_read = sizeof(length) + length;
    }
    return value;
}

}  // namespace minidb::serialization
