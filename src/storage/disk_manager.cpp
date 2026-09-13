#include "minidb/storage/disk_manager.hpp"

#include <array>
#include <format>
#include <ios>
#include <string>
#include <utility>

#include "minidb/common/serialization.hpp"

namespace minidb {
namespace {

// Field offsets inside page 0. See the table in disk_manager.hpp.
constexpr std::size_t kOffsetMagic = 0;
constexpr std::size_t kOffsetVersion = 4;
constexpr std::size_t kOffsetPageSize = 6;
constexpr std::size_t kOffsetPageCount = 8;
constexpr std::size_t kOffsetFreePageHead = 12;
constexpr std::size_t kOffsetCatalogPageId = 16;

// Field offsets inside a page that sits on the free list.
constexpr std::size_t kOffsetFreePageType = 0;
constexpr std::size_t kOffsetNextFreePage = 4;

using Page = std::array<std::byte, kPageSize>;

}  // namespace

DiskManager::DiskManager(std::filesystem::path database_path) : path_(std::move(database_path)) {
    const bool exists = std::filesystem::exists(path_);

    if (!exists) {
        if (path_.has_parent_path()) {
            std::filesystem::create_directories(path_.parent_path());
        }
        // Create the file first: fstream will not create a file opened for both
        // reading and writing.
        std::ofstream create(path_, std::ios::binary);
        if (!create) {
            throw StorageError("No se pudo crear la base de datos: " + path_.string());
        }
    }

    file_.open(path_, std::ios::in | std::ios::out | std::ios::binary);
    if (!file_.is_open()) {
        throw StorageError("No se pudo abrir la base de datos: " + path_.string());
    }

    if (exists && std::filesystem::file_size(path_) > 0) {
        LoadAndValidateHeader();
    } else {
        CreateDatabase();
        was_created_ = true;
    }
}

DiskManager::~DiskManager() {
    // Best effort: a destructor must not throw. A caller that cares about
    // durability calls Flush() explicitly, which Database does on shutdown.
    try {
        Flush();
    } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch)
    }
    if (file_.is_open()) {
        file_.close();
    }
}

void DiskManager::CreateDatabase() {
    page_count_ = 1;  // page 0 is the header itself
    free_page_head_ = kInvalidPageId;

    Page header{};
    serialization::WriteU32(header, kOffsetMagic, kMagicNumber);
    serialization::WriteU16(header, kOffsetVersion, kFormatVersion);
    serialization::WriteU16(header, kOffsetPageSize, static_cast<std::uint16_t>(kPageSize));
    serialization::WriteU32(header, kOffsetPageCount, page_count_);
    serialization::WriteU32(header, kOffsetFreePageHead, free_page_head_);
    serialization::WriteU32(header, kOffsetCatalogPageId, kCatalogPageId);

    WriteRaw(kFileHeaderPageId, header);
    file_.flush();
    header_dirty_ = false;
}

void DiskManager::LoadAndValidateHeader() {
    const std::uint64_t size = std::filesystem::file_size(path_);

    if (size % kPageSize != 0) {
        throw StorageError("Archivo de base de datos corrupto: el tamaño (" +
                           std::to_string(size) + " bytes) no es múltiplo de " +
                           std::to_string(kPageSize));
    }
    if (size < kPageSize) {
        throw StorageError("Archivo de base de datos incompleto: falta la cabecera");
    }

    Page header{};
    ReadRaw(kFileHeaderPageId, header);

    const std::uint32_t magic = serialization::ReadU32(header, kOffsetMagic);
    if (magic != kMagicNumber) {
        throw StorageError(std::format(
            "El archivo no es una base de datos MiniDB (número mágico 0x{:08X}, se esperaba 0x{:08X})",
            magic, kMagicNumber));
    }

    const std::uint16_t version = serialization::ReadU16(header, kOffsetVersion);
    if (version != kFormatVersion) {
        throw StorageError("Versión de formato incompatible: el archivo usa la " +
                           std::to_string(version) + " y este binario espera la " +
                           std::to_string(kFormatVersion));
    }

    const std::uint16_t page_size = serialization::ReadU16(header, kOffsetPageSize);
    if (page_size != kPageSize) {
        throw StorageError("Tamaño de página incompatible: el archivo usa " +
                           std::to_string(page_size) + " bytes y este binario espera " +
                           std::to_string(kPageSize));
    }

    const std::uint32_t catalog_page_id = serialization::ReadU32(header, kOffsetCatalogPageId);
    if (catalog_page_id != kCatalogPageId) {
        throw StorageError("Cabecera inconsistente: la página de catálogo declarada es " +
                           std::to_string(catalog_page_id));
    }

    page_count_ = serialization::ReadU32(header, kOffsetPageCount);
    free_page_head_ = serialization::ReadU32(header, kOffsetFreePageHead);

    const std::uint64_t pages_on_disk = size / kPageSize;
    if (page_count_ != pages_on_disk) {
        throw StorageError("Cabecera inconsistente: declara " + std::to_string(page_count_) +
                           " páginas pero el archivo contiene " + std::to_string(pages_on_disk));
    }
    if (free_page_head_ != kInvalidPageId && free_page_head_ >= page_count_) {
        throw StorageError("Cabecera inconsistente: la lista de páginas libres apunta a la " +
                           std::to_string(free_page_head_));
    }

    header_dirty_ = false;
}

void DiskManager::WriteHeader() {
    Page header{};
    ReadRaw(kFileHeaderPageId, header);
    serialization::WriteU32(header, kOffsetPageCount, page_count_);
    serialization::WriteU32(header, kOffsetFreePageHead, free_page_head_);
    WriteRaw(kFileHeaderPageId, header);
    header_dirty_ = false;
}

void DiskManager::ValidatePageId(PageId page_id, const char* operation) const {
    if (page_id == kInvalidPageId) {
        throw StorageError(std::string(operation) + ": identificador de página inválido");
    }
    if (page_id >= page_count_) {
        throw StorageError(std::string(operation) + ": la página " + std::to_string(page_id) +
                           " está fuera de rango (el archivo tiene " + std::to_string(page_count_) +
                           " páginas)");
    }
}

void DiskManager::ReadRaw(PageId page_id, std::span<std::byte, kPageSize> destination) {
    const auto offset = static_cast<std::streamoff>(page_id) * static_cast<std::streamoff>(kPageSize);

    // Seeking between a write and a read is required by the standard for a
    // stream opened in both modes; doing it unconditionally keeps that
    // guarantee no matter the previous operation.
    file_.seekg(offset, std::ios::beg);
    if (!file_) {
        file_.clear();
        throw StorageError("No se pudo posicionar la lectura en la página " +
                           std::to_string(page_id));
    }

    file_.read(reinterpret_cast<char*>(destination.data()),
               static_cast<std::streamsize>(kPageSize));
    if (file_.gcount() != static_cast<std::streamsize>(kPageSize)) {
        const auto read_bytes = file_.gcount();
        file_.clear();
        throw StorageError("Lectura incompleta de la página " + std::to_string(page_id) + ": " +
                           std::to_string(read_bytes) + " de " + std::to_string(kPageSize) +
                           " bytes");
    }
}

void DiskManager::WriteRaw(PageId page_id, std::span<const std::byte, kPageSize> source) {
    const auto offset = static_cast<std::streamoff>(page_id) * static_cast<std::streamoff>(kPageSize);

    file_.seekp(offset, std::ios::beg);
    if (!file_) {
        file_.clear();
        throw StorageError("No se pudo posicionar la escritura en la página " +
                           std::to_string(page_id));
    }

    file_.write(reinterpret_cast<const char*>(source.data()),
                static_cast<std::streamsize>(kPageSize));
    if (!file_) {
        file_.clear();
        throw StorageError("Escritura incompleta de la página " + std::to_string(page_id));
    }
}

void DiskManager::ReadPage(PageId page_id, std::span<std::byte, kPageSize> destination) {
    ValidatePageId(page_id, "Lectura de página");
    ReadRaw(page_id, destination);
}

void DiskManager::WritePage(PageId page_id, std::span<const std::byte, kPageSize> source) {
    ValidatePageId(page_id, "Escritura de página");
    WriteRaw(page_id, source);
}

PageId DiskManager::AllocatePage() {
    Page blank{};

    if (free_page_head_ != kInvalidPageId) {
        const PageId reused = free_page_head_;

        Page freed{};
        ReadRaw(reused, freed);
        if (static_cast<PageType>(serialization::ReadU8(freed, kOffsetFreePageType)) !=
            PageType::kFree) {
            throw StorageError("La lista de páginas libres está corrupta: la página " +
                               std::to_string(reused) + " no está marcada como libre");
        }
        free_page_head_ = serialization::ReadU32(freed, kOffsetNextFreePage);

        WriteRaw(reused, blank);
        header_dirty_ = true;
        return reused;
    }

    const PageId fresh = page_count_;
    ++page_count_;
    WriteRaw(fresh, blank);  // extends the file by exactly one page
    header_dirty_ = true;
    return fresh;
}

void DiskManager::DeallocatePage(PageId page_id) {
    if (page_id == kFileHeaderPageId) {
        throw StorageError("No se puede liberar la página 0: contiene la cabecera del archivo");
    }
    ValidatePageId(page_id, "Liberación de página");

    Page freed{};
    serialization::WriteU8(freed, kOffsetFreePageType, static_cast<std::uint8_t>(PageType::kFree));
    serialization::WriteU32(freed, kOffsetNextFreePage, free_page_head_);
    WriteRaw(page_id, freed);

    free_page_head_ = page_id;
    header_dirty_ = true;
}

void DiskManager::Flush() {
    if (header_dirty_) {
        WriteHeader();
    }
    file_.flush();
    if (!file_) {
        file_.clear();
        throw StorageError("No se pudo sincronizar la base de datos con el disco");
    }
}

std::uint64_t DiskManager::FileSize() {
    // The size must reflect pages still sitting in the stream buffer, so flush
    // before asking the filesystem.
    file_.flush();
    return std::filesystem::file_size(path_);
}

}  // namespace minidb
