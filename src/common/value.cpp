#include "minidb/common/value.hpp"

#include <compare>
#include <format>

namespace minidb {
namespace {

/// Abbreviated form of a vector: enough components to recognise it, plus its
/// dimension. Printing 128 floats in a result table would make it unreadable.
constexpr std::size_t kVectorPreviewComponents = 3;

[[nodiscard]] std::string VectorToString(const Vector& vector) {
    std::string text = "[";
    const std::size_t shown = std::min(kVectorPreviewComponents, vector.size());
    for (std::size_t i = 0; i < shown; ++i) {
        text += std::format("{}{:.4g}", i == 0 ? "" : ", ", vector[i]);
    }
    if (vector.size() > shown) {
        text += std::format(", ... ({} dims)", vector.size());
    }
    return text + "]";
}

}  // namespace

std::string ValueToString(const Value& value) {
    if (std::holds_alternative<std::int32_t>(value)) {
        return std::to_string(std::get<std::int32_t>(value));
    }
    if (std::holds_alternative<Vector>(value)) {
        return VectorToString(std::get<Vector>(value));
    }
    if (std::holds_alternative<float>(value)) {
        return std::format("{:.6g}", std::get<float>(value));
    }
    return std::get<std::string>(value);
}

bool CompareValues(const Value& left, CompareOperator op, const Value& right) {
    if (left.index() != right.index()) {
        throw QueryError("No se pueden comparar valores de tipos distintos");
    }

    // A vector has no single natural order — that is the whole reason similarity
    // search exists — so an ordering comparison over one is a query error rather
    // than an arbitrary answer. Nearest neighbour queries compare *distances*,
    // which are floats.
    if (std::holds_alternative<Vector>(left)) {
        throw QueryError(
            "No se puede comparar por orden una columna VECTOR; use una consulta "
            "de vecinos más cercanos (NEAREST ... TO ...)");
    }

    // Every remaining alternative is totally ordered, so one three-way comparison
    // drives every operator.
    std::strong_ordering ordering = std::strong_ordering::equal;
    if (std::holds_alternative<std::int32_t>(left)) {
        ordering = std::get<std::int32_t>(left) <=> std::get<std::int32_t>(right);
    } else if (std::holds_alternative<float>(left)) {
        // Distances are never NaN here: they come from finite inputs, and the
        // metrics reject a query whose dimension does not match. A partial
        // ordering is collapsed deliberately so the switch below stays uniform.
        const float a = std::get<float>(left);
        const float b = std::get<float>(right);
        ordering = a < b ? std::strong_ordering::less
                         : (a > b ? std::strong_ordering::greater : std::strong_ordering::equal);
    } else {
        ordering = std::get<std::string>(left) <=> std::get<std::string>(right);
    }

    switch (op) {
        case CompareOperator::kEqual:
            return ordering == 0;
        case CompareOperator::kNotEqual:
            return ordering != 0;
        case CompareOperator::kLess:
            return ordering < 0;
        case CompareOperator::kLessEqual:
            return ordering <= 0;
        case CompareOperator::kGreater:
            return ordering > 0;
        case CompareOperator::kGreaterEqual:
            return ordering >= 0;
    }
    throw StorageError("Operador de comparación desconocido");
}

}  // namespace minidb
