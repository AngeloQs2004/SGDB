#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace minidb {

/// Explicit little-endian (de)serialisation helpers.
///
/// Everything written to disk goes through these functions. Objects are never
/// dumped with reinterpret_cast, because struct padding, endianness and
/// non-trivial members (std::string) would all leak into the file format.
///
/// Byte order is little-endian by choice: it is fixed across platforms, and on
/// x86 it also makes `xxd` dumps read the same way the debugger shows values.
///
/// Every function bounds-checks against the destination span and throws
/// StorageError rather than writing out of range.
namespace serialization {

void WriteU8(std::span<std::byte> buffer, std::size_t offset, std::uint8_t value);
void WriteU16(std::span<std::byte> buffer, std::size_t offset, std::uint16_t value);
void WriteU32(std::span<std::byte> buffer, std::size_t offset, std::uint32_t value);
void WriteU64(std::span<std::byte> buffer, std::size_t offset, std::uint64_t value);
void WriteI32(std::span<std::byte> buffer, std::size_t offset, std::int32_t value);

/// Writes the IEEE 754 binary32 bit pattern of `value`, little-endian.
///
/// The bits are obtained with std::bit_cast, not by reinterpreting a float*: the
/// cast is well defined, whereas type-punning through a pointer is not, and it
/// keeps the format explicit rather than dependent on how the compiler happens to
/// lay a float out.
void WriteF32(std::span<std::byte> buffer, std::size_t offset, float value);

[[nodiscard]] std::uint8_t ReadU8(std::span<const std::byte> buffer, std::size_t offset);
[[nodiscard]] std::uint16_t ReadU16(std::span<const std::byte> buffer, std::size_t offset);
[[nodiscard]] std::uint32_t ReadU32(std::span<const std::byte> buffer, std::size_t offset);
[[nodiscard]] std::uint64_t ReadU64(std::span<const std::byte> buffer, std::size_t offset);
[[nodiscard]] std::int32_t ReadI32(std::span<const std::byte> buffer, std::size_t offset);
[[nodiscard]] float ReadF32(std::span<const std::byte> buffer, std::size_t offset);

/// A string is stored as a uint16 byte count followed by that many raw bytes.
/// The length counts UTF-8 *bytes*, not characters, so 'Ciencia de la
/// Computacion' with an accent occupies 26 bytes even though it is 25
/// characters. VARCHAR(n) limits are checked in bytes for the same reason.
///
/// Returns the number of bytes consumed/produced, so callers can walk a record
/// field by field.
std::size_t WriteString(std::span<std::byte> buffer, std::size_t offset, const std::string& value);

[[nodiscard]] std::string ReadString(std::span<const std::byte> buffer, std::size_t offset,
                                     std::size_t* bytes_read = nullptr);

/// Bytes occupied by WriteString for a given string.
[[nodiscard]] inline std::size_t StringSize(const std::string& value) {
    return sizeof(std::uint16_t) + value.size();
}

/// Bytes occupied by a vector of `dimension` components: a uint16 dimension
/// followed by that many 32-bit floats.
[[nodiscard]] inline std::size_t VectorSize(std::size_t dimension) {
    return sizeof(std::uint16_t) + dimension * sizeof(float);
}

}  // namespace serialization
}  // namespace minidb
