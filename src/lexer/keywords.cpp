#include "minidb/lexer/keywords.hpp"

#include <array>
#include <cctype>
#include <utility>

namespace minidb {
namespace {

// Palabras reservadas en mayusculas; la entrada se normaliza antes de comparar.
constexpr std::array<std::pair<std::string_view, TokenType>, 5> kKeywords{{
    {"SELECT", TokenType::kKeywordSelect},
    {"FROM", TokenType::kKeywordFrom},
    {"WHERE", TokenType::kKeywordWhere},
    {"AND", TokenType::kKeywordAnd},
    {"OR", TokenType::kKeywordOr},
}};

bool EqualsIgnoreCase(std::string_view word, std::string_view upper_keyword) {
    if (word.size() != upper_keyword.size()) {
        return false;
    }
    for (std::size_t i = 0; i < word.size(); ++i) {
        if (std::toupper(static_cast<unsigned char>(word[i])) != upper_keyword[i]) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::optional<TokenType> LookupKeyword(std::string_view word) {
    for (const auto& [keyword, type] : kKeywords) {
        if (EqualsIgnoreCase(word, keyword)) {
            return type;
        }
    }
    return std::nullopt;
}

}  // namespace minidb
