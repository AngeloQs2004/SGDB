#include "minidb/lexer/lexer.hpp"

#include <cctype>

#include "minidb/lexer/keywords.hpp"

namespace minidb {
namespace {

bool IsWordStart(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool IsWordPart(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

// Los bytes 10xxxxxx de UTF-8 continuan un caracter ya empezado; no cuentan
// como una columna nueva.
bool IsUtf8Continuation(char c) {
    return (static_cast<unsigned char>(c) & 0xC0U) == 0x80U;
}

}  // namespace

char Lexer::Advance() {
    const char c = input_[pos_++];
    if (c == '\n') {
        ++line_;
        column_ = 1;
    } else if (!IsUtf8Continuation(c)) {
        ++column_;
    }
    return c;
}

void Lexer::SkipWhitespace() {
    while (!AtEnd()) {
        const char c = Peek();
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            return;
        }
        Advance();
    }
}

Token Lexer::ScanWord() {
    const std::size_t start = pos_;
    Token token;
    token.line = line_;
    token.column = column_;

    while (!AtEnd() && IsWordPart(Peek())) {
        Advance();
    }

    token.lexeme = std::string(input_.substr(start, pos_ - start));
    // Se lee como palabra y luego se consulta la tabla: asi "selected" sigue
    // siendo un identificador aunque empiece con una palabra reservada.
    token.type = LookupKeyword(token.lexeme).value_or(TokenType::kIdentifier);
    return token;
}

void Lexer::ReportUnexpectedCharacter(std::size_t line, std::size_t column) {
    const std::size_t start = pos_;
    Advance();
    // Un caracter no ASCII ocupa varios bytes: se informa como uno solo.
    while (!AtEnd() && IsUtf8Continuation(Peek())) {
        Advance();
    }
    result_.errors.push_back(LexicalError{
        "Caracter no admitido: '" + std::string(input_.substr(start, pos_ - start)) + "'", line,
        column});
}

LexResult Lexer::Tokenize() {
    result_ = LexResult{};

    while (true) {
        SkipWhitespace();
        if (AtEnd()) {
            break;
        }

        if (IsWordStart(Peek())) {
            result_.tokens.push_back(ScanWord());
        } else {
            ReportUnexpectedCharacter(line_, column_);
        }
    }

    result_.tokens.push_back(Token{TokenType::kEndOfInput, "", line_, column_});
    return std::move(result_);
}

}  // namespace minidb
