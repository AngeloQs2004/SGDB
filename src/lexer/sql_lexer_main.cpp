// Programa de linea de comandos del analizador lexico.
//
//   sql_lexer archivo.sql        lee la sentencia de un archivo
//   sql_lexer -e "SELECT ..."    analiza el texto dado
//   sql_lexer                    lee de la entrada estandar (tambien con "-")
//
// Imprime una fila por token y, si hay, la lista de errores con su posicion.
// Codigo de salida: 0 sin errores lexicos, 1 con errores, 2 uso o lectura.
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>

#include "minidb/lexer/lexer.hpp"

namespace {

void PrintUsage() {
    std::cerr << "Uso: sql_lexer [archivo | -e \"sentencia\" | -]\n"
                 "Sin argumentos o con '-' lee la sentencia de la entrada estandar.\n";
}

bool ReadAll(std::istream& in, std::string& out) {
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !in.bad();
}

void PrintTokens(const minidb::LexResult& result) {
    std::printf("%6s %4s  %-16s %s\n", "LINEA", "COL", "TIPO", "LEXEMA");
    std::printf("-------------------------------------------------\n");
    for (const minidb::Token& token : result.tokens) {
        const std::string type(minidb::ToString(token.type));
        const std::string lexeme =
            token.type == minidb::TokenType::kEndOfInput ? "<EOF>" : token.lexeme;
        std::printf("%6zu %4zu  %-16s %s\n", token.line, token.column, type.c_str(),
                    lexeme.c_str());
    }
    std::printf("\nTokens: %zu (incluye EOF)\n", result.tokens.size());
}

void PrintErrors(const minidb::LexResult& result) {
    std::printf("\nErrores lexicos (%zu):\n", result.errors.size());
    for (const minidb::LexicalError& error : result.errors) {
        std::printf("  [%zu:%zu] %s\n", error.line, error.column, error.message.c_str());
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string input;

    if (argc == 1 || (argc == 2 && std::string(argv[1]) == "-")) {
        if (!ReadAll(std::cin, input)) {
            std::cerr << "Error al leer la entrada estandar\n";
            return 2;
        }
    } else if (argc == 3 && std::string(argv[1]) == "-e") {
        input = argv[2];
    } else if (argc == 2 && std::string(argv[1]) != "-e") {
        std::ifstream file(argv[1], std::ios::binary);
        if (!file) {
            std::cerr << "No se pudo abrir el archivo: " << argv[1] << '\n';
            return 2;
        }
        if (!ReadAll(file, input)) {
            std::cerr << "Error al leer el archivo: " << argv[1] << '\n';
            return 2;
        }
    } else {
        PrintUsage();
        return 2;
    }

    const minidb::LexResult result = minidb::Lexer(input).Tokenize();
    PrintTokens(result);
    if (!result.Ok()) {
        PrintErrors(result);
        return 1;
    }
    return 0;
}
