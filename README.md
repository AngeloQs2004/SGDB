# MiniDB

Mini sistema gestor de base de datos persistente escrito en C++20. Almacena los
datos en un único archivo binario organizado en páginas de tamaño fijo, los
administra en RAM mediante un Buffer Pool con reemplazo LRU, indexa la clave
primaria con un hash persistente mapeado sobre las mismas páginas, y procesa
consultas SQL con un parser propio y operadores físicos que siguen el modelo
Volcano (`Open`, `Next`, `Close`).

No pretende competir con PostgreSQL, MySQL ni SQLite: es un sistema pequeño,
académico y demostrable. Todo lo que hay aquí está implementado desde cero sobre
la biblioteca estándar de C++; la única dependencia externa es GoogleTest, y
solo para las pruebas.

## Índice

- [Características](#características)
- [Requisitos](#requisitos)
- [Compilación](#compilación)
- [Ejecución](#ejecución)
- [Pruebas](#pruebas)
- [SQL soportado](#sql-soportado)
- [Arquitectura](#arquitectura)
- [Diseño de páginas](#diseño-de-páginas)
- [Buffer Pool](#buffer-pool)
- [Índice hash](#índice-hash)
- [Modelo Volcano](#modelo-volcano)
- [Búsqueda por similitud vectorial](#búsqueda-por-similitud-vectorial)
- [Ejecución vectorizada](#ejecución-vectorizada)
- [Evaluación de rendimiento](#evaluación-de-rendimiento)
- [Archivos en disco](#archivos-en-disco)
- [Limitaciones](#limitaciones)

El detalle técnico completo —offsets, invariantes y diagramas— está en
[ARCHITECTURE.md](ARCHITECTURE.md).

## Características

- **Persistencia real**: los datos viven en `data/minidb.db`, en páginas de 4096
  bytes. Al cerrar y reabrir el programa siguen ahí.
- **Gestor de páginas**: `DiskManager` lee y escribe páginas completas, asigna y
  libera identificadores, y mantiene una lista de páginas libres para reutilizar
  el espacio.
- **Slotted pages**: los registros de longitud variable se organizan con un
  directorio de slots, con compactación y reutilización del espacio borrado.
- **Buffer Pool**: número fijo de frames en RAM, page table para búsqueda O(1),
  contador de fijaciones, marca de página sucia y reemplazo LRU.
- **Índice hash persistente**: la clave primaria se indexa en páginas físicas
  que se leen y escriben a través del Buffer Pool, con resolución de colisiones
  por encadenamiento y páginas de overflow.
- **Procesamiento de consultas**: tokenizador y parser descendente recursivo
  propios, y seis operadores físicos componibles, incluidos `ORDER BY` y
  `GROUP BY` como operadores bloqueantes.
- **Búsqueda por similitud vectorial**: un tipo `VECTOR(d)` persistido en las mismas
  páginas ranuradas que los demás registros, tres métricas de distancia y una consulta
  de los `k` vecinos más cercanos con selección Top-k acotada.
- **Ejecución vectorizada**: además del camino Volcano tupla a tupla, un camino
  por lotes en el que `NextBatch` mueve ~1024 registros de una vez, el escaneo
  fija cada página una sola vez y el filtro compara sobre una columna contigua
  con instrucciones SIMD. Los dos conviven y se pueden comparar.
- **Medición del coste**: cada sentencia informa de su plan con contadores por
  operador, del tiempo empleado y de las páginas leídas del disco.
- **Evaluación con y sin índice**: `.bench` resuelve el mismo lote de búsquedas
  por los dos caminos de acceso y compara tiempo, páginas y registros
  examinados.

## Requisitos

| Herramienta | Versión mínima | Notas |
|---|---|---|
| Compilador C++20 | GCC 12 o Clang 15 | Desarrollado con GCC 16 |
| CMake | 3.20 | |
| Ninja | cualquiera | Opcional, pero es el generador usado aquí |
| GoogleTest | 1.10 | Solo para las pruebas |

En Arch Linux y CachyOS:

```bash
sudo pacman -S --needed base-devel cmake ninja gtest
```

En Debian o Ubuntu:

```bash
sudo apt install build-essential cmake ninja-build libgtest-dev
```

## Compilación

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build -j"$(nproc)"
```

Compilación con sanitizadores de direcciones y de comportamiento indefinido.
Merece la pena porque toda la capa de almacenamiento manipula offsets crudos
dentro de búferes de 4096 bytes:

```bash
cmake -S . -B build-sanitize -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMINIDB_ENABLE_SANITIZERS=ON
cmake --build build-sanitize -j"$(nproc)"
ctest --test-dir build-sanitize --output-on-failure
```

## Ejecución

Sesión interactiva:

```bash
./build/minidb data/minidb.db
```

```
MiniDB iniciado correctamente
Archivo: /home/usuario/mini-dbms/data/minidb.db
Tamaño de página: 4096 bytes
Frames del Buffer Pool: 8
Base de datos vacía: use CREATE TABLE para empezar
Escriba .help para ver la ayuda

minidb> SELECT * FROM students WHERE age >= 20;
Plan físico (modelo Volcano): ProjectionOperator <- FilterOperator <- SequentialScanOperator
+----+------+-----+---------------------------+
| id | name | age | career                    |
+----+------+-----+---------------------------+
| 1  | Ana  | 21  | Ciencia de la Computación |
| 2  | Luis | 22  | Ingeniería de Sistemas    |
+----+------+-----+---------------------------+
2 filas devueltas
```

Ejecución de un guion:

```bash
./build/minidb data/minidb.db < examples/demo.sql
```

Demostración completa, que además **cierra y reabre** la base para probar la
persistencia y comprueba que el archivo no crece tras un ciclo de borrado y
reinserción:

```bash
bash scripts/demo.sh
```

Evidencia de los archivos en disco (tamaños, `stat`, `file` y volcado
hexadecimal de las cabeceras):

```bash
bash scripts/inspect_database.sh
```

### Comandos internos

| Comando | Qué muestra |
|---|---|
| `.help` | SQL admitido, comandos y ejemplos |
| `.clear` | Limpia la pantalla y devuelve el cursor al inicio |
| `.schema` | Esquema de la tabla y número de registros |
| `.pages` | Páginas del archivo, por tipo, y la lista de libres |
| `.buffer` | Estado de cada frame y estadísticas del Buffer Pool |
| `.files` | Rutas y tamaños de los archivos en disco |
| `.flush` | Sincroniza todas las páginas sucias |
| `.indice [on\|off]` | Activa o desactiva el uso del índice, para medir su efecto |
| `.vectorizado [on\|off]` | Cambia entre ejecución tupla a tupla y por lotes |
| `.topk [on\|off]` | Top-k acotado u orden completo en las consultas `NEAREST` |
| `.bench [n]` | Compara `n` búsquedas por clave con y sin índice |
| `.knnbench [k] [n]` | Compara las dos estrategias de ranking vectorial |
| `.knncsv <ruta> [k] [n]` | Igual, exportando una fila CSV por consulta |
| `.benchvec [n]` | Compara los dos modelos de ejecución sobre la misma consulta |
| `.exit` | Sincroniza y sale |

Evaluación de rendimiento con y sin índice, sobre su propia base de datos:

```bash
bash scripts/benchmark.sh
```

## Pruebas

```bash
ctest --test-dir build --output-on-failure
```

Más de 310 casos repartidos en trece binarios, uno por componente. Cada prueba
usa su propio archivo temporal y lo borra al terminar, así que no comparten
estado y pueden ejecutarse en cualquier orden.

| Binario | Cubre |
|---|---|
| `serialization_test` | Orden de bytes, límites, cadenas UTF-8 |
| `disk_manager_test` | Persistencia, validación de cabecera, lista de libres |
| `table_page_test` | Slotted page, compactación, capacidad exacta, registros |
| `buffer_pool_test` | LRU, fijaciones, escritura de páginas sucias, `PageGuard` |
| `table_heap_test` | Cadena de páginas, reubicación, no crecimiento del archivo |
| `hash_index_test` | Colisiones, overflow, inserción masiva, persistencia |
| `parser_test` | Gramática, errores con posición, mayúsculas, acentos |
| `volcano_test` | `Open`/`Next`/`Close`, elección de plan, streaming |
| `blocking_operators_test` | `ORDER BY` y `GROUP BY`: orden, estabilidad, grupos, fijaciones |
| `evaluation_test` | Contadores por operador, tiempo, páginas leídas, con y sin índice |
| `vectorized_test` | Lotes, vector de selección, equivalencia de los dos modelos |
| `distance_test` | Métricas de distancia contra valores calculados a mano |
| `knn_test` | Tipo `VECTOR`, gramática `NEAREST`, casos límite y equivalencia del ranking |
| `integration_test` | Ciclo completo, consistencia tabla-índice, configuración |

## SQL soportado

```sql
CREATE TABLE <tabla> (<col> INT [PRIMARY KEY] | <col> VARCHAR(<n>)
                      | <col> VECTOR(<d>), ...);
INSERT INTO <tabla> VALUES (<v1>, <v2>, ...);   -- un vector: [0.1, 0.9, ...]
SELECT * | <col> | COUNT(*), ... FROM <tabla> [WHERE <col> <op> <valor>]
    [GROUP BY <col>] [ORDER BY <col> | COUNT(*) [ASC | DESC]];
SELECT ... FROM <tabla> [WHERE <col> <op> <valor>]
    NEAREST <col_vector> TO [<v1>, ...] [USING EUCLIDEAN|COSINE|DOT] LIMIT <k>;
UPDATE <tabla> SET <col> = <valor> [, ...] [WHERE <col> <op> <valor>];
DELETE FROM <tabla> [WHERE <col> <op> <valor>];
```

- Operadores de comparación: `=`, `!=` (o `<>`), `<`, `<=`, `>`, `>=`.
- `ORDER BY` admite una columna, con `ASC` (por defecto) o `DESC`. Se resuelve
  contra las columnas que produce el operador de debajo, así que puede ordenar
  por una columna que no se proyecta.
- `GROUP BY` admite una columna, y `COUNT(*)` es la única función de agregado.
  `SELECT COUNT(*) FROM t` sin `GROUP BY` cuenta la tabla entera. Con `GROUP BY`
  hay que indicar las columnas: `SELECT *` se rechaza porque el resultado ya no
  son las columnas de la tabla.
- El ordenamiento es estable: las filas que empatan conservan el orden en que
  las produjo el escaneo, que es el de inserción.
- Las palabras clave no distinguen mayúsculas de minúsculas; los identificadores
  conservan su forma pero se comparan sin distinguirlas.
- El `;` final es opcional en modo interactivo y separa sentencias en un guion.
- Tipos: `INT` (`std::int32_t`) y `VARCHAR(n)`, con `n` entre 1 y 255 **bytes**
  UTF-8. Esto importa con datos en español: `'Ciencia de la Computación'` son 25
  caracteres pero 26 bytes.
- Las cadenas van entre comillas simples; `''` dentro de una cadena es una
  comilla literal.

## Arquitectura

```
CLI (main.cpp)
      |
   Database  -- fachada: abre el archivo y ensambla el resto
      |
  Parser + ExecutionEngine
      |
  Operadores físicos (Volcano)
      |
  TableHeap  +  HashIndex
      |
  BufferPoolManager  (frames, LRU, páginas sucias)
      |
  DiskManager        (páginas de 4096 bytes, lista de libres)
      |
  data/minidb.db
```

Las reglas de dependencia que sostienen el diseño:

- `TableHeap`, `HashIndex` y `Catalog` solo conocen el `BufferPoolManager`.
  Ninguno tiene acceso al `DiskManager` ni a un archivo.
- La página 0 pertenece en exclusiva al `DiskManager`: guarda los metadatos del
  asignador, y mantenerla privada evita una dependencia circular entre el
  asignador y el Buffer Pool.
- El parser no incluye nada de `storage/`, `buffer/` ni `index/`: convierte
  texto en un `Statement` y nada más.
- Los operadores no conocen el Buffer Pool ni el disco.
- `main.cpp` solo habla con `Database`.

## Diseño de páginas

Todas las páginas miden 4096 bytes y su offset es `page_id * 4096`. Una página
de tabla se organiza así:

```
0        12                 free_space_begin      free_space_end      4096
|--------|------------------|---------------------|-------------------|
 cabecera   directorio slots      espacio libre      datos de registros
            (crece ->)                                  (<- crece)
```

La cabecera ocupa 12 bytes y cada slot 4. `free_space_begin` no se almacena: es
siempre `12 + 4 * slot_count`, y un campo redundante es un invariante más que
puede desincronizarse.

Un slot cuyo offset vale 0 está libre. El offset 0 es imposible para datos
reales porque la cabecera ocupa los bytes 0..11, así que sirve de centinela y
ahorra un byte de estado por slot.

Con el esquema de demostración, un registro ocupa como máximo 112 bytes, de modo
que en una página caben exactamente 35 registros en el peor caso
(`12 + 35*4 + 35*112 = 4072`) y no 36 (`4188 > 4096`).

**Reutilización del espacio.** Al borrar, el slot queda disponible para la
siguiente inserción. Al actualizar un registro que crece, se intenta primero en
el sitio, después en el hueco libre, después compactando, y solo si nada de eso
basta se reubica en otra página. `Compact()` conserva los identificadores de
slot, lo cual es imprescindible: el índice apunta a pares
`(página, slot)`, y renumerarlos dejaría todas sus entradas colgando.

## Buffer Pool

Ocho frames por defecto, configurables en `minidb.conf`. La operación más
costosa —un `UPDATE` que reubica un registro— fija seis páginas a la vez, así
que ocho dejan margen.

- Una `page table` (`unordered_map`) resuelve la búsqueda en O(1).
- `pin_count` cuenta los usuarios activos de una página. Una página fijada nunca
  se reemplaza: sencillamente no está en la estructura del reemplazador.
- La marca de página sucia solo se borra **después** de que la escritura a disco
  haya tenido éxito, y se acumula entre desfijaciones, para que un lector que
  desfija limpiamente no borre el hecho de que otro la modificó.
- `PageGuard` desfija en su destructor. Una fijación perdida retira un frame de
  un conjunto muy pequeño, y unas sentencias después el sistema se queda sin
  víctimas; una excepción a mitad de operación lo provocaría en silencio.

## Índice hash

La clave primaria se mapea a la posición del registro. La clase **no guarda
ninguna entrada en RAM**: sus únicos miembros son una referencia al Buffer Pool
y el identificador de su página de cabecera, así que cada búsqueda, inserción y
borrado lee y escribe páginas reales.

- 16 buckets, con sus páginas asignadas por adelantado.
- Cada página de bucket almacena hasta 408 entradas de 10 bytes.
- Las colisiones se resuelven por encadenamiento: cuando una página se llena, se
  enlaza detrás una página de overflow.
- El borrado mueve la última entrada al hueco en lugar de dejar una lápida, así
  que las páginas no se degradan con agujeros. Una página de overflow que se
  vacía se desenlaza y vuelve a la lista de páginas libres del archivo.
- La función hash es propia (multiplicativa de Knuth), no `std::hash`: esta
  última está definida por la implementación —es la identidad en libstdc++—, de
  modo que una base creada con una biblioteca estándar no podría leerse con
  otra.

## Modelo Volcano

Cada operador es un iterador: `Open` lo prepara, `Next` produce un registro por
llamada y devuelve `std::nullopt` al agotarse, y `Close` libera lo que `Open`
tomó. Los operadores se componen sujetando un hijo y tirando de él, de modo que
un plan es un árbol que va pasando registros hacia arriba sin materializar
nunca el resultado completo.

```
SELECT * FROM students;              SELECT * FROM students WHERE id = 1;

  ProjectionOperator                   ProjectionOperator
          |                                    |
  SequentialScanOperator               IndexScanOperator


SELECT * FROM students WHERE age >= 20;   SELECT * FROM students ORDER BY age DESC;

  ProjectionOperator                        ProjectionOperator
          |                                         |
  FilterOperator                            SortOperator
          |                                         |
  SequentialScanOperator                    SequentialScanOperator


SELECT career, COUNT(*) FROM students GROUP BY career ORDER BY COUNT(*) DESC;

  ProjectionOperator
          |
  SortOperator             <- ordena la salida del agregado
          |
  AggregateOperator        <- una fila por grupo
          |
  SequentialScanOperator
```

**Operadores en flujo y operadores bloqueantes.** `Filter` y `Projection`
responden a un `Next` con como mucho una llamada a su hijo, así que un plan hecho
solo de ellos usa memoria proporcional a un registro. `Sort` y `Aggregate` no
pueden: ni la primera fila ordenada ni un conteo se conocen antes de haber leído
el último registro de la entrada, de modo que ambos vacían a su hijo dentro de
`Open`. Por eso viven en su propio archivo, `src/execution/blocking_operators.cpp`.

`Sort` se coloca **debajo** de la proyección cuando no hay `GROUP BY`, para que
`ORDER BY` pueda nombrar una columna que no se proyecta, y **encima** del
agregado cuando lo hay, para poder ordenar por el grupo o por `COUNT(*)`. Es una
sola regla: *`ORDER BY` se resuelve contra las columnas que produce su hijo*.

El planificador elige `IndexScanOperator` **solo** cuando la condición es una
igualdad sobre la clave primaria, que es la única pregunta que un índice hash
sabe responder. `WHERE id > 1` recae en escaneo más filtro aunque `id` sea la
clave.

`UPDATE` y `DELETE` no son operadores: el modelo Volcano describe iteradores que
producen tuplas, y una sentencia cuya única salida es un contador no encaja en
esa forma. Sí construyen un plan real para localizar sus filas, en dos fases:
primero recorren el plan y recogen las posiciones, lo cierran, y solo entonces
modifican. Reescribir páginas mientras un escaneo sigue recorriéndolas
invalidaría los offsets de los que depende.

## Búsqueda por similitud vectorial

Un índice hash responde «qué registro tiene exactamente esta clave». Dispersa las
claves precisamente para destruir la vecindad, de modo que no puede responder «qué
registros se parecen más a este». Esa segunda pregunta es la que necesitan las
representaciones vectoriales densas, y es la que el tipo `VECTOR(d)` y la cláusula
`NEAREST` añaden al sistema.

```
minidb> CREATE TABLE docs (id INT PRIMARY KEY, titulo VARCHAR(20), emb VECTOR(2));
minidb> INSERT INTO docs VALUES (1, 'A', [1, 0]);
minidb> INSERT INTO docs VALUES (2, 'B', [0, 1]);
minidb> INSERT INTO docs VALUES (3, 'C', [1, 1]);

minidb> SELECT * FROM docs NEAREST emb TO [0.9, 0.1] LIMIT 3;
Plan físico (modelo Volcano):
  ProjectionOperator                    filas=3  next=4
  └─ KnnScanOperator                    filas=3  next=4  distancias=3
    └─ SequentialScanOperator           filas=3  next=4
+----+--------+--------+-----------+
| id | titulo | emb    | distancia |
+----+--------+--------+-----------+
| 1  | A      | [1, 0] | 0.141421  |
| 3  | C      | [1, 1] | 0.905538  |
| 2  | B      | [0, 1] | 1.27279   |
+----+--------+--------+-----------+
```

Las tres distancias son `√0,02`, `√0,82` y `√1,62`: el ejemplo se comprueba a mano.

**Almacenamiento.** Un vector es `2 + 4d` bytes —una dimensión de 16 bits seguida de
`d` valores IEEE 754 de 32 bits *little-endian*— dentro del mismo registro ranurado que
las demás columnas, leído por el mismo Buffer Pool. No hay un almacén vectorial
aparte, y por eso los accesos a vectores aparecen en los mismos contadores de aciertos
y fallos que todo lo demás.

Con `INT` y `VARCHAR` las cotas del sistema ya demostraban que ningún registro válido
podía exceder una página. Un vector rompe eso por sí solo, así que el constructor del
esquema comprueba ahora que el registro **más ancho posible** de la tabla quepa en una
página vacía. Sin esa comprobación, un `CREATE TABLE` podría aceptarse y luego producir
filas que no caben en ninguna página.

**Métricas.** Distancia euclidiana, distancia y similitud coseno —que son cantidades
distintas, y confundirlas invierte el ranking— y producto punto. Internamente todas se
reducen a una puntuación en la que *menor es siempre más cercano*: la euclidiana **al
cuadrado**, porque la raíz es monótona y solo hace falta en las `k` filas que se
devuelven, y el producto punto **negado**, porque es una similitud. Así un único
operador implementa las tres con una sola comparación.

El coseno del vector nulo no está definido; la implementación lo define como 0 y lo
documenta, en lugar de devolver un `NaN` que haría que el resultado dependiera del
orden de visita de los registros.

**Ranking.** `KnnScanOperator` mantiene un montículo de máximos acotado a `k`: un
candidato solo entra si mejora al peor de los supervivientes. Cuesta `O(n log k)` en
tiempo y `O(k)` en espacio. La alternativa ingenua, ordenar las `n` distancias, existe
también como `KnnFullSortOperator` y se activa con `.topk off`; cuesta `O(n log n)` y
`O(n)`. Las dos son **exactas** y devuelven exactamente las mismas filas, lo que las
pruebas comprueban fila a fila.

Los empates se resuelven por clave primaria, para que dos registros a la misma
distancia salgan siempre en el mismo orden.

**Búsqueda híbrida.** Un `WHERE` situado debajo del ranking reduce los candidatos antes
de que se calcule una sola distancia, por simple composición de operadores:

```
minidb> SELECT id FROM docs WHERE id <= 50 NEAREST emb TO [...] LIMIT 5;
  ProjectionOperator → KnnScanOperator → FilterOperator → SequentialScanOperator
```
Con 400 registros en la tabla y ese filtro, se calculan exactamente 50 distancias.

### Lo que NO es

**No hay índice vectorial.** La búsqueda es exhaustiva: examina todos los registros, y
su coste crece linealmente con la colección. La mejora del Top-k acotado es un factor
constante, no un cambio de orden de complejidad. Un índice IVF o HNSW sería lo que
cambiaría eso, y se propone como trabajo futuro.

Por la misma razón no hay búsqueda aproximada y no se reporta `Recall@k`: esa métrica
cuantifica lo que pierde una búsqueda aproximada, y aquí ambas estrategias son exactas.

### Evaluación

`.knnbench [k] [consultas]` compara las dos estrategias sobre las mismas consultas,
generadas con semilla fija dentro del binario. Con 100 000 vectores de dimensión 64 y
`k = 10`, el Top-k acotado promedia 63,0 ms por consulta frente a 101,8 ms del orden
completo: **1,61x**. La ventaja crece con el número de vectores —1,32x con mil— y decrece
con la dimensión —1,19x con dimensión 256—, porque el término aritmético `Θ(n·d)`, común
a las dos, pasa a dominar.

Las lecturas de disco son **idénticas** en ambas: 7139 páginas por consulta con 100 000
vectores. La mejora es exclusivamente de cómputo y de memoria, y no reduce el tráfico
con el disco.

El estudio completo, con las tablas, las figuras y el análisis, está en
[docs/articulo_busqueda_vectorizada.md](docs/articulo_busqueda_vectorizada.md); los
experimentos se reproducen con [experimentos/](experimentos/README.md).

## Ejecución vectorizada

El modelo Volcano mueve **una tupla** por llamada. Eso lo hace fácil de entender y
de componer, pero cada registro paga una llamada virtual por nivel del plan, y en
este sistema paga además una fijación de página en el Buffer Pool: el iterador del
*heap* fija la página, lee un slot, la suelta, y la vuelve a fijar para el
siguiente. En una página con 90 registros son 90 accesos donde bastaría uno.

La ejecución vectorizada —lo que hicieron MonetDB/X100 y Vectorwise— mueve un
**lote** de ~1024 registros por llamada. Está implementada como un segundo camino
que convive con el primero:

```
                          NextBatch()                 Next()
  tupla a tupla    SequentialScanOperator      FilterOperator
  por lotes        VectorizedScanOperator      VectorizedFilterOperator
```

Tres cosas cambian, y las tres se pueden medir:

1. **Una fijación de página por página, no por registro.** `VectorizedScanOperator`
   pide la página al Buffer Pool una vez y deserializa todos sus registros vivos.
2. **Un vector de selección en lugar de copiar.** `VectorizedFilterOperator` no
   mueve ningún registro: reescribe la lista de posiciones que sobrevivieron al
   predicado. Los registros se quedan donde el escaneo los dejó.
3. **La comparación se convierte en SIMD.** Los valores de la columna del filtro
   se copian a un array contiguo y se comparan en un bucle sin ramas ni llamadas
   indirectas, que el compilador traduce a instrucciones que comparan cuatro
   valores a la vez.

**El interfaz Volcano no cambia.** Los operadores por lotes siguen siendo
operadores físicos con `Open`, `Next` y `Close`. Lo que hay son dos adaptadores de
diez líneas cada uno:

- `PhysicalOperator::NextBatch` por defecto llama a `Next()` hasta llenar el lote,
  así que **cualquier** operador puede participar en un plan vectorizado sin
  reescribirse.
- `BatchOperator::Next` sirve un registro del lote que tiene en la mano, así que un
  operador vectorizado puede colocarse debajo de uno que no sabe nada de lotes.

Por eso `ORDER BY` y `GROUP BY`, que son tupla a tupla, funcionan tal cual encima
de un escaneo vectorizado.

### Cómo usarla y qué se gana

Está **desactivada por defecto**: los operadores tupla a tupla son la
implementación de referencia del modelo Volcano sobre el que está construido el
sistema, y tener los dos caminos disponibles es justo lo que permite medir uno
contra el otro. Se activa con `.vectorizado on`, o con `vectorized = true` en
`minidb.conf`.

```
minidb> SELECT * FROM students WHERE age >= 20;
Plan físico (modelo Volcano):
  ProjectionOperator                    filas=75  next=76
  └─ FilterOperator                     filas=75  next=76
    └─ SequentialScanOperator           filas=3000  next=3001

minidb> .vectorizado on
minidb> SELECT * FROM students WHERE age >= 20;
Plan físico (modelo Volcano):
  ProjectionOperator                    filas=75  next=76
  └─ VectorizedFilterOperator           filas=75  next=76  lotes=3
    └─ VectorizedScanOperator           filas=3000  next=0  lotes=3
```

`.benchvec [n]` compara los dos modelos sobre la misma consulta:

```
+-------------------------+-------------+-------------+------------+---------------+
| modelo                  | tiempo      | por consulta| lotes      | accesos al BP |
+-------------------------+-------------+-------------+------------+---------------+
| Volcano tupla a tupla   | 218.368 ms  | 10.9184 ms  | 0          | 60360         |
| Vectorizado por lotes   | 99.699 ms   | 4.9849 ms   | 60         | 360           |
+-------------------------+-------------+-------------+------------+---------------+

Llamadas a Next() en total: 60060 tupla a tupla frente a 40 vectorizado.
Accesos al Buffer Pool: 60360 frente a 360.
Aceleración: 2.19x.
```

3000 registros, 20 repeticiones, compilación Debug. Las dos cifras que no dependen
de la máquina son las importantes: **60060 llamadas a `Next()` se convierten en 40**
y **60360 accesos al Buffer Pool en 360**.

### Comprobar que realmente hay SIMD

El bucle de comparación solo se convierte en instrucciones vectoriales con
optimización, así que la compilación `Debug` **no** las tiene. Se puede comprobar:

```bash
g++ -std=c++20 -O3 -Iinclude -c src/execution/vectorized_operators.cpp -o /tmp/simd.o
objdump -d /tmp/simd.o | grep -cE 'pcmpgtd|pcmpeqd'      # 15
```

`scripts/benchmark.sh` hace esa comprobación y muestra el fragmento generado.
Tres detalles del código son los que lo hacen posible, y los tres son fáciles de
perder por accidente:

- El predicado es un **parámetro de plantilla**, no un `enum` en tiempo de
  ejecución, así que se inlinea y el cuerpo del bucle es una sola comparación.
- La máscara de resultados es `int32_t`, **no** `uint8_t` ni `bool`. Una escritura
  a través de un tipo tipo `char` puede aliasar cualquier objeto, y con la máscara
  de bytes el compilador no podía descartar que escribir un byte modificara el
  siguiente valor a leer: no vectorizaba nada. Cuatro bytes por bandera es un
  precio barato.
- Los punteros son `__restrict`, que afirma que los dos arrays no se solapan.

### Límites honestos

- La aceleración en tiempo es de **1.5x a 2.2x**, no de un orden de magnitud. El
  coste que domina un escaneo aquí es **deserializar** cada registro —cada
  `VARCHAR` construye un `std::string`— y eso lo paga igual el camino por lotes.
  Quitarlo exigiría almacenamiento columnar, que cambiaría el formato del archivo.
- La proyección y los operadores bloqueantes siguen siendo tupla a tupla. Se puede
  ver como el trabajo que queda, o como la demostración de que los dos modelos
  conviven: la parte vectorizada rinde igual con ellos encima.
- Un `VARCHAR` no se puede vectorizar: los valores son de longitud variable y
  están en offsets dispersos. El filtro lo detecta y compara registro a registro
  dentro del lote, que aun así ahorra la llamada virtual por registro.
- Una búsqueda por clave primaria **no** se vectoriza: no hay nada que agrupar en
  recuperar una sola fila, así que el planificador deja el camino del índice
  intacto.

## Evaluación de rendimiento

Cada sentencia informa de lo que costó:

```
minidb> SELECT * FROM students WHERE id = 1500;
Plan físico (modelo Volcano):
  ProjectionOperator                    filas=1  next=2
  └─ IndexScanOperator                  filas=1  next=2
...
1 fila devuelta.
Tiempo: 0.038 ms | páginas leídas del disco: 2 | buffer 2/2 (aciertos/fallos)
```

`filas` es lo que el operador entregó y `next` las veces que se tiró de él; la
diferencia entre lo que produce un escaneo y lo que deja pasar el filtro es el
trabajo que el índice evita.

`.bench [n]` resuelve el mismo lote de `n` búsquedas por clave primaria por los
dos caminos de acceso. Las claves se recogen antes de medir y las consultas son
idénticas en ambas rondas: lo único que cambia es si el planificador puede usar
el índice, que se controla con `.indice on|off`. No modifica la tabla.

```
$ bash scripts/benchmark.sh          # 3000 filas, 100 consultas por ronda

Comparación de rendimiento con y sin índice
Tabla: students   3000 registros   18 páginas de datos   100 consultas por clave primaria

+-----------------------------+-------------+-------------+------------+-------------+
| plan                        | tiempo      | por consulta| páginas    | registros   |
+-----------------------------+-------------+-------------+------------+-------------+
| IndexScan (con índice)      | 3.586 ms    | 0.0359 ms   | 119        | 100         |
| SeqScan+Filter (sin índice) | 1056.872 ms | 10.5687 ms  | 1800       | 300000      |
+-----------------------------+-------------+-------------+------------+-------------+

Registros examinados: 100 con índice frente a 300000 sin él.
Aceleración en tiempo: 294.8x.
Páginas leídas del disco: 15.1x menos con índice.
```

El volumen es configurable: `BENCH_ROWS=10000 BENCH_QUERIES=200 bash
scripts/benchmark.sh`.

Dos matices que el propio comando señala:

- La ventaja en **registros examinados** es estructural: el índice lee la fila
  que se le pidió, el escaneo lee todas. No depende de la máquina.
- La ventaja en **páginas leídas** depende del tamaño relativo del Buffer Pool.
  El índice tiene sus propias páginas —cabecera y buckets— compitiendo por los
  frames, así que en una tabla que casi cabe en el pool puede leer *más* páginas
  que un escaneo. Con tablas mayores que el pool la diferencia se dispara. El
  comando lo dice en lugar de presentar solo el caso favorable.
- Los tiempos son de una máquina concreta y de una compilación **Debug**; sirven
  para comparar las dos filas entre sí, no como cifra absoluta.

## Archivos en disco

| Archivo | Qué es |
|---|---|
| `data/minidb.db` | Base de datos completa: cabecera, catálogo, datos, índice y overflow |
| `minidb.conf` | Configuración en texto: archivo de datos, número de frames y modelo de ejecución |
| `data/bench.db` | Solo si se ejecuta `scripts/benchmark.sh`: base independiente para la evaluación |

El sistema no genera archivos temporales. `ORDER BY` y `GROUP BY` sí son
operadores bloqueantes, pero materializan en RAM, acotados por el tamaño de la
tabla. Una ordenación externa por mezcla, que volcaría los tramos ordenados a
disco, sería el paso siguiente y no está implementada.

Tras crear la tabla, el archivo ocupa exactamente 20 páginas (81 920 bytes): la
cabecera, el catálogo, la cabecera del índice, los 16 buckets y la primera
página de datos. Los cuatro primeros bytes son `MIND`, de modo que el archivo se
identifica a simple vista con `xxd`.

## Limitaciones

Conocidas y asumidas; están fuera del alcance del trabajo:

1. **Una sola tabla de usuario.** El catálogo guarda un esquema.
2. **Sin recuperación ante fallos.** No hay registro de escritura anticipada
   (WAL). Un cierre limpio, `.exit` y `.flush` sincronizan; una terminación
   abrupta puede perder las últimas sentencias no sincronizadas.
3. **Sin transacciones ni concurrencia.** Un proceso, un hilo.
4. **El índice solo acelera la igualdad sobre la clave primaria.** Los rangos y
   las columnas no indexadas usan escaneo secuencial.
5. **Número de buckets fijo.** No hay hash extensible ni redistribución; el
   crecimiento se absorbe con páginas de overflow.
6. **Sin `JOIN`, `GROUP BY`, `ORDER BY`, subconsultas, `AND`/`OR`, `ALTER TABLE`
   ni `NULL`.**
7. **No se permite actualizar la clave primaria**, para no tener que reescribir
   entradas de índice bajo una clave distinta.
8. **`VARCHAR` compara bytes**, no aplica reglas lingüísticas de ordenación.

## Fase 1 — Índice B+ Tree

Índice B+ Tree persistente (páginas de 4096 bytes sobre el `BufferPoolManager`) que mapea `clave INT -> RecordId(page_id, slot_id)`.
Documentación técnica, diagramas y guion de demo: [`docs/FASE1_BTREE.md`](docs/FASE1_BTREE.md), [`docs/PRESENTACION.md`](docs/PRESENTACION.md).

### Requisitos

- Compilador C++20 (probado con GCC 16 en Linux)
- CMake >= 3.20 (Ninja opcional)
- GoogleTest instalado en el sistema (el `CMakeLists.txt` usa `find_package(GTest REQUIRED)`; p. ej. `sudo apt install libgtest-dev` o `sudo pacman -S gtest`)

### Compilar y ejecutar

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release     # añade -G Ninja si lo tienes
cmake --build build -j"$(nproc)"
./build/btree_demo data/btree_fase1.db 5000 200 --trace   # demo: splits + Index Scan vs Full Scan
./build/btree_demo data/btree_fase1.db 5000 200 --sweep   # comparativa con 1.000 / 5.000 / 20.000 filas
```

Opciones de la demo: `[ruta.db] [filas] [consultas] [--trace] [--sweep] [--degree=N]`.

### Pruebas automatizadas

```bash
ctest --test-dir build --output-on-failure              # toda la suite
ctest --test-dir build -R BPlusTree --output-on-failure  # solo el B+ Tree (22 casos)
```

### Grado `t` del árbol

Se usa el **grado mínimo `t`**: cada nodo no raíz tiene entre `t-1` y `2t-1` claves (un nodo interno, entre `t` y `2t` hijos) y se divide al llegar a `2t`.
Por defecto `t = 16` (hasta 31 claves por nodo): da árboles de altura 3 con pocos miles de filas, lo que permite ver splits y crecimiento de altura en la demo.
El límite físico es `t = 200` (`16 + 10·(2t-1) <= 4096` bytes); `BPlusTree::Create` rechaza grados mayores. Las pruebas usan `t = 2..8` para forzar splits frecuentes.
