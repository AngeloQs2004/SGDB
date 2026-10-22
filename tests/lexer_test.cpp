#include "minidb/lexer/lexer.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace minidb {
namespace {

using Type = TokenType;

LexResult Lex(const std::string& input) { return Lexer(input).Tokenize(); }

std::vector<TokenType> Types(const LexResult& result) {
    std::vector<TokenType> types;
    for (const Token& token : result.tokens) {
        types.push_back(token.type);
    }
    return types;
}

std::vector<std::string> Lexemes(const LexResult& result) {
    std::vector<std::string> lexemes;
    for (const Token& token : result.tokens) {
        lexemes.push_back(token.lexeme);
    }
    return lexemes;
}

// --- Sentencia valida ------------------------------------------------------

TEST(LexerTest, ClassifiesASimpleSelect) {
    const LexResult result = Lex("SELECT nombre, edad FROM estudiantes;");

    EXPECT_TRUE(result.Ok());
    EXPECT_EQ(Types(result),
              (std::vector<TokenType>{Type::kKeywordSelect, Type::kIdentifier, Type::kComma,
                                      Type::kIdentifier, Type::kKeywordFrom, Type::kIdentifier,
                                      Type::kSemicolon, Type::kEndOfInput}));
    EXPECT_EQ(Lexemes(result), (std::vector<std::string>{"SELECT", "nombre", ",", "edad", "FROM",
                                                         "estudiantes", ";", ""}));
}

TEST(LexerTest, TokensKeepTheirOrderOfAppearance) {
    const LexResult result = Lex("FROM SELECT FROM");
    EXPECT_EQ(Types(result), (std::vector<TokenType>{Type::kKeywordFrom, Type::kKeywordSelect,
                                                     Type::kKeywordFrom, Type::kEndOfInput}));
}

TEST(LexerTest, ToStringUsesTheNamesOfTheSpecification) {
    EXPECT_EQ(ToString(Type::kKeywordSelect), "KEYWORD_SELECT");
    EXPECT_EQ(ToString(Type::kIdentifier), "IDENTIFIER");
    EXPECT_EQ(ToString(Type::kIntegerLiteral), "INTEGER_LITERAL");
    EXPECT_EQ(ToString(Type::kComma), "COMMA");
    EXPECT_EQ(ToString(Type::kSemicolon), "SEMICOLON");
    EXPECT_EQ(ToString(Type::kEndOfInput), "EOF");
}

// --- Palabras reservadas e identificadores ---------------------------------

TEST(LexerTest, KeywordsIgnoreCase) {
    const LexResult result = Lex("select Select SELECT sElEcT");
    for (std::size_t i = 0; i < 4; ++i) {
        EXPECT_EQ(result.tokens[i].type, Type::kKeywordSelect);
    }
    // El lexema conserva lo que escribio el usuario.
    EXPECT_EQ(result.tokens[0].lexeme, "select");
    EXPECT_EQ(result.tokens[3].lexeme, "sElEcT");
}

TEST(LexerTest, AllReservedWordsAreRecognised) {
    const LexResult result = Lex("SELECT FROM WHERE AND OR");
    EXPECT_EQ(Types(result),
              (std::vector<TokenType>{Type::kKeywordSelect, Type::kKeywordFrom,
                                      Type::kKeywordWhere, Type::kKeywordAnd, Type::kKeywordOr,
                                      Type::kEndOfInput}));
}

TEST(LexerTest, WordsContainingAKeywordAreIdentifiers) {
    const LexResult result = Lex("selected from_table orders where1 _or AND_");
    for (std::size_t i = 0; i + 1 < result.tokens.size(); ++i) {
        EXPECT_EQ(result.tokens[i].type, Type::kIdentifier) << result.tokens[i].lexeme;
    }
}

TEST(LexerTest, IdentifiersMayContainDigitsAndUnderscores) {
    const LexResult result = Lex("tabla_1 _x col2");
    EXPECT_TRUE(result.Ok());
    EXPECT_EQ(Lexemes(result), (std::vector<std::string>{"tabla_1", "_x", "col2", ""}));
}

// --- Enteros y puntuacion --------------------------------------------------

TEST(LexerTest, RecognisesIntegerLiterals) {
    const LexResult result = Lex("18 0 2026");
    EXPECT_EQ(Types(result),
              (std::vector<TokenType>{Type::kIntegerLiteral, Type::kIntegerLiteral,
                                      Type::kIntegerLiteral, Type::kEndOfInput}));
    EXPECT_EQ(result.tokens[2].lexeme, "2026");
}

TEST(LexerTest, PunctuationNeedsNoSurroundingSpaces) {
    const LexResult result = Lex("a,b;");
    EXPECT_EQ(Types(result),
              (std::vector<TokenType>{Type::kIdentifier, Type::kComma, Type::kIdentifier,
                                      Type::kSemicolon, Type::kEndOfInput}));
}

// --- Espacios en blanco y posiciones ---------------------------------------

TEST(LexerTest, WhitespaceIsIgnored) {
    const LexResult result = Lex("  SELECT\t\ta \r\n\n  FROM   t  ");
    EXPECT_TRUE(result.Ok());
    EXPECT_EQ(Types(result),
              (std::vector<TokenType>{Type::kKeywordSelect, Type::kIdentifier, Type::kKeywordFrom,
                                      Type::kIdentifier, Type::kEndOfInput}));
}

TEST(LexerTest, RecordsLineAndColumnWhereEachTokenStarts) {
    const LexResult result = Lex("SELECT nombre,\n  edad\nFROM t;");

    struct Expected {
        std::size_t line;
        std::size_t column;
    };
    const std::vector<Expected> expected = {{1, 1}, {1, 8}, {1, 14}, {2, 3},
                                            {3, 1}, {3, 6}, {3, 7},  {3, 8}};
    ASSERT_EQ(result.tokens.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(result.tokens[i].line, expected[i].line) << "token " << i;
        EXPECT_EQ(result.tokens[i].column, expected[i].column) << "token " << i;
    }
}

TEST(LexerTest, EofPositionIsTheEndOfTheInput) {
    const LexResult result = Lex("a\nbc");
    EXPECT_EQ(result.tokens.back().type, Type::kEndOfInput);
    EXPECT_EQ(result.tokens.back().line, 2U);
    EXPECT_EQ(result.tokens.back().column, 3U);
}

// --- Entrada vacia y errores basicos ---------------------------------------

TEST(LexerTest, EmptyInputProducesOnlyEof) {
    for (const char* input : {"", "   ", "\n\t\r\n"}) {
        const LexResult result = Lex(input);
        EXPECT_TRUE(result.Ok());
        ASSERT_EQ(result.tokens.size(), 1U) << "entrada: '" << input << "'";
        EXPECT_EQ(result.tokens[0].type, Type::kEndOfInput);
    }
}

TEST(LexerTest, UnknownCharacterIsReportedWithItsPositionAndAnalysisContinues) {
    const LexResult result = Lex("a @ b");

    ASSERT_EQ(result.errors.size(), 1U);
    EXPECT_EQ(result.errors[0].line, 1U);
    EXPECT_EQ(result.errors[0].column, 3U);
    EXPECT_NE(result.errors[0].message.find('@'), std::string::npos);
    // El caracter invalido no aparece como token y lo demas se sigue leyendo.
    EXPECT_EQ(Types(result), (std::vector<TokenType>{Type::kIdentifier, Type::kIdentifier,
                                                     Type::kEndOfInput}));
}

TEST(LexerTest, AllErrorsAreReportedInOnePass) {
    const LexResult result = Lex("a $\n# b");
    ASSERT_EQ(result.errors.size(), 2U);
    EXPECT_EQ(result.errors[0].line, 1U);
    EXPECT_EQ(result.errors[0].column, 3U);
    EXPECT_EQ(result.errors[1].line, 2U);
    EXPECT_EQ(result.errors[1].column, 1U);
}

TEST(LexerTest, NonAsciiCharacterCountsAsOneColumnAndOneError) {
    const LexResult result = Lex("a \xC3\xB1 b");  // "a ñ b"
    ASSERT_EQ(result.errors.size(), 1U);
    EXPECT_EQ(result.errors[0].column, 3U);
    // 'b' esta en la columna 5: la ñ ocupa una sola columna aunque sean 2 bytes.
    EXPECT_EQ(result.tokens[1].lexeme, "b");
    EXPECT_EQ(result.tokens[1].column, 5U);
}

}  // namespace
}  // namespace minidb
