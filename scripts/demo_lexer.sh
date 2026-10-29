#!/usr/bin/env bash
# Demostracion del analizador lexico: ejecuta los siete escenarios de validacion.
# Uso: ./scripts/demo_lexer.sh [directorio_de_build]   (por defecto: build)
set -u
BUILD_DIR="${1:-build}"
LEXER="$BUILD_DIR/sql_lexer"

if [ ! -x "$LEXER" ]; then
    echo "No se encontro $LEXER. Compila primero:" >&2
    echo "  cmake -S . -B $BUILD_DIR && cmake --build $BUILD_DIR -j" >&2
    exit 2
fi

caso() {  # caso "titulo" "sentencia"
    echo
    echo "=================================================================="
    echo "$1"
    echo "Entrada: $2"
    echo "------------------------------------------------------------------"
    "$LEXER" -e "$2"
}

caso "1. Sentencia valida (SELECT, FROM, WHERE)" \
     $'SELECT nombre, edad\nFROM estudiantes\nWHERE edad >= 18;'
caso "2. Operadores compuestos como tokens individuales" \
     "a >= 1 AND b <= 2 OR c <> 3 AND d != 4"
caso "3. Literales: enteros, decimales y cadenas" \
     "SELECT 42, 3.14, 'Ana Quispe' FROM t;"
caso "4. Palabras reservadas contra identificadores" \
     "select selection from fromage where wherever"
caso "5. Espacios, tabulaciones y saltos de linea" \
     $'SELECT\ta,\n\tb\r\nFROM    t\n\n;'
caso "6a. Error lexico: caracter no admitido" \
     "SELECT a # FROM t;"
caso "6b. Error lexico: cadena sin cerrar" \
     "SELECT * FROM t WHERE nombre = 'Ana"
caso "6c. Error lexico: numero mal formado" \
     "SELECT 12abc, 3. FROM t;"
caso "7a. Caso limite: entrada vacia" ""
caso "7b. Caso limite: sentencia incompleta" "SELECT nombre FROM"
caso "7c. Caso limite: token pegado al final de la entrada" "edad>=18"
echo
