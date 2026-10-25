#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "minidb/lexer/token.hpp"

namespace minidb {

/// Un problema lexico y el lugar donde empieza (linea y columna desde 1).
struct LexicalError {
    std::string message;
    std::size_t line = 1;
    std::size_t column = 1;
};

/// Resultado de analizar una entrada: los tokens validos en orden de aparicion
/// (siempre terminan en EOF) y los errores encontrados. Un lexema invalido
/// nunca aparece como token.
struct LexResult {
    std::vector<Token> tokens;
    std::vector<LexicalError> errors;

    [[nodiscard]] bool Ok() const { return errors.empty(); }
};

/// Analizador lexico del subconjunto SQL. Solo clasifica lexemas: no ejecuta la
/// consulta ni comprueba que las tablas o columnas existan.
///
/// Se recorre la entrada de izquierda a derecha, un caracter a la vez, y en cada
/// posicion se decide por el primer caracter que token empieza ahi.
class Lexer {
public:
    explicit Lexer(std::string_view input) : input_(input) {}

    /// Analiza toda la entrada. Ante un error lo registra y sigue, para informar
    /// de todos los problemas en una sola pasada.
    [[nodiscard]] LexResult Tokenize();

private:
    [[nodiscard]] bool AtEnd() const { return pos_ >= input_.size(); }
    [[nodiscard]] char Peek() const { return AtEnd() ? '\0' : input_[pos_]; }
    [[nodiscard]] char PeekNext() const { return pos_ + 1 < input_.size() ? input_[pos_ + 1] : '\0'; }
    char Advance();
    void SkipWhitespace();

    Token ScanWord();
    void ScanNumber();
    void ScanString();
    void ScanOperator();
    Token ScanPunctuation(TokenType type);
    void ReportUnexpectedCharacter(std::size_t line, std::size_t column);

    std::string_view input_;
    std::size_t pos_ = 0;
    std::size_t line_ = 1;
    std::size_t column_ = 1;
    LexResult result_;
};

}  // namespace minidb
