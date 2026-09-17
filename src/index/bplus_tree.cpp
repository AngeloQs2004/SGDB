#include "minidb/index/bplus_tree.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <optional>
#include <queue>
#include <utility>
#include <vector>
#include <string>

#include "minidb/common/constants.hpp"
#include "minidb/common/serialization.hpp"

namespace minidb {
namespace {

constexpr std::size_t kHeaderRootOffset = 4;
constexpr std::size_t kHeaderDegreeOffset = 8;
constexpr std::size_t kHeaderEntryCountOffset = 12;
constexpr std::size_t kHeaderSplitCountOffset = 20;

constexpr std::size_t kNodeKeyCountOffset = 2;
constexpr std::size_t kNodeParentOffset = 4;
constexpr std::size_t kNodeNextLeafOffset = 8;
constexpr std::size_t kNodeDataOffset = 16;
constexpr std::size_t kLeafEntrySize = 10;  // int32 + uint32 + uint16
constexpr std::size_t kInternalPairSize = 8;  // int32 separator + uint32 right child

constexpr std::uint8_t kTypeBTreeHeader = 8;
constexpr std::uint8_t kTypeBTreeInternal = 9;
constexpr std::uint8_t kTypeBTreeLeaf = 10;

[[nodiscard]] std::span<const std::byte> ConstBytes(std::span<std::byte> bytes) {
    return {bytes.data(), bytes.size()};
}

void Zero(std::span<std::byte> bytes) {
    std::fill(bytes.begin(), bytes.end(), std::byte{0});
}

}  // namespace

PageId BPlusTree::Create(BufferPoolManager& pool, std::uint16_t minimum_degree) {
    if (minimum_degree < 2) {
        throw QueryError("B+ Tree: el grado minimo t debe ser >= 2");
    }

    // La hoja es el límite físico: 16 + 10*(2t-1) <= 4096.
    const std::size_t logical_max = 2U * static_cast<std::size_t>(minimum_degree) - 1U;
    const std::size_t physical_leaf_max = (kPageSize - kNodeDataOffset) / kLeafEntrySize;
    if (logical_max > physical_leaf_max) {
        throw QueryError("B+ Tree: el grado t no cabe en una pagina de 4096 bytes");
    }

    PageId header_id = kInvalidPageId;
    PageGuard header_guard = NewGuarded(pool, header_id);
    Zero(header_guard.Data());
    serialization::WriteU8(header_guard.Data(), 0, kTypeBTreeHeader);
    serialization::WriteU16(header_guard.Data(), kHeaderDegreeOffset, minimum_degree);
    header_guard.MarkDirty();

    PageId root_id = kInvalidPageId;
    PageGuard root_guard = NewGuarded(pool, root_id);
    Zero(root_guard.Data());
    serialization::WriteU8(root_guard.Data(), 0, kTypeBTreeLeaf);
    serialization::WriteU16(root_guard.Data(), kNodeKeyCountOffset, 0);
    serialization::WriteU32(root_guard.Data(), kNodeParentOffset, kInvalidPageId);
    serialization::WriteU32(root_guard.Data(), kNodeNextLeafOffset, kInvalidPageId);
    root_guard.MarkDirty();

    serialization::WriteU32(header_guard.Data(), kHeaderRootOffset, root_id);
    serialization::WriteU64(header_guard.Data(), kHeaderEntryCountOffset, 0);
    serialization::WriteU64(header_guard.Data(), kHeaderSplitCountOffset, 0);
    header_guard.MarkDirty();
    return header_id;
}

BPlusTree::BPlusTree(BufferPoolManager& pool, PageId header_page_id)
    : pool_(pool), header_page_id_(header_page_id) {
    const Header header = ReadHeader();
    if (header.root_page_id == kInvalidPageId || header.minimum_degree < 2) {
        throw StorageError("B+ Tree: cabecera invalida");
    }
}

BPlusTree::Header BPlusTree::ReadHeader() const {
    PageGuard guard = FetchGuarded(pool_, header_page_id_);
    const auto bytes = ConstBytes(guard.Data());
    if (serialization::ReadU8(bytes, 0) != kTypeBTreeHeader) {
        throw StorageError("B+ Tree: la pagina indicada no es una cabecera B+ Tree");
    }
    Header h;
    h.root_page_id = serialization::ReadU32(bytes, kHeaderRootOffset);
    h.minimum_degree = serialization::ReadU16(bytes, kHeaderDegreeOffset);
    h.entry_count = serialization::ReadU64(bytes, kHeaderEntryCountOffset);
    h.split_count = serialization::ReadU64(bytes, kHeaderSplitCountOffset);
    return h;
}

void BPlusTree::WriteHeader(const Header& header) {
    PageGuard guard = FetchGuarded(pool_, header_page_id_);
    serialization::WriteU8(guard.Data(), 0, kTypeBTreeHeader);
    serialization::WriteU32(guard.Data(), kHeaderRootOffset, header.root_page_id);
    serialization::WriteU16(guard.Data(), kHeaderDegreeOffset, header.minimum_degree);
    serialization::WriteU64(guard.Data(), kHeaderEntryCountOffset, header.entry_count);
    serialization::WriteU64(guard.Data(), kHeaderSplitCountOffset, header.split_count);
    guard.MarkDirty();
}

PageId BPlusTree::RootPageId() const { return ReadHeader().root_page_id; }
std::uint16_t BPlusTree::MinimumDegree() const { return ReadHeader().minimum_degree; }

std::size_t BPlusTree::MaxKeys() const {
    return 2U * static_cast<std::size_t>(MinimumDegree()) - 1U;
}

BPlusTree::Node BPlusTree::ReadNode(PageId page_id) const {
    PageGuard guard = FetchGuarded(pool_, page_id);
    const auto bytes = ConstBytes(guard.Data());
    const std::uint8_t type = serialization::ReadU8(bytes, 0);
    if (type != kTypeBTreeLeaf && type != kTypeBTreeInternal) {
        throw StorageError("B+ Tree: pagina de nodo con tipo invalido");
    }

    Node node;
    node.page_id = page_id;
    node.is_leaf = type == kTypeBTreeLeaf;
    const std::uint16_t count = serialization::ReadU16(bytes, kNodeKeyCountOffset);
    node.parent_page_id = serialization::ReadU32(bytes, kNodeParentOffset);
    node.next_leaf_page_id = serialization::ReadU32(bytes, kNodeNextLeafOffset);
    node.keys.reserve(count);

    if (node.is_leaf) {
        node.record_ids.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t off = kNodeDataOffset + i * kLeafEntrySize;
            node.keys.push_back(serialization::ReadI32(bytes, off));
            node.record_ids.push_back(RecordId{
                serialization::ReadU32(bytes, off + 4),
                serialization::ReadU16(bytes, off + 8)});
        }
    } else {
        node.children.reserve(static_cast<std::size_t>(count) + 1U);
        if (count == 0) {
            throw StorageError("B+ Tree: nodo interno sin separadores");
        }
        node.children.push_back(serialization::ReadU32(bytes, kNodeDataOffset));
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t off = kNodeDataOffset + 4U + i * kInternalPairSize;
            node.keys.push_back(serialization::ReadI32(bytes, off));
            node.children.push_back(serialization::ReadU32(bytes, off + 4));
        }
    }
    return node;
}

void BPlusTree::WriteNode(const Node& node) {
    if (node.keys.size() > MaxKeys()) {
        throw StorageError("B+ Tree: intento de persistir un nodo sobrecargado");
    }
    if (node.is_leaf && node.record_ids.size() != node.keys.size()) {
        throw StorageError("B+ Tree: hoja inconsistente");
    }
    if (!node.is_leaf && node.children.size() != node.keys.size() + 1U) {
        throw StorageError("B+ Tree: nodo interno inconsistente");
    }

    PageGuard guard = FetchGuarded(pool_, node.page_id);
    Zero(guard.Data());
    serialization::WriteU8(guard.Data(), 0, node.is_leaf ? kTypeBTreeLeaf : kTypeBTreeInternal);
    serialization::WriteU16(guard.Data(), kNodeKeyCountOffset,
                            static_cast<std::uint16_t>(node.keys.size()));
    serialization::WriteU32(guard.Data(), kNodeParentOffset, node.parent_page_id);
    serialization::WriteU32(guard.Data(), kNodeNextLeafOffset,
                            node.is_leaf ? node.next_leaf_page_id : kInvalidPageId);

    if (node.is_leaf) {
        for (std::size_t i = 0; i < node.keys.size(); ++i) {
            const std::size_t off = kNodeDataOffset + i * kLeafEntrySize;
            serialization::WriteI32(guard.Data(), off, node.keys[i]);
            serialization::WriteU32(guard.Data(), off + 4, node.record_ids[i].page_id);
            serialization::WriteU16(guard.Data(), off + 8, node.record_ids[i].slot_id);
        }
    } else {
        serialization::WriteU32(guard.Data(), kNodeDataOffset, node.children.front());
        for (std::size_t i = 0; i < node.keys.size(); ++i) {
            const std::size_t off = kNodeDataOffset + 4U + i * kInternalPairSize;
            serialization::WriteI32(guard.Data(), off, node.keys[i]);
            serialization::WriteU32(guard.Data(), off + 4, node.children[i + 1U]);
        }
    }
    guard.MarkDirty();
}

PageId BPlusTree::NewNode(bool leaf, PageId parent_page_id) {
    PageId id = kInvalidPageId;
    PageGuard guard = NewGuarded(pool_, id);
    Zero(guard.Data());
    serialization::WriteU8(guard.Data(), 0, leaf ? kTypeBTreeLeaf : kTypeBTreeInternal);
    serialization::WriteU16(guard.Data(), kNodeKeyCountOffset, 0);
    serialization::WriteU32(guard.Data(), kNodeParentOffset, parent_page_id);
    serialization::WriteU32(guard.Data(), kNodeNextLeafOffset, kInvalidPageId);
    guard.MarkDirty();
    return id;
}

PageId BPlusTree::FindLeaf(std::int32_t key, std::vector<PageId>* ancestors,
                           std::uint32_t* nodes_visited) const {
    PageId current = RootPageId();
    while (true) {
        const Node node = ReadNode(current);
        if (nodes_visited != nullptr) {
            ++*nodes_visited;
        }
        if (node.is_leaf) {
            return current;
        }
        if (ancestors != nullptr) {
            ancestors->push_back(current);
        }
        std::size_t child_index = 0;
        while (child_index < node.keys.size() && key >= node.keys[child_index]) {
            ++child_index;
        }
        current = node.children[child_index];
    }
}

std::optional<RecordId> BPlusTree::Search(std::int32_t key, std::uint32_t* nodes_visited) const {
    const Node leaf = ReadNode(FindLeaf(key, nullptr, nodes_visited));
    const auto it = std::lower_bound(leaf.keys.begin(), leaf.keys.end(), key);
    if (it == leaf.keys.end() || *it != key) {
        return std::nullopt;
    }
    const std::size_t index = static_cast<std::size_t>(std::distance(leaf.keys.begin(), it));
    return leaf.record_ids[index];
}

void BPlusTree::SetParent(PageId page_id, PageId parent_page_id) {
    Node node = ReadNode(page_id);
    node.parent_page_id = parent_page_id;
    WriteNode(node);
}

void BPlusTree::IncrementSplitCount() {
    Header header = ReadHeader();
    ++header.split_count;
    WriteHeader(header);
}

void BPlusTree::Insert(std::int32_t key, RecordId rid) {
    if (!rid.IsValid()) {
        throw QueryError("B+ Tree: no se puede indexar un RecordId invalido");
    }

    std::vector<PageId> ancestors;
    const PageId leaf_id = FindLeaf(key, &ancestors);
    Node leaf = ReadNode(leaf_id);
    auto it = std::lower_bound(leaf.keys.begin(), leaf.keys.end(), key);
    if (it != leaf.keys.end() && *it == key) {
        throw QueryError("B+ Tree: clave duplicada " + std::to_string(key));
    }

    const std::size_t pos = static_cast<std::size_t>(std::distance(leaf.keys.begin(), it));
    leaf.keys.insert(leaf.keys.begin() + static_cast<std::ptrdiff_t>(pos), key);
    leaf.record_ids.insert(leaf.record_ids.begin() + static_cast<std::ptrdiff_t>(pos), rid);

    Header header = ReadHeader();
    ++header.entry_count;
    WriteHeader(header);

    if (leaf.keys.size() <= MaxKeys()) {
        WriteNode(leaf);
        return;
    }

    // Hoja con 2t claves: t en cada mitad; la primera clave de la derecha sube al padre.
    const std::size_t split = leaf.keys.size() / 2U;
    Node right;
    right.page_id = NewNode(true, leaf.parent_page_id);
    right.is_leaf = true;
    right.parent_page_id = leaf.parent_page_id;
    right.next_leaf_page_id = leaf.next_leaf_page_id;
    right.keys.assign(leaf.keys.begin() + static_cast<std::ptrdiff_t>(split), leaf.keys.end());
    right.record_ids.assign(leaf.record_ids.begin() + static_cast<std::ptrdiff_t>(split),
                            leaf.record_ids.end());

    leaf.keys.resize(split);
    leaf.record_ids.resize(split);
    leaf.next_leaf_page_id = right.page_id;
    WriteNode(leaf);
    WriteNode(right);
    IncrementSplitCount();

    const std::int32_t separator = right.keys.front();
    if (trace_ != nullptr) {
        *trace_ << "[B+Tree] split hoja page=" << leaf.page_id << " -> {" << leaf.page_id
                << ", " << right.page_id << "}, promote=" << separator << '\n';
    }
    InsertIntoParent(leaf.page_id, separator, right.page_id, ancestors);
}

void BPlusTree::InsertIntoParent(PageId left_page_id, std::int32_t separator_key,
                                 PageId right_page_id, std::vector<PageId>& ancestors) {
    if (ancestors.empty()) {
        const PageId new_root_id = NewNode(false, kInvalidPageId);
        Node root;
        root.page_id = new_root_id;
        root.is_leaf = false;
        root.parent_page_id = kInvalidPageId;
        root.keys = {separator_key};
        root.children = {left_page_id, right_page_id};
        WriteNode(root);
        SetParent(left_page_id, new_root_id);
        SetParent(right_page_id, new_root_id);

        Header header = ReadHeader();
        header.root_page_id = new_root_id;
        WriteHeader(header);
        if (trace_ != nullptr) {
            *trace_ << "[B+Tree] nueva raiz page=" << new_root_id << " key=" << separator_key
                    << "  -> altura del arbol = " << GetStatistics().height << '\n';
        }
        return;
    }

    const PageId parent_id = ancestors.back();
    ancestors.pop_back();
    Node parent = ReadNode(parent_id);
    const auto child_it = std::find(parent.children.begin(), parent.children.end(), left_page_id);
    if (child_it == parent.children.end()) {
        throw StorageError("B+ Tree: el padre no referencia al hijo izquierdo");
    }
    const std::size_t child_pos =
        static_cast<std::size_t>(std::distance(parent.children.begin(), child_it));
    parent.keys.insert(parent.keys.begin() + static_cast<std::ptrdiff_t>(child_pos), separator_key);
    parent.children.insert(parent.children.begin() + static_cast<std::ptrdiff_t>(child_pos + 1U),
                           right_page_id);

    if (parent.keys.size() <= MaxKeys()) {
        WriteNode(parent);
        SetParent(right_page_id, parent.page_id);
        return;
    }

    // En un nodo interno la clave central sube y no se duplica en ningún hijo.
    const std::size_t middle = parent.keys.size() / 2U;
    const std::int32_t promote = parent.keys[middle];

    Node right;
    right.page_id = NewNode(false, parent.parent_page_id);
    right.is_leaf = false;
    right.parent_page_id = parent.parent_page_id;
    right.keys.assign(parent.keys.begin() + static_cast<std::ptrdiff_t>(middle + 1U),
                      parent.keys.end());
    right.children.assign(parent.children.begin() + static_cast<std::ptrdiff_t>(middle + 1U),
                          parent.children.end());

    parent.keys.resize(middle);
    parent.children.resize(middle + 1U);
    WriteNode(parent);
    WriteNode(right);
    for (PageId child : right.children) {
        SetParent(child, right.page_id);
    }
    IncrementSplitCount();

    if (trace_ != nullptr) {
        *trace_ << "[B+Tree] split interno page=" << parent.page_id << " -> {" << parent.page_id
                << ", " << right.page_id << "}, promote=" << promote << '\n';
    }
    InsertIntoParent(parent.page_id, promote, right.page_id, ancestors);
}

void BPlusTree::BulkLoad(std::span<const std::pair<std::int32_t, RecordId>> entries) {
    if (entries.empty()) {
        return;
    }
    for (const auto& entry : entries) {
        if (!entry.second.IsValid()) {
            throw QueryError("B+ Tree: no se puede indexar un RecordId invalido");
        }
    }

    std::vector<std::pair<std::int32_t, RecordId>> sorted(entries.begin(), entries.end());
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    for (std::size_t i = 1; i < sorted.size(); ++i) {
        if (sorted[i].first == sorted[i - 1].first) {
            throw QueryError("B+ Tree: clave duplicada " + std::to_string(sorted[i].first));
        }
    }

    Header header = ReadHeader();
    if (header.entry_count != 0) {
        for (const auto& [key, rid] : sorted) {
            if (Search(key).has_value()) {
                throw QueryError("B+ Tree: clave duplicada " + std::to_string(key));
            }
        }
        for (const auto& [key, rid] : sorted) {
            Insert(key, rid);
        }
        return;
    }

    // Árbol vacío: hojas repartidas parejo (>= t claves salvo hoja única) y niveles hacia arriba.
    const std::size_t max_keys = MaxKeys();
    const std::size_t n = sorted.size();
    const std::size_t leaf_count = (n + max_keys - 1U) / max_keys;

    std::vector<Node> level;
    std::vector<std::int32_t> mins;  // clave mínima del subárbol de cada nodo de `level`
    level.reserve(leaf_count);
    std::size_t pos = 0;
    for (std::size_t i = 0; i < leaf_count; ++i) {
        const std::size_t size = n / leaf_count + (i < n % leaf_count ? 1U : 0U);
        Node leaf;
        leaf.page_id = (i == 0) ? header.root_page_id : NewNode(true, kInvalidPageId);
        leaf.is_leaf = true;
        for (std::size_t j = 0; j < size; ++j, ++pos) {
            leaf.keys.push_back(sorted[pos].first);
            leaf.record_ids.push_back(sorted[pos].second);
        }
        mins.push_back(leaf.keys.front());
        level.push_back(std::move(leaf));
    }
    for (std::size_t i = 0; i + 1 < level.size(); ++i) {
        level[i].next_leaf_page_id = level[i + 1].page_id;
    }

    while (level.size() > 1) {
        const std::size_t child_count = level.size();
        const std::size_t parent_count = (child_count + max_keys) / (max_keys + 1U);
        std::vector<Node> parents;
        std::vector<std::int32_t> parent_mins;
        std::size_t child_pos = 0;
        for (std::size_t p = 0; p < parent_count; ++p) {
            const std::size_t take = child_count / parent_count + (p < child_count % parent_count ? 1U : 0U);
            Node parent;
            parent.page_id = NewNode(false, kInvalidPageId);
            parent.is_leaf = false;
            parent_mins.push_back(mins[child_pos]);
            for (std::size_t j = 0; j < take; ++j, ++child_pos) {
                if (j > 0) {
                    parent.keys.push_back(mins[child_pos]);
                }
                parent.children.push_back(level[child_pos].page_id);
                level[child_pos].parent_page_id = parent.page_id;
            }
            parents.push_back(std::move(parent));
        }
        for (const Node& node : level) {
            WriteNode(node);
        }
        if (trace_ != nullptr) {
            *trace_ << "[B+Tree] bulk load: nivel de " << level.size() << " nodos -> "
                    << parents.size() << " padres\n";
        }
        level = std::move(parents);
        mins = std::move(parent_mins);
    }
    WriteNode(level.front());

    header.root_page_id = level.front().page_id;
    header.entry_count = n;
    WriteHeader(header);
    if (trace_ != nullptr) {
        *trace_ << "[B+Tree] bulk load: " << n << " entradas, " << leaf_count
                << " hojas, raiz page=" << header.root_page_id << '\n';
    }
}

struct BPlusTree::ValidationState {
    std::string error;
    std::int64_t leaf_depth = -1;
    std::uint64_t total_keys = 0;
    std::vector<Node> leaves;  // de izquierda a derecha
    PageId root = kInvalidPageId;
    std::size_t min_keys = 0;
};

bool BPlusTree::ValidateNode(PageId page_id, PageId expected_parent,
                             std::optional<std::int32_t> low, std::optional<std::int32_t> high,
                             std::uint32_t depth, ValidationState& st) const {
    auto fail = [&](const std::string& msg) {
        st.error = "page " + std::to_string(page_id) + ": " + msg;
        return false;
    };
    const Node node = ReadNode(page_id);
    if (node.parent_page_id != expected_parent) {
        return fail("puntero al padre incorrecto");
    }
    if (node.keys.size() > MaxKeys()) {
        return fail("nodo con mas de 2t-1 claves");
    }
    if (page_id != st.root && node.keys.size() < st.min_keys) {
        return fail("nodo (no raiz) con menos de t-1 claves");
    }
    for (std::size_t i = 0; i < node.keys.size(); ++i) {
        if (i > 0 && node.keys[i - 1] >= node.keys[i]) {
            return fail("claves no estrictamente ordenadas");
        }
        if ((low && node.keys[i] < *low) || (high && node.keys[i] >= *high)) {
            return fail("clave fuera del rango definido por los separadores del padre");
        }
    }
    if (node.is_leaf) {
        if (st.leaf_depth < 0) {
            st.leaf_depth = depth;
        } else if (st.leaf_depth != static_cast<std::int64_t>(depth)) {
            return fail("hojas a distinta profundidad (arbol desbalanceado)");
        }
        st.total_keys += node.keys.size();
        st.leaves.push_back(node);
        return true;
    }
    for (std::size_t i = 0; i < node.children.size(); ++i) {
        const auto child_low = (i == 0) ? low : std::optional<std::int32_t>(node.keys[i - 1]);
        const auto child_high =
            (i == node.keys.size()) ? high : std::optional<std::int32_t>(node.keys[i]);
        if (!ValidateNode(node.children[i], page_id, child_low, child_high, depth + 1U, st)) {
            return false;
        }
    }
    return true;
}

bool BPlusTree::Validate(std::string* error) const {
    ValidationState st;
    const Header header = ReadHeader();
    st.root = header.root_page_id;
    st.min_keys = header.minimum_degree - 1U;
    bool ok = ValidateNode(st.root, kInvalidPageId, std::nullopt, std::nullopt, 1, st);
    if (ok) {
        for (std::size_t i = 0; i < st.leaves.size(); ++i) {
            const PageId expected_next =
                (i + 1 < st.leaves.size()) ? st.leaves[i + 1].page_id : kInvalidPageId;
            if (st.leaves[i].next_leaf_page_id != expected_next) {
                st.error = "cadena de hojas incorrecta en page " +
                           std::to_string(st.leaves[i].page_id);
                ok = false;
                break;
            }
        }
    }
    if (ok && st.total_keys != header.entry_count) {
        st.error = "entry_count (" + std::to_string(header.entry_count) +
                   ") no coincide con las claves en hojas (" + std::to_string(st.total_keys) + ")";
        ok = false;
    }
    if (!ok && error != nullptr) {
        *error = st.error;
    }
    return ok;
}

BPlusTree::Statistics BPlusTree::GetStatistics() const {
    Statistics stats;
    const Header header = ReadHeader();
    stats.entry_count = header.entry_count;
    stats.split_count = header.split_count;

    std::queue<std::pair<PageId, std::uint32_t>> pending;
    pending.push({header.root_page_id, 1});
    while (!pending.empty()) {
        const auto [page_id, level] = pending.front();
        pending.pop();
        const Node node = ReadNode(page_id);
        ++stats.node_count;
        stats.height = std::max(stats.height, level);
        if (node.is_leaf) {
            ++stats.leaf_count;
        } else {
            ++stats.internal_count;
            for (PageId child : node.children) {
                pending.push({child, level + 1U});
            }
        }
    }
    return stats;
}

}  // namespace minidb
