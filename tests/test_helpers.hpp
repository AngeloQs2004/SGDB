#pragma once

#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

#include "minidb/common/constants.hpp"

namespace minidb::testing {

/// Gives each test its own database file under the system temp directory and
/// deletes it afterwards, so tests never share state and leave nothing behind.
class TempDatabase {
public:
    explicit TempDatabase(const std::string& label) {
        static int counter = 0;
        directory_ = std::filesystem::temp_directory_path() /
                     ("minidb_test_" + label + "_" + std::to_string(++counter) + "_" +
                      std::to_string(::getpid()));
        std::filesystem::create_directories(directory_);
        path_ = directory_ / "test.db";
    }

    ~TempDatabase() {
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }

    TempDatabase(const TempDatabase&) = delete;
    TempDatabase& operator=(const TempDatabase&) = delete;

    [[nodiscard]] const std::filesystem::path& Path() const { return path_; }

    [[nodiscard]] std::uint64_t SizeOnDisk() const {
        std::error_code ignored;
        const auto size = std::filesystem::file_size(path_, ignored);
        return ignored ? 0 : size;
    }

private:
    std::filesystem::path directory_;
    std::filesystem::path path_;
};

/// Fills a page with a byte pattern derived from `seed`, so a page read back
/// from disk can be checked byte for byte.
inline std::array<std::byte, kPageSize> MakePattern(std::uint8_t seed) {
    std::array<std::byte, kPageSize> page{};
    for (std::size_t i = 0; i < kPageSize; ++i) {
        page[i] = static_cast<std::byte>((seed + i) % 251);
    }
    return page;
}

}  // namespace minidb::testing
