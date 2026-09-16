#pragma once

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "minidb/buffer/buffer_pool_manager.hpp"
#include "minidb/common/types.hpp"

namespace minidb {

/// B+ Tree persistente: clave INT -> RecordId. Cada nodo es una página del BufferPoolManager;
/// la página de cabecera (`header_page_id`) guarda raíz, grado t, nº de entradas y de splits.
class BPlusTree {
public:
    struct Statistics {
        std::uint32_t height = 0;
        std::uint32_t node_count = 0;
        std::uint32_t leaf_count = 0;
        std::uint32_t internal_count = 0;
        std::uint64_t entry_count = 0;
        std::uint64_t split_count = 0;
    };

    BPlusTree(BufferPoolManager& pool, PageId header_page_id);

    /// Crea un árbol vacío y devuelve el PageId de su cabecera.
    [[nodiscard]] static PageId Create(BufferPoolManager& pool, std::uint16_t minimum_degree = 16);

    /// Búsqueda exacta. Si `nodes_visited` no es nulo, suma las páginas recorridas.
    [[nodiscard]] std::optional<RecordId> Search(std::int32_t key,
                                                 std::uint32_t* nodes_visited = nullptr) const;

    /// Lanza QueryError si la clave ya existe.
    void Insert(std::int32_t key, RecordId rid);

    /// Entrada sin ordenar. Árbol vacío: construcción bottom-up; con datos: Insert() por entrada.
    /// Ante duplicados o RecordId inválidos lanza QueryError sin modificar el árbol.
    void BulkLoad(std::span<const std::pair<std::int32_t, RecordId>> entries);

    [[nodiscard]] Statistics GetStatistics() const;

    /// Comprueba orden de claves, rangos de separadores, profundidad uniforme de hojas,
    /// ocupación mínima, punteros al padre, cadena de hojas y entry_count.
    [[nodiscard]] bool Validate(std::string* error = nullptr) const;
    [[nodiscard]] PageId HeaderPageId() const { return header_page_id_; }
    [[nodiscard]] PageId RootPageId() const;
    [[nodiscard]] std::uint16_t MinimumDegree() const;

    /// Log de splits y nuevas raíces (para la demo).
    void SetTrace(std::ostream* output) { trace_ = output; }

private:
    struct Node {
        PageId page_id = kInvalidPageId;
        PageId parent_page_id = kInvalidPageId;
        PageId next_leaf_page_id = kInvalidPageId;
        bool is_leaf = false;
        std::vector<std::int32_t> keys;
        std::vector<RecordId> record_ids;  // solo hojas
        std::vector<PageId> children;      // solo internos; keys.size() + 1
    };

    struct Header {
        PageId root_page_id = kInvalidPageId;
        std::uint16_t minimum_degree = 0;
        std::uint64_t entry_count = 0;
        std::uint64_t split_count = 0;
    };

    [[nodiscard]] Header ReadHeader() const;
    void WriteHeader(const Header& header);
    [[nodiscard]] Node ReadNode(PageId page_id) const;
    void WriteNode(const Node& node);
    [[nodiscard]] PageId NewNode(bool leaf, PageId parent_page_id);

    [[nodiscard]] std::size_t MaxKeys() const;
    [[nodiscard]] PageId FindLeaf(std::int32_t key, std::vector<PageId>* ancestors = nullptr,
                                  std::uint32_t* nodes_visited = nullptr) const;
    struct ValidationState;
    [[nodiscard]] bool ValidateNode(PageId page_id, PageId expected_parent,
                                    std::optional<std::int32_t> low,
                                    std::optional<std::int32_t> high, std::uint32_t depth,
                                    ValidationState& state) const;
    void InsertIntoParent(PageId left_page_id, std::int32_t separator_key, PageId right_page_id,
                          std::vector<PageId>& ancestors);
    void SetParent(PageId page_id, PageId parent_page_id);
    void IncrementSplitCount();

    BufferPoolManager& pool_;
    PageId header_page_id_;
    std::ostream* trace_ = nullptr;
};

}  // namespace minidb
