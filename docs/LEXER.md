# Analizador léxico SQL

Primera etapa de la Fase 2: convierte el texto de una sentencia `SELECT` en una
secuencia ordenada de tokens. No ejecuta la consulta ni comprueba que las tablas
o columnas existan; eso corresponde a las etapas siguientes.

El módulo es independiente del motor de almacenamiento: vive en `src/lexer/` y
`include/minidb/lexer/`, se compila como la biblioteca `minidb_lexer` y no
depende de `minidb_core`.

## Estructura del módulo

| Archivo | Responsabilidad |
|---|---|
| `include/minidb/lexer/token.hpp`, `src/lexer/token.cpp` | Modelo de tokens: `TokenType`, `Token`, `ToString` |
| `include/minidb/lexer/keywords.hpp`, `src/lexer/keywords.cpp` | Tabla de palabras reservadas |
| `include/minidb/lexer/lexer.hpp`, `src/lexer/lexer.cpp` | Reconocedor léxico y manejo de errores |
| `src/lexer/sql_lexer_main.cpp` | Programa de línea de comandos `sql_lexer` (interfaz de salida) |
| `tests/lexer_test.cpp` | Pruebas unitarias |

## Uso desde C++

```cpp
#include "minidb/lexer/lexer.hpp"

minidb::LexResult result = minidb::Lexer("SELECT nombre FROM alumnos;").Tokenize();
for (const minidb::Token& token : result.tokens) { /* tipo, lexema, línea, columna */ }
for (const minidb::LexicalError& error : result.errors) { /* mensaje, línea, columna */ }
```

`Tokenize()` siempre devuelve la lista de tokens terminada en `EOF` y la lista de
errores encontrados. Un lexema inválido nunca aparece como token. La lista de
tokens es la entrada del analizador sintáctico.

## Uso desde la línea de comandos

```bash
./build/sql_lexer archivo.sql
./build/sql_lexer -e "SELECT * FROM t WHERE a <> 3;"
echo "SELECT a FROM t" | ./build/sql_lexer
```

Imprime una fila por token (línea, columna, tipo, lexema) y, si los hay, la lista
de errores. Código de salida: `0` sin errores léxicos, `1` con errores léxicos,
`2` por uso incorrecto o fallo de lectura.

## Modelo de token

| Campo | Contenido |
|---|---|
| `type` | Categoría léxica (`TokenType`) |
| `lexeme` | Texto tal como aparece en la entrada (las cadenas conservan sus comillas) |
| `line` | Línea donde empieza, desde 1 |
| `column` | Columna donde empieza, desde 1 |

## Categorías de tokens

| Lexema | Tipo de token |
|---|---|
| `SELECT`, `FROM`, `WHERE`, `AND`, `OR` | `KEYWORD_SELECT`, `KEYWORD_FROM`, `KEYWORD_WHERE`, `KEYWORD_AND`, `KEYWORD_OR` |
| nombres de tablas y columnas | `IDENTIFIER` |
| `18`, `2026` | `INTEGER_LITERAL` |
| `3.14`, `0.5` | `DECIMAL_LITERAL` |
| `'Ana'`, `'it''s'` | `STRING_LITERAL` |
| `=` | `EQUAL` |
| `<>`, `!=` | `NOT_EQUAL` |
| `<`, `>` | `LESS`, `GREATER` |
| `<=`, `>=` | `LESS_EQUAL`, `GREATER_EQUAL` |
| `,` | `COMMA` |
| `;` | `SEMICOLON` |
| `*` | `ASTERISK` |
| fin de la entrada | `EOF` |

Los nombres de los tokens son referenciales; `ToString(TokenType)` devuelve los
de esta tabla. `<>` y `!=` son el mismo token (`NOT_EQUAL`) y el lexema conserva
cuál de los dos se escribió. `*` se incluye para admitir `SELECT * FROM t`.

## Reglas léxicas

- **Mayúsculas y minúsculas.** Las palabras reservadas no distinguen mayúsculas:
  `select`, `Select` y `SELECT` son el mismo token. El lexema conserva lo que
  escribió el usuario. Los identificadores conservan su capitalización original.
- **Identificadores.** Empiezan con una letra ASCII o `_` y continúan con
  letras, dígitos o `_`.
- **Palabras reservadas contra identificadores.** Primero se lee la palabra
  completa y después se consulta la tabla de reservadas. Por eso `selected`,
  `from_tabla` o `or_x` son identificadores aunque empiecen con una palabra
  reservada.
- **Números.** Un entero son dígitos; un decimal son dígitos, un punto y dígitos
  (`3.14`). No hay signo, notación científica ni punto inicial: `-5` y `.5` no son
  números del lexer. El signo menos lo resolverá el analizador sintáctico.
- **Cadenas.** Van entre comillas simples. Dentro de ellas, `''` representa una
  comilla. Pueden contener cualquier carácter, incluidos espacios, palabras
  reservadas, operadores y saltos de línea. El lexema conserva las comillas.
- **Operadores.** Se toma siempre el lexema más largo posible (*maximal munch*):
  en `a>=1`, el `>=` es un solo token. `> =` con un espacio en medio son dos
  tokens.
- **Espacios en blanco.** Espacio, tabulación, `\r` y `\n` separan tokens y se
  ignoran.
- **Posiciones.** Línea y columna empiezan en 1. Un salto de línea incrementa la
  línea y reinicia la columna. La columna cuenta caracteres, no bytes: un
  carácter UTF-8 de varios bytes ocupa una sola columna. La posición de un token
  es la de su primer carácter; la de una cadena sin cerrar, la de su comilla de
  apertura.
- **Fin de entrada.** El último token siempre es `EOF`, con la posición del final
  del texto. Una entrada vacía produce solo `EOF`.

## Errores léxicos

El análisis no se detiene en el primer error: se registran todos, en el orden en
que aparecen, y lo válido que los rodea se reconoce con normalidad. Un lexema
inválido nunca se clasifica como token.

| Situación | Mensaje | Posición |
|---|---|---|
| Carácter que no pertenece a ningún token (`@`, `#`, `$`, `-`, `.`, ...) | `Caracter no admitido: 'x'` | el carácter |
| `!` que no forma `!=` | `Caracter no admitido: '!' (solo es valido como '!=')` | el `!` |
| Cadena sin comilla de cierre | `Cadena sin cerrar` | la comilla de apertura |
| Número seguido de letras, de un punto sin dígitos o con más de un punto (`12abc`, `3.`, `1.2.3`) | `Numero mal formado: '12abc'` | el primer dígito |

Un número mal formado se informa como un único error que cubre toda la secuencia,
para no dejar fragmentos que parezcan tokens válidos.

## Qué no hace

- No comprueba la gramática: `SELECT FROM` produce los tokens `KEYWORD_SELECT`,
  `KEYWORD_FROM` y `EOF` sin error. Eso es trabajo del analizador sintáctico.
- No comprueba que las tablas ni las columnas existan.
- No interpreta los literales: no quita las comillas de las cadenas ni convierte
  los números.

## Pruebas

```bash
./build/lexer_test                                   # las 49 pruebas del módulo
./build/lexer_test --gtest_filter='LexerScenarios.*' # los siete escenarios del enunciado
```
