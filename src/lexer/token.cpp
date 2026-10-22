#include "minidb/lexer/token.hpp"

namespace minidb {

std::string_view ToString(TokenType type) {
    switch (type) {
        case TokenType::kKeywordSelect: return "KEYWORD_SELECT";
        case TokenType::kKeywordFrom: return "KEYWORD_FROM";
        case TokenType::kKeywordWhere: return "KEYWORD_WHERE";
        case TokenType::kKeywordAnd: return "KEYWORD_AND";
        case TokenType::kKeywordOr: return "KEYWORD_OR";
        case TokenType::kIdentifier: return "IDENTIFIER";
        case TokenType::kIntegerLiteral: return "INTEGER_LITERAL";
        case TokenType::kDecimalLiteral: return "DECIMAL_LITERAL";
        case TokenType::kStringLiteral: return "STRING_LITERAL";
        case TokenType::kEqual: return "EQUAL";
        case TokenType::kNotEqual: return "NOT_EQUAL";
        case TokenType::kLess: return "LESS";
        case TokenType::kGreater: return "GREATER";
        case TokenType::kLessEqual: return "LESS_EQUAL";
        case TokenType::kGreaterEqual: return "GREATER_EQUAL";
        case TokenType::kComma: return "COMMA";
        case TokenType::kSemicolon: return "SEMICOLON";
        case TokenType::kAsterisk: return "ASTERISK";
        case TokenType::kEndOfInput: return "EOF";
    }
    return "UNKNOWN";
}

}  // namespace minidb
