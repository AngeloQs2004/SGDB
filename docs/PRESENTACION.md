# Guion de presentación — Fase 1 (12 min)

## Parte 1 — Arquitectura (4–5 min)

1. **Contexto (30 s):** servidor de BD para `SELECT`; la Fase 1 es almacenamiento + índice para evitar el Full Table Scan.
2. **Storage Engine (1 min):** páginas de 4096 bytes, `BufferPoolManager`, `TableHeap`; las tuplas INT/VARCHAR se serializan en binario dentro de `TablePage`; cada tupla se identifica con `RecordId{page_id, slot_id}` (el RowID).
3. **Nodo B+ Tree (1.5 min):** cada nodo = 1 página. Mostrar los diagramas de `docs/FASE1_BTREE.md`:
   - hoja: `clave:int32 → RecordId` (10 bytes por entrada) + `next_leaf`;
   - interno: `child0, key0, child1, key1, …` (separadores).
4. **Grado `t` (1 min):** nodo no raíz entre `t-1` y `2t-1` claves; split al llegar a `2t`. `t=16` por defecto; el límite físico es `t=200` (`16 + 10·(2t-1) ≤ 4096`). Se justificó `t=16` por claridad de la demo, no por eficiencia de espacio.
5. **Split (1 min):** hoja → copia de la primera clave de la derecha al padre; interno → la clave central *sube*; si se divide la raíz, crece la altura. Búsqueda: `O(log_t n)` páginas.

## Parte 2 — Demostración (7–8 min)

```bash
./build/btree_demo data/btree_fase1.db 5000 200 --trace
```

1. **Inserción masiva en tiempo real (2 min):** mostrar el log `split hoja / split interno / nueva raiz -> altura del arbol = N`. Señalar el momento en que la altura pasa de 2 a 3.
2. **Bulk load (1 min):** la línea de comparación: mismas 5.000 claves, 235 splits uno a uno vs 0 en bulk, ~38x más rápido y menos nodos. Ambos con `invariantes=OK`.
3. **Index Scan vs Full Scan (2 min):** 3 nodos por consulta frente a ~2.400 registros; speedup y `disk_reads`.
4. **Escalabilidad (1.5 min):** `./build/btree_demo data/btree_fase1.db 5000 200 --sweep` — el speedup crece con n (≈45x → 190x → 700x) porque el índice solo gana un nivel mientras el scan crece linealmente.
5. **Pruebas (1 min):** `ctest --test-dir build -R BPlusTree` — 22 casos, todos con el validador de invariantes.

## Preguntas probables

| Pregunta | Respuesta corta |
|---|---|
| ¿Por qué B+ y no B? | Los datos (RowIDs) solo en hojas → nodos internos más pequeños, más fan-out, hojas enlazadas para rangos. |
| ¿Cuántas claves máximo por nodo? | `2t-1`; mínimo `t-1` (no raíz). |
| ¿Se promueve o se copia la clave? | Hoja: se copia (sigue en la hoja). Interno: sube y se elimina del hijo. |
| ¿Cómo garantizan el balance? | Todo crecimiento ocurre en la raíz (todas las hojas bajan a la vez); `Validate()` lo verifica. |
| ¿Qué hace el bulk load? | Ordena, reparte parejo en hojas, construye niveles hacia arriba; sin splits. |
| ¿Por qué el índice a veces hace más I/O en tablas pequeñas? | Con ~1.000 filas la tabla cabe en el Buffer Pool de 16 frames; el índice ocupa más páginas que la tabla. La ventaja de I/O aparece cuando la tabla excede el pool. |
| ¿Claves duplicadas / VARCHAR? | Duplicados rechazados (índice único, como una PK). El índice solo soporta claves `INT`; las tuplas sí admiten INT y VARCHAR. |
| ¿Eliminación? | Fuera del alcance de la Fase 1 (solo search/insert/bulk load). |
