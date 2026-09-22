#include "minidb/index/bplus_tree.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <optional>
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

// Capacidad cableada (equivale a t = 16: 2t - 1 claves). Se reemplaza en el commit 3.
constexpr std::size_t kMaxKeys = 31;

[[nodiscard]] std::span<const std::byte> ConstBytes(std::span<std::byte> bytes) {
    return {bytes.data(), bytes.size()};
}

void Zero(std::span<std::byte> bytes) {
    std::fill(bytes.begin(), bytes.end(), std::byte{0});
}

}  // namespace

PageId BPlusTree::Create(BufferPoolManager& pool, std::uint16_t minimum_degree) {
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
    if (header.root_page_id == kInvalidPageId) {
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
    if (node.keys.size() > kMaxKeys) {
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

    if (leaf.keys.size() <= kMaxKeys) {
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
                    << '\n';
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

    if (parent.keys.size() <= kMaxKeys) {
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

void BPlusTree::BulkLoad(std::span<const std::pair<std::int32_t, RecordId>>) {
    // TODO(commit 4): construccion bottom-up.
    throw QueryError("B+ Tree: BulkLoad aun no implementado");
}

}  // namespace minidb
