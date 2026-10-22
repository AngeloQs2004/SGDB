# Analizador léxico SQL

Primera etapa de la Fase 2: convierte el texto de una sentencia `SELECT` en una
secuencia ordenada de tokens. No ejecuta la consulta ni comprueba que las tablas
o columnas existan; eso corresponde a las etapas siguientes.

El módulo es independiente del motor de almacenamiento: vive en `src/lexer/` y
`include/minidb/lexer/`, se compila como la biblioteca `minidb_lexer` y no
depende de `minidb_core`.

## Uso

```cpp
#include "minidb/lexer/lexer.hpp"

minidb::LexResult result = minidb::Lexer("SELECT nombre FROM alumnos;").Tokenize();
for (const minidb::Token& token : result.tokens) { /* tipo, lexema, línea, columna */ }
for (const minidb::LexicalError& error : result.errors) { /* mensaje, línea, columna */ }
```

`Tokenize()` siempre devuelve la lista de tokens terminada en `EOF` y la lista de
errores encontrados. Un lexema inválido nunca aparece como token.

## Modelo de token

| Campo | Contenido |
|---|---|
| `type` | Categoría léxica (`TokenType`) |
| `lexeme` | Texto tal como aparece en la entrada |
| `line` | Línea donde empieza, desde 1 |
| `column` | Columna donde empieza, desde 1 |

## Categorías implementadas

| Lexema | Tipo de token |
|---|---|
| `SELECT`, `FROM`, `WHERE`, `AND`, `OR` | `KEYWORD_SELECT`, `KEYWORD_FROM`, `KEYWORD_WHERE`, `KEYWORD_AND`, `KEYWORD_OR` |
| nombres de tablas y columnas | `IDENTIFIER` |
| `18`, `2026` | `INTEGER_LITERAL` |
| `,` | `COMMA` |
| `;` | `SEMICOLON` |
| fin de la entrada | `EOF` |

Los nombres de los tokens son referenciales; `ToString(TokenType)` devuelve los
de esta tabla.

## Reglas léxicas

- **Mayúsculas y minúsculas.** Las palabras reservadas no distinguen mayúsculas:
  `select`, `Select` y `SELECT` son el mismo token. El lexema conserva lo que
  escribió el usuario.
- **Identificadores.** Empiezan con una letra ASCII o `_` y continúan con
  letras, dígitos o `_`.
- **Palabras reservadas contra identificadores.** Primero se lee la palabra
  completa y después se consulta la tabla de reservadas. Por eso `selected`,
  `from_tabla` o `or_x` son identificadores aunque empiecen con una palabra
  reservada.
- **Espacios en blanco.** Espacio, tabulación, `\r` y `\n` separan tokens y se
  ignoran.
- **Posiciones.** Línea y columna empiezan en 1. Un salto de línea incrementa la
  línea y reinicia la columna. La columna cuenta caracteres, no bytes: un
  carácter UTF-8 de varios bytes ocupa una sola columna.
- **Fin de entrada.** El último token siempre es `EOF`, con la posición del final
  del texto. Una entrada vacía produce solo `EOF`.

## Errores léxicos

Un carácter que no pertenece a ningún token se informa como
`Caracter no admitido: 'x'` con su línea y columna. El análisis continúa, de modo
que todos los errores de la entrada se reportan en una sola pasada.

## Pendiente

Decimales, literales de texto entre comillas simples, operadores relacionales
(`=`, `<>`, `!=`, `<`, `>`, `<=`, `>=`), `*`, errores por cadenas sin cerrar y
números mal formados, y el programa de línea de comandos.
