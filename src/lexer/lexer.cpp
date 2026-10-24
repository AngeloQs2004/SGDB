#include "minidb/lexer/lexer.hpp"

#include <cctype>

#include "minidb/lexer/keywords.hpp"

namespace minidb {
namespace {

bool IsWordStart(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool IsDigit(char c) {
    return std::isdigit(static_cast<unsigned char>(c)) != 0;
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

Token Lexer::ScanNumber() {
    const std::size_t start = pos_;
    Token token;
    token.line = line_;
    token.column = column_;

    while (!AtEnd() && IsDigit(Peek())) {
        Advance();
    }

    token.type = TokenType::kIntegerLiteral;
    // Un punto solo forma parte del numero si lo sigue un digito: "3.14" es un
    // decimal, pero en "3." el punto no se consume.
    if (Peek() == '.' && IsDigit(PeekNext())) {
        Advance();
        while (!AtEnd() && IsDigit(Peek())) {
            Advance();
        }
        token.type = TokenType::kDecimalLiteral;
    }
    token.lexeme = std::string(input_.substr(start, pos_ - start));
    return token;
}

// Cadena entre comillas simples. Dentro de ella '' representa una comilla. El
// lexema conserva las comillas tal como estan en la entrada; quitarlas es
// trabajo de quien interprete el literal. Puede abarcar varias lineas.
void Lexer::ScanString() {
    const std::size_t start = pos_;
    const std::size_t line = line_;
    const std::size_t column = column_;

    Advance();  // comilla de apertura
    while (!AtEnd()) {
        if (Peek() == '\'') {
            Advance();
            if (Peek() != '\'') {
                result_.tokens.push_back(Token{TokenType::kStringLiteral,
                                               std::string(input_.substr(start, pos_ - start)),
                                               line, column});
                return;
            }
        }
        Advance();  // contenido, o la segunda comilla de ''
    }

    // Se llego al final sin la comilla de cierre: no es un token valido.
    result_.errors.push_back(LexicalError{"Cadena sin cerrar", line, column});
}

// Operadores relacionales. Se toma siempre el lexema mas largo posible: tras '<'
// se mira el siguiente caracter para decidir entre '<', '<=' y '<>', de modo
// que un operador compuesto nunca se parte en dos tokens.
void Lexer::ScanOperator() {
    const std::size_t line = line_;
    const std::size_t column = column_;
    const char first = Advance();
    TokenType type = TokenType::kEqual;
    std::string lexeme(1, first);

    switch (first) {
        case '=':
            break;
        case '<':
            type = TokenType::kLess;
            if (Peek() == '=') {
                type = TokenType::kLessEqual;
                lexeme += Advance();
            } else if (Peek() == '>') {
                type = TokenType::kNotEqual;
                lexeme += Advance();
            }
            break;
        case '>':
            type = TokenType::kGreater;
            if (Peek() == '=') {
                type = TokenType::kGreaterEqual;
                lexeme += Advance();
            }
            break;
        default:  // '!'
            if (Peek() != '=') {
                result_.errors.push_back(
                    LexicalError{"Caracter no admitido: '!' (solo es valido como '!=')", line, column});
                return;
            }
            type = TokenType::kNotEqual;
            lexeme += Advance();
            break;
    }
    result_.tokens.push_back(Token{type, std::move(lexeme), line, column});
}

Token Lexer::ScanPunctuation(TokenType type) {
    Token token;
    token.type = type;
    token.line = line_;
    token.column = column_;
    token.lexeme = std::string(1, Advance());
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

        const char c = Peek();
        if (IsWordStart(c)) {
            result_.tokens.push_back(ScanWord());
        } else if (IsDigit(c)) {
            result_.tokens.push_back(ScanNumber());
        } else if (c == '\'') {
            ScanString();
        } else if (c == '=' || c == '<' || c == '>' || c == '!') {
            ScanOperator();
        } else if (c == ',') {
            result_.tokens.push_back(ScanPunctuation(TokenType::kComma));
        } else if (c == ';') {
            result_.tokens.push_back(ScanPunctuation(TokenType::kSemicolon));
        } else if (c == '*') {
            result_.tokens.push_back(ScanPunctuation(TokenType::kAsterisk));
        } else {
            ReportUnexpectedCharacter(line_, column_);
        }
    }

    result_.tokens.push_back(Token{TokenType::kEndOfInput, "", line_, column_});
    return std::move(result_);
}

}  // namespace minidb
