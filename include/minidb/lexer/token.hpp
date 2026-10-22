#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace minidb {

/// Categorias lexicas del subconjunto SQL. ToString() devuelve el nombre en el
/// estilo del enunciado (KEYWORD_SELECT, IDENTIFIER, ...).
enum class TokenType {
    kKeywordSelect,
    kKeywordFrom,
    kKeywordWhere,
    kKeywordAnd,
    kKeywordOr,
    kIdentifier,
    kIntegerLiteral,
    kDecimalLiteral,
    kStringLiteral,
    kEqual,
    kNotEqual,
    kLess,
    kGreater,
    kLessEqual,
    kGreaterEqual,
    kComma,
    kSemicolon,
    kAsterisk,
    kEndOfInput,
};

/// Nombre legible de la categoria, por ejemplo "GREATER_EQUAL".
[[nodiscard]] std::string_view ToString(TokenType type);

/// Un lexema reconocido, su categoria y donde empieza en la entrada.
/// Linea y columna comienzan en 1; la columna cuenta caracteres de esa linea.
struct Token {
    TokenType type = TokenType::kEndOfInput;
    std::string lexeme;
    std::size_t line = 1;
    std::size_t column = 1;

    bool operator==(const Token&) const = default;
};

}  // namespace minidb
