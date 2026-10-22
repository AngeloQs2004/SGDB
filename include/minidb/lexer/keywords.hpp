#pragma once

#include <optional>
#include <string_view>

#include "minidb/lexer/token.hpp"

namespace minidb {

/// Busca `word` en la tabla de palabras reservadas (SELECT, FROM, WHERE, AND,
/// OR). La comparacion ignora mayusculas y minusculas: "select" y "SeLeCt" son
/// la misma palabra reservada. Devuelve std::nullopt si es un identificador.
[[nodiscard]] std::optional<TokenType> LookupKeyword(std::string_view word);

}  // namespace minidb
