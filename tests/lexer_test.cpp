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

// --- Literales -------------------------------------------------------------

TEST(LexerTest, RecognisesDecimalLiterals) {
    const LexResult result = Lex("3.14 0.5 100.0");
    EXPECT_EQ(Types(result),
              (std::vector<TokenType>{Type::kDecimalLiteral, Type::kDecimalLiteral,
                                      Type::kDecimalLiteral, Type::kEndOfInput}));
    EXPECT_EQ(Lexemes(result), (std::vector<std::string>{"3.14", "0.5", "100.0", ""}));
}

TEST(LexerTest, IntegerAndDecimalAreTokenisedDifferently) {
    const LexResult result = Lex("18, 18.5");
    EXPECT_EQ(result.tokens[0].type, Type::kIntegerLiteral);
    EXPECT_EQ(result.tokens[2].type, Type::kDecimalLiteral);
}

TEST(LexerTest, RecognisesStringLiterals) {
    const LexResult result = Lex("'Ana' 'Ciencia de la Computacion' ''");
    EXPECT_TRUE(result.Ok());
    EXPECT_EQ(Types(result),
              (std::vector<TokenType>{Type::kStringLiteral, Type::kStringLiteral,
                                      Type::kStringLiteral, Type::kEndOfInput}));
    // El lexema conserva las comillas.
    EXPECT_EQ(result.tokens[0].lexeme, "'Ana'");
    EXPECT_EQ(result.tokens[2].lexeme, "''");
}

TEST(LexerTest, StringsMayContainKeywordsOperatorsAndSpaces) {
    const LexResult result = Lex("'SELECT >= 5, x;'");
    ASSERT_EQ(result.tokens.size(), 2U);
    EXPECT_EQ(result.tokens[0].type, Type::kStringLiteral);
    EXPECT_EQ(result.tokens[0].lexeme, "'SELECT >= 5, x;'");
}

TEST(LexerTest, DoubledQuoteInsideAStringIsAnEscapedQuote) {
    const LexResult result = Lex("'it''s' x");
    EXPECT_TRUE(result.Ok());
    EXPECT_EQ(result.tokens[0].lexeme, "'it''s'");
    EXPECT_EQ(result.tokens[1].lexeme, "x");
}

TEST(LexerTest, StringCanSpanSeveralLinesAndKeepsPositionsAfterIt) {
    const LexResult result = Lex("'a\nb' x");
    EXPECT_TRUE(result.Ok());
    EXPECT_EQ(result.tokens[0].line, 1U);
    EXPECT_EQ(result.tokens[1].line, 2U);
    EXPECT_EQ(result.tokens[1].column, 4U);
}

// --- Operadores relacionales -----------------------------------------------

TEST(LexerTest, RecognisesEveryRelationalOperator) {
    const LexResult result = Lex("= <> != < > <= >=");
    EXPECT_TRUE(result.Ok());
    EXPECT_EQ(Types(result),
              (std::vector<TokenType>{Type::kEqual, Type::kNotEqual, Type::kNotEqual, Type::kLess,
                                      Type::kGreater, Type::kLessEqual, Type::kGreaterEqual,
                                      Type::kEndOfInput}));
    EXPECT_EQ(Lexemes(result),
              (std::vector<std::string>{"=", "<>", "!=", "<", ">", "<=", ">=", ""}));
}

TEST(LexerTest, TwoCharacterOperatorsAreSingleTokens) {
    for (const char* op : {">=", "<=", "<>", "!="}) {
        const LexResult result = Lex(std::string("a ") + op + " b");
        ASSERT_EQ(result.tokens.size(), 4U) << op;
        EXPECT_EQ(result.tokens[1].lexeme, op);
    }
}

TEST(LexerTest, SplitOperatorIsTwoTokens) {
    // Con un espacio en medio ya no es un operador compuesto.
    const LexResult result = Lex("> =");
    EXPECT_EQ(Types(result), (std::vector<TokenType>{Type::kGreater, Type::kEqual,
                                                     Type::kEndOfInput}));
}

TEST(LexerTest, OperatorsNeedNoSurroundingSpaces) {
    const LexResult result = Lex("edad>=18");
    EXPECT_EQ(Types(result),
              (std::vector<TokenType>{Type::kIdentifier, Type::kGreaterEqual,
                                      Type::kIntegerLiteral, Type::kEndOfInput}));
}

TEST(LexerTest, ConsecutiveOperatorsAreNotMerged) {
    const LexResult result = Lex(">=<=!=");
    EXPECT_EQ(Types(result), (std::vector<TokenType>{Type::kGreaterEqual, Type::kLessEqual,
                                                     Type::kNotEqual, Type::kEndOfInput}));
}

TEST(LexerTest, AsteriskIsATokenOfItsOwn) {
    const LexResult result = Lex("SELECT * FROM t");
    EXPECT_EQ(Types(result),
              (std::vector<TokenType>{Type::kKeywordSelect, Type::kAsterisk, Type::kKeywordFrom,
                                      Type::kIdentifier, Type::kEndOfInput}));
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

// --- Errores lexicos -------------------------------------------------------

TEST(LexerTest, UnterminatedStringIsAnErrorAtTheOpeningQuote) {
    const LexResult result = Lex("SELECT 'sin cerrar");
    ASSERT_EQ(result.errors.size(), 1U);
    EXPECT_EQ(result.errors[0].message, "Cadena sin cerrar");
    EXPECT_EQ(result.errors[0].line, 1U);
    EXPECT_EQ(result.errors[0].column, 8U);
    // La cadena rota no se clasifica como literal valido.
    EXPECT_EQ(Types(result), (std::vector<TokenType>{Type::kKeywordSelect, Type::kEndOfInput}));
}

TEST(LexerTest, UnterminatedStringOnALaterLineReportsWhereItStarted) {
    const LexResult result = Lex("SELECT a\nFROM t\nWHERE n = 'Ana");
    ASSERT_EQ(result.errors.size(), 1U);
    EXPECT_EQ(result.errors[0].line, 3U);
    EXPECT_EQ(result.errors[0].column, 11U);
}

TEST(LexerTest, StringEndingWithADoubledQuoteIsStillUnterminated) {
    const LexResult result = Lex("'abc''");
    ASSERT_EQ(result.errors.size(), 1U);
    EXPECT_EQ(result.errors[0].message, "Cadena sin cerrar");
}

TEST(LexerTest, MalformedNumbersAreReportedAsOneError) {
    for (const char* input : {"12abc", "3.", "1.2.3", "7x"}) {
        const LexResult result = Lex(input);
        ASSERT_EQ(result.errors.size(), 1U) << input;
        EXPECT_NE(result.errors[0].message.find("Numero mal formado"), std::string::npos) << input;
        EXPECT_EQ(result.errors[0].column, 1U) << input;
        // Ninguna parte del numero roto aparece como token valido.
        EXPECT_EQ(Types(result), (std::vector<TokenType>{Type::kEndOfInput})) << input;
    }
}

TEST(LexerTest, LoneBangIsAnErrorButNotEqualIsFine) {
    const LexResult bad = Lex("a ! b");
    ASSERT_EQ(bad.errors.size(), 1U);
    EXPECT_EQ(bad.errors[0].column, 3U);
    EXPECT_TRUE(Lex("a != b").Ok());
}

TEST(LexerTest, ErrorsDoNotStopTheAnalysis) {
    const LexResult result = Lex("SELECT 12abc, @ FROM 'x");
    EXPECT_EQ(result.errors.size(), 3U);
    // Lo valido que rodea a los errores se reconoce igualmente.
    EXPECT_EQ(Types(result), (std::vector<TokenType>{Type::kKeywordSelect, Type::kComma,
                                                     Type::kKeywordFrom, Type::kEndOfInput}));
}

TEST(LexerTest, ErrorsAreReportedInOrderOfPosition) {
    const LexResult result = Lex("@ a\n$ 1x");
    ASSERT_EQ(result.errors.size(), 3U);
    EXPECT_EQ(result.errors[0].line, 1U);
    EXPECT_EQ(result.errors[1].line, 2U);
    EXPECT_EQ(result.errors[1].column, 1U);
    EXPECT_EQ(result.errors[2].line, 2U);
    EXPECT_EQ(result.errors[2].column, 3U);
}

// --- Escenarios del enunciado ----------------------------------------------
// Un test por cada escenario de la seccion de pruebas del laboratorio.

// Escenario 1: sentencia valida con SELECT, FROM y WHERE (Cuadro 1 del enunciado).
TEST(LexerScenarios, ValidStatementWithSelectFromWhere) {
    const LexResult result = Lex("SELECT nombre, edad\nFROM estudiantes\nWHERE edad >= 18;");

    EXPECT_TRUE(result.Ok());
    EXPECT_EQ(Types(result),
              (std::vector<TokenType>{
                  Type::kKeywordSelect, Type::kIdentifier, Type::kComma, Type::kIdentifier,
                  Type::kKeywordFrom, Type::kIdentifier, Type::kKeywordWhere, Type::kIdentifier,
                  Type::kGreaterEqual, Type::kIntegerLiteral, Type::kSemicolon,
                  Type::kEndOfInput}));
    EXPECT_EQ(Lexemes(result),
              (std::vector<std::string>{"SELECT", "nombre", ",", "edad", "FROM", "estudiantes",
                                        "WHERE", "edad", ">=", "18", ";", ""}));
}

// Escenario 2: >=, <=, <> y != se reconocen como tokens individuales.
TEST(LexerScenarios, CompoundOperatorsAreIndividualTokens) {
    const LexResult result = Lex("a>=1 b<=2 c<>3 d!=4");
    std::vector<std::string> operators;
    for (const Token& token : result.tokens) {
        if (token.type != Type::kIdentifier && token.type != Type::kIntegerLiteral &&
            token.type != Type::kEndOfInput) {
            operators.push_back(token.lexeme);
        }
    }
    EXPECT_EQ(operators, (std::vector<std::string>{">=", "<=", "<>", "!="}));
    EXPECT_TRUE(result.Ok());
}

// Escenario 3: enteros, decimales y cadenas de texto.
TEST(LexerScenarios, IntegerDecimalAndStringLiterals) {
    const LexResult result = Lex("42 3.14 'texto'");
    EXPECT_EQ(Types(result),
              (std::vector<TokenType>{Type::kIntegerLiteral, Type::kDecimalLiteral,
                                      Type::kStringLiteral, Type::kEndOfInput}));
}

// Escenario 4: palabras reservadas distintas de identificadores.
TEST(LexerScenarios, ReservedWordsAreToldApartFromIdentifiers) {
    const LexResult result = Lex("select selection from fromage where wherever and android or order");
    const std::vector<TokenType> expected = {
        Type::kKeywordSelect, Type::kIdentifier, Type::kKeywordFrom, Type::kIdentifier,
        Type::kKeywordWhere,  Type::kIdentifier, Type::kKeywordAnd,  Type::kIdentifier,
        Type::kKeywordOr,     Type::kIdentifier, Type::kEndOfInput};
    EXPECT_EQ(Types(result), expected);
}

// Escenario 5: espacios, tabulaciones y saltos de linea.
TEST(LexerScenarios, SpacesTabsAndNewlinesAreSkippedAndLinesAreCounted) {
    const LexResult result = Lex("SELECT\ta,\n\tb\r\nFROM    t\n\n;");
    EXPECT_TRUE(result.Ok());
    EXPECT_EQ(Lexemes(result),
              (std::vector<std::string>{"SELECT", "a", ",", "b", "FROM", "t", ";", ""}));
    EXPECT_EQ(result.tokens[4].line, 3U);   // FROM
    EXPECT_EQ(result.tokens[6].line, 5U);   // ;
}

// Escenario 6: caracteres no admitidos y cadenas sin cerrar.
TEST(LexerScenarios, InvalidCharactersAndUnterminatedStringsAreReported) {
    const LexResult result = Lex("SELECT a # FROM t WHERE n = 'Ana");

    ASSERT_EQ(result.errors.size(), 2U);
    EXPECT_NE(result.errors[0].message.find('#'), std::string::npos);
    EXPECT_EQ(result.errors[0].column, 10U);
    EXPECT_EQ(result.errors[1].message, "Cadena sin cerrar");
    EXPECT_EQ(result.errors[1].column, 29U);
    for (const Token& token : result.tokens) {
        EXPECT_NE(token.lexeme, "#");
        EXPECT_NE(token.type, Type::kStringLiteral);
    }
}

// Escenario 7: entradas vacias, sentencias incompletas y tokens al final.
TEST(LexerScenarios, EmptyIncompleteAndTrailingTokens) {
    // Vacia: solo EOF.
    EXPECT_EQ(Types(Lex("")), (std::vector<TokenType>{Type::kEndOfInput}));

    // Incompleta: el lexer no valida la gramatica, solo clasifica lo que hay.
    const LexResult incomplete = Lex("SELECT nombre FROM");
    EXPECT_TRUE(incomplete.Ok());
    EXPECT_EQ(Types(incomplete), (std::vector<TokenType>{Type::kKeywordSelect, Type::kIdentifier,
                                                         Type::kKeywordFrom, Type::kEndOfInput}));

    // Un token pegado al final de la entrada, sin espacio ni punto y coma.
    EXPECT_EQ(Lex("edad >= 18").tokens[2].lexeme, "18");
    EXPECT_EQ(Lex("tabla").tokens[0].lexeme, "tabla");
    EXPECT_EQ(Lex("a >").tokens[1].type, Type::kGreater);
}

}  // namespace
}  // namespace minidb
