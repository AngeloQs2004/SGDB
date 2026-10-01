#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "minidb/database/database.hpp"

namespace minidb {
namespace {

/// Display width of a UTF-8 string, counting characters rather than bytes.
///
/// Without this the table columns misalign on the accented Spanish data, since
/// 'ó' occupies two bytes but one column.
std::size_t DisplayWidth(const std::string& text) {
    std::size_t width = 0;
    for (char c : text) {
        // Continuation bytes (10xxxxxx) do not start a new character.
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) {
            ++width;
        }
    }
    return width;
}

std::string Pad(const std::string& text, std::size_t width) {
    const std::size_t used = DisplayWidth(text);
    return text + std::string(width > used ? width - used : 0, ' ');
}

/// Prints a result as an ASCII table.
void PrintRows(const QueryResult& result) {
    if (result.column_names.empty()) {
        return;
    }

    std::vector<std::size_t> widths;
    for (const std::string& name : result.column_names) {
        widths.push_back(DisplayWidth(name));
    }
    for (const Record& row : result.rows) {
        for (std::size_t i = 0; i < row.Size() && i < widths.size(); ++i) {
            widths[i] = std::max(widths[i], DisplayWidth(ValueToString(row.GetValue(i))));
        }
    }

    auto separator = [&widths]() {
        std::string line = "+";
        for (std::size_t width : widths) {
            line += std::string(width + 2, '-') + "+";
        }
        return line;
    };

    std::cout << separator() << '\n' << "|";
    for (std::size_t i = 0; i < result.column_names.size(); ++i) {
        std::cout << ' ' << Pad(result.column_names[i], widths[i]) << " |";
    }
    std::cout << '\n' << separator() << '\n';

    for (const Record& row : result.rows) {
        std::cout << "|";
        for (std::size_t i = 0; i < row.Size() && i < widths.size(); ++i) {
            std::cout << ' ' << Pad(ValueToString(row.GetValue(i)), widths[i]) << " |";
        }
        std::cout << '\n';
    }
    std::cout << separator() << '\n';
}

/// Prints the plan as a tree, with what each operator did.
///
/// The counters are what make the difference between access paths visible: the
/// same query answered by an index scan pulls two records, while a sequential
/// scan plus filter pulls the whole table and throws nearly all of it away.
void PrintPlan(const QueryResult& result) {
    if (result.metrics.empty()) {
        return;
    }

    std::cout << "Plan físico (modelo Volcano):\n";
    for (std::size_t i = 0; i < result.metrics.size(); ++i) {
        const OperatorMetrics& op = result.metrics[i];
        // The root is at the top and each child is indented under its parent, so
        // the tree reads in the direction the records travel: bottom to top.
        const std::string branch = (i == 0) ? "  " : std::string(2 * i, ' ') + "└─ ";
        std::cout << Pad(branch + op.name, 40) << "filas=" << op.rows_produced
                  << "  next=" << op.next_calls;
        if (op.batches_produced > 0) {
            std::cout << "  lotes=" << op.batches_produced;
        }
        if (op.distance_calculations > 0) {
            std::cout << "  distancias=" << op.distance_calculations;
        }
        std::cout << '\n';
    }
}

/// One line with what the statement cost, in time and in pages read.
void PrintCost(const QueryResult& result) {
    std::cout << "Tiempo: " << std::fixed << std::setprecision(3) << result.elapsed_ms
              << " ms | páginas leídas del disco: " << result.pages_read << " | buffer "
              << result.buffer_hits << '/' << result.buffer_misses << " (aciertos/fallos)\n";
    std::cout.unsetf(std::ios::fixed);
}

void PrintHelp() {
    std::cout << R"(SQL admitido:
  CREATE TABLE <tabla> (<col> INT [PRIMARY KEY] | <col> VARCHAR(<n>)
                        | <col> VECTOR(<d>), ...);
  INSERT INTO <tabla> VALUES (<v1>, <v2>, ...);   -- un vector: [0.1, 0.9, ...]
  SELECT * | <col> | COUNT(*), ... FROM <tabla> [WHERE <col> <op> <valor>]
         [GROUP BY <col>] [ORDER BY <col> [ASC|DESC]];
  SELECT ... FROM <tabla> [WHERE ...]
         NEAREST <col_vector> TO [<v1>, ...] [USING EUCLIDEAN|COSINE|DOT]
         LIMIT <k>;
  UPDATE <tabla> SET <col> = <valor> [, ...] [WHERE <col> <op> <valor>];
  DELETE FROM <tabla> [WHERE <col> <op> <valor>];

Operadores de comparación: =  !=  <  <=  >  >=
Las palabras clave no distinguen mayúsculas; el ';' final es opcional.
Tras cada sentencia se muestra el plan físico, el tiempo y las páginas leídas.

Comandos internos:
  .help     Esta ayuda
  .clear    Limpia la pantalla
  .schema   Esquema de la tabla
  .pages    Páginas del archivo por tipo
  .buffer   Frames del Buffer Pool y estadísticas
  .files    Archivos en disco y su tamaño
  .flush    Sincroniza todas las páginas sucias
  .indice [on|off]      Activa o desactiva el uso del índice (para medir)
  .vectorizado [on|off] Cambia entre ejecución tupla a tupla y por lotes
  .topk [on|off]        Top-k acotado u orden completo en las consultas NEAREST
  .bench [n]            Compara n búsquedas por clave con y sin índice
  .benchvec [n]         Compara los dos modelos de ejecución
  .knnbench [k] [n]     Compara Top-k acotado y orden completo en n consultas
  .knncsv <ruta> [k] [n]  Igual, exportando una fila CSV por consulta
  .exit     Sincroniza y sale

Ejemplos:
  CREATE TABLE students (id INT PRIMARY KEY, name VARCHAR(50), age INT, career VARCHAR(50));
  INSERT INTO students VALUES (1, 'Ana', 20, 'Ciencia de la Computación');
  SELECT * FROM students WHERE id = 1;
  SELECT * FROM students WHERE age >= 20 ORDER BY age DESC;
  SELECT career, COUNT(*) FROM students GROUP BY career;
)";
}

void PrintSchema(const Database& db) {
    if (!db.HasTable()) {
        std::cout << "No hay ninguna tabla definida.\n";
        return;
    }

    const Catalog& catalog = db.GetCatalog();
    const Schema& schema = catalog.GetSchema();

    std::cout << "Tabla: " << catalog.TableName() << " (" << catalog.RecordCount()
              << " registros)\n";
    for (const Column& column : schema.Columns()) {
        std::string type_name;
        switch (column.type) {
            case ColumnType::kInteger:
                type_name = "INT";
                break;
            case ColumnType::kVarchar:
                type_name = "VARCHAR(" + std::to_string(column.max_length) + ")";
                break;
            case ColumnType::kVector:
                type_name = "VECTOR(" + std::to_string(column.max_length) + ")";
                break;
        }
        std::cout << "  " << column.name << ' ' << type_name
                  << (column.is_primary_key ? " PRIMARY KEY" : "") << '\n';
    }
}

void PrintPages(Database& db) {
    std::cout << "Tamaño de página:      " << kPageSize << " bytes\n"
              << "Páginas totales:       " << db.Disk().PageCount() << '\n'
              << "  Cabecera del archivo: 1 (página 0)\n"
              << "  Catálogo:             1 (página 1)\n"
              << "  Índice hash:          " << db.IndexPageCount()
              << " (cabecera + buckets + overflow)\n"
              << "  Datos de la tabla:    " << db.TablePageCount() << '\n';

    const PageId free_head = db.Disk().FreePageHead();
    std::cout << "Lista de páginas libres: "
              << (free_head == kInvalidPageId ? std::string("vacía")
                                              : "cabeza en la página " + std::to_string(free_head))
              << '\n';
}

void PrintBuffer(const Database& db) {
    const BufferPoolManager& pool = db.Pool();
    const BufferPoolStatistics& stats = pool.Statistics();

    std::cout << "Frames del Buffer Pool: " << pool.FrameCount() << " (política LRU)\n";
    std::cout << "+-------+---------+-------+-----------+\n"
              << "| frame | página  | pin   | modificada|\n"
              << "+-------+---------+-------+-----------+\n";

    std::size_t frame_id = 0;
    for (const Frame& frame : pool.Frames()) {
        std::cout << "| " << Pad(std::to_string(frame_id), 5) << " | "
                  << Pad(frame.is_occupied ? std::to_string(frame.page_id) : "-", 7) << " | "
                  << Pad(std::to_string(frame.pin_count), 5) << " | "
                  << Pad(frame.is_dirty ? "sí" : "no", 9) << " |\n";
        ++frame_id;
    }
    std::cout << "+-------+---------+-------+-----------+\n";

    const std::uint64_t accesses = stats.hits + stats.misses;
    std::cout << "Aciertos (hits):    " << stats.hits << '\n'
              << "Fallos (misses):    " << stats.misses << '\n'
              << "Reemplazos (LRU):   " << stats.evictions << '\n'
              << "Lecturas de disco:  " << stats.disk_reads << '\n'
              << "Escrituras a disco: " << stats.disk_writes << '\n';
    if (accesses > 0) {
        std::cout << "Tasa de aciertos:   " << (stats.hits * 100 / accesses) << "%\n";
    }
}

void PrintFiles(Database& db, const std::filesystem::path& config_path) {
    std::cout << "Archivo de datos:  " << std::filesystem::absolute(db.Path()).string() << '\n'
              << "  Tamaño:          " << db.Disk().FileSize() << " bytes ("
              << db.Disk().PageCount() << " páginas de " << kPageSize << ")\n";

    std::error_code error;
    const auto config_size = std::filesystem::file_size(config_path, error);
    std::cout << "Archivo de config: " << std::filesystem::absolute(config_path).string() << '\n'
              << "  Tamaño:          "
              << (error ? std::string("no existe (se usan los valores por defecto)")
                        : std::to_string(config_size) + " bytes")
              << '\n';
    std::cout << "El sistema no genera archivos temporales. ORDER BY y GROUP BY son\n"
                 "operadores bloqueantes, pero materializan en RAM (acotados por el tamaño\n"
                 "de la tabla); una ordenación externa por mezcla, que sí necesitaría\n"
                 "archivos temporales, sería el paso siguiente y no está implementada.\n";
}

/// Compares the same lookups answered with and without the index.
///
/// The keys are collected first, outside the measurement, and the queries are
/// identical in both rounds: the only difference is whether the planner is
/// allowed to reach for the index. It reads and never modifies the table, so it
/// is safe to run at any point in a demo.
void RunBenchmark(Database& db, std::size_t queries) {
    if (!db.HasTable()) {
        std::cout << "No hay ninguna tabla que medir. Use CREATE TABLE e inserte filas.\n";
        return;
    }

    const Catalog& catalog = db.GetCatalog();
    const std::string key_column = catalog.GetSchema().GetColumn(catalog.GetSchema().PrimaryKeyIndex()).name;

    // One scan up front to learn which keys exist. Not measured.
    std::vector<std::int32_t> keys;
    for (const Record& row : db.Execute("SELECT " + key_column + " FROM " + catalog.TableName()).rows) {
        keys.push_back(std::get<std::int32_t>(row.GetValue(0)));
    }
    if (keys.empty()) {
        std::cout << "La tabla está vacía; no hay nada que medir.\n";
        return;
    }

    struct Round {
        const char* label;
        bool use_index;
        double elapsed_ms = 0.0;
        std::uint64_t pages_read = 0;
        std::uint64_t rows_examined = 0;
    };
    Round rounds[] = {{"IndexScan (con índice)", true}, {"SeqScan+Filter (sin índice)", false}};

    const bool restore = db.IndexEnabled();
    for (Round& round : rounds) {
        db.SetIndexEnabled(round.use_index);
        for (std::size_t i = 0; i < queries; ++i) {
            // Spread the lookups over the whole key range so neither round is
            // favoured by hitting the same page every time.
            const std::int32_t key = keys[(i * keys.size() / queries) % keys.size()];
            const QueryResult result =
                db.Execute("SELECT * FROM " + catalog.TableName() + " WHERE " + key_column + " = " +
                           std::to_string(key));
            round.elapsed_ms += result.elapsed_ms;
            round.pages_read += result.pages_read;
            for (const OperatorMetrics& op : result.metrics) {
                if (op.name == "SequentialScanOperator" || op.name == "IndexScanOperator") {
                    round.rows_examined += op.rows_produced;
                }
            }
        }
    }
    db.SetIndexEnabled(restore);

    std::cout << "Comparación de rendimiento con y sin índice\n"
              << "Tabla: " << catalog.TableName() << "   " << keys.size() << " registros   "
              << db.TablePageCount() << " páginas de datos   " << queries
              << " consultas por clave primaria\n\n";

    const std::string separator =
        "+-----------------------------+-------------+-------------+------------+-------------+";
    std::cout << separator << '\n'
              << "| plan                        | tiempo      | por consulta| páginas    "
                 "| registros   |\n"
              << separator << '\n';
    for (const Round& round : rounds) {
        std::cout << "| " << Pad(round.label, 27) << " | "
                  << Pad(std::format("{:.3f} ms", round.elapsed_ms), 11) << " | "
                  << Pad(std::format("{:.4f} ms", round.elapsed_ms / static_cast<double>(queries)),
                         11)
                  << " | " << Pad(std::to_string(round.pages_read), 10) << " | "
                  << Pad(std::to_string(round.rows_examined), 11) << " |\n";
    }
    std::cout << separator << '\n';

    const Round& with = rounds[0];
    const Round& without = rounds[1];
    std::cout << "\nRegistros examinados: " << with.rows_examined << " con índice frente a "
              << without.rows_examined << " sin él.\n";
    if (with.elapsed_ms > 0.0) {
        std::cout << std::format("Aceleración en tiempo: {:.1f}x.\n",
                                 without.elapsed_ms / with.elapsed_ms);
    }

    // Las lecturas de disco no siempre favorecen al índice, y merece la pena
    // decirlo: el índice tiene sus propias páginas (cabecera y buckets), así que
    // en una tabla que cabe casi entera en el pool puede costar más lecturas que
    // recorrerla. La ventaja en registros examinados, en cambio, es estructural.
    if (without.pages_read > with.pages_read && with.pages_read > 0) {
        std::cout << std::format("Páginas leídas del disco: {:.1f}x menos con índice.\n",
                                 static_cast<double>(without.pages_read) /
                                     static_cast<double>(with.pages_read));
    } else if (without.pages_read > with.pages_read) {
        std::cout << "Con índice no hizo falta ninguna lectura de disco; sin él, "
                  << without.pages_read << ".\n";
    } else {
        std::cout << "Páginas leídas del disco: " << with.pages_read << " con índice frente a "
                  << without.pages_read << " sin él.\n"
                  << "El índice no reduce las lecturas en esta tabla: sus propias páginas\n"
                     "(cabecera y buckets) compiten por el Buffer Pool con las de datos.\n"
                     "La ventaja se ve con tablas mayores que el pool; pruebe con más filas.\n";
    }
}

/// Splits a dot command into its name and its argument, if any.
[[nodiscard]] std::pair<std::string, std::string> SplitCommand(const std::string& command) {
    const auto space = command.find(' ');
    if (space == std::string::npos) {
        return {command, ""};
    }
    const auto argument_start = command.find_first_not_of(' ', space);
    return {command.substr(0, space),
            argument_start == std::string::npos ? "" : command.substr(argument_start)};
}

void RunVectorizedCommand(Database& db, const std::string& argument) {
    if (argument == "on") {
        db.SetVectorizedEnabled(true);
    } else if (argument == "off") {
        db.SetVectorizedEnabled(false);
    } else if (!argument.empty()) {
        std::cout << "Uso: .vectorizado [on|off]\n";
        return;
    }

    if (db.VectorizedEnabled()) {
        std::cout << "Ejecución vectorizada por lotes: ACTIVADA\n"
                     "Los escaneos completos usan VectorizedScanOperator (una fijación de\n"
                     "página por página, no por registro) y VectorizedFilterOperator\n"
                     "(comparación sobre columna contigua y vector de selección).\n";
    } else {
        std::cout << "Ejecución vectorizada por lotes: desactivada\n"
                     "Los escaneos completos usan el camino Volcano tupla a tupla.\n";
    }
}

/// Compares the two execution models on the same full-scan query.
///
/// The query, the data and the buffer pool are identical in both rounds; the only
/// difference is whether the planner builds the batch-at-a-time operators.
void RunVectorizedBenchmark(Database& db, std::size_t repetitions) {
    if (!db.HasTable()) {
        std::cout << "No hay ninguna tabla que medir. Use CREATE TABLE e inserte filas.\n";
        return;
    }

    const Catalog& catalog = db.GetCatalog();
    const Schema& schema = catalog.GetSchema();

    // A filter over an INT column that is not the primary key: the shape that
    // forces a full scan and lets the comparison be vectorized.
    std::string filter_column;
    for (std::size_t i = 0; i < schema.ColumnCount(); ++i) {
        if (schema.GetColumn(i).type == ColumnType::kInteger && i != schema.PrimaryKeyIndex()) {
            filter_column = schema.GetColumn(i).name;
            break;
        }
    }
    if (filter_column.empty()) {
        std::cout << "La tabla no tiene ninguna columna INT que no sea la clave primaria,\n"
                     "así que no hay una consulta con filtro vectorizable que medir.\n";
        return;
    }

    // A predicate that keeps roughly nothing, so the cost measured is the scan
    // and the comparison rather than the assembly of the result.
    const std::string sql =
        "SELECT * FROM " + catalog.TableName() + " WHERE " + filter_column + " < -1";

    struct Round {
        const char* label;
        bool vectorized;
        double elapsed_ms = 0.0;
        std::uint64_t next_calls = 0;
        std::uint64_t batches = 0;
        std::uint64_t buffer_accesses = 0;
    };
    Round rounds[] = {{"Volcano tupla a tupla", false}, {"Vectorizado por lotes", true}};

    const bool restore = db.VectorizedEnabled();
    for (Round& round : rounds) {
        db.SetVectorizedEnabled(round.vectorized);
        for (std::size_t i = 0; i < repetitions; ++i) {
            const QueryResult result = db.Execute(sql);
            round.elapsed_ms += result.elapsed_ms;
            round.buffer_accesses += result.buffer_hits + result.buffer_misses;
            for (const OperatorMetrics& op : result.metrics) {
                round.next_calls += op.next_calls;
                round.batches += op.batches_produced;
            }
        }
    }
    db.SetVectorizedEnabled(restore);

    std::cout << "Comparación de los dos modelos de ejecución\n"
              << "Consulta: " << sql << '\n'
              << "Tabla: " << catalog.RecordCount() << " registros en " << db.TablePageCount()
              << " páginas   " << repetitions << " repeticiones\n\n";

    const std::string separator =
        "+-------------------------+-------------+-------------+------------+---------------+";
    std::cout << separator << '\n'
              << "| modelo                  | tiempo      | por consulta| lotes      "
                 "| accesos al BP |\n"
              << separator << '\n';
    for (const Round& round : rounds) {
        std::cout << "| " << Pad(round.label, 23) << " | "
                  << Pad(std::format("{:.3f} ms", round.elapsed_ms), 11) << " | "
                  << Pad(std::format("{:.4f} ms",
                                     round.elapsed_ms / static_cast<double>(repetitions)),
                         11)
                  << " | " << Pad(std::to_string(round.batches), 10) << " | "
                  << Pad(std::to_string(round.buffer_accesses), 13) << " |\n";
    }
    std::cout << separator << '\n';

    const Round& tuple = rounds[0];
    const Round& vector = rounds[1];
    std::cout << "\nLlamadas a Next() en total: " << tuple.next_calls << " tupla a tupla frente a "
              << vector.next_calls << " vectorizado.\n"
              << "Accesos al Buffer Pool: " << tuple.buffer_accesses << " frente a "
              << vector.buffer_accesses << " (el escaneo por lotes fija cada página una vez,\n"
                 "no una vez por registro).\n";
    if (vector.elapsed_ms > 0.0) {
        std::cout << std::format("Aceleración: {:.2f}x.\n", tuple.elapsed_ms / vector.elapsed_ms);
    }
}

/// Everything a nearest neighbour benchmark needs to know about the table.
struct VectorColumnInfo {
    std::string name;
    std::size_t dimension = 0;
};

/// Finds the table's VECTOR column, or reports why there is nothing to measure.
[[nodiscard]] std::optional<VectorColumnInfo> FindVectorColumn(const Database& db) {
    if (!db.HasTable()) {
        std::cout << "No hay ninguna tabla. Use CREATE TABLE con una columna VECTOR.\n";
        return std::nullopt;
    }
    const Schema& schema = db.GetCatalog().GetSchema();
    for (const Column& column : schema.Columns()) {
        if (column.type == ColumnType::kVector) {
            return VectorColumnInfo{column.name, column.max_length};
        }
    }
    std::cout << "La tabla '" << db.GetCatalog().TableName()
              << "' no tiene ninguna columna VECTOR, así que no hay búsqueda por similitud"
                 " que medir.\n";
    return std::nullopt;
}

/// Query vectors for the benchmarks, drawn from a fixed seed.
///
/// Generated here rather than passed in so that both strategies are measured
/// against byte-identical queries and the whole experiment is reproducible from
/// the binary alone. std::mt19937 with an explicit seed is portable across
/// platforms, unlike a default-constructed random device.
[[nodiscard]] std::vector<Vector> MakeQueryVectors(std::size_t count, std::size_t dimension,
                                                   std::uint32_t seed = 42) {
    std::mt19937 generator(seed);
    std::uniform_real_distribution<float> component(0.0F, 1.0F);

    std::vector<Vector> queries;
    queries.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        Vector query;
        query.reserve(dimension);
        for (std::size_t j = 0; j < dimension; ++j) {
            query.push_back(component(generator));
        }
        queries.push_back(std::move(query));
    }
    return queries;
}

/// Writes a nearest neighbour query in the system's own SQL.
[[nodiscard]] std::string KnnQuery(const Database& db, const std::string& column,
                                   const Vector& query, std::size_t k) {
    std::string sql = "SELECT * FROM " + db.GetCatalog().TableName() + " NEAREST " + column + " TO [";
    for (std::size_t i = 0; i < query.size(); ++i) {
        sql += std::format("{}{:.6f}", i == 0 ? "" : ",", query[i]);
    }
    return sql + "] LIMIT " + std::to_string(k);
}

/// One measured execution of a k-NN query.
struct KnnSample {
    double elapsed_ms = 0.0;
    std::uint64_t distances = 0;
    std::uint64_t pages_read = 0;
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::size_t rows = 0;
};

/// Runs one batch of queries under one strategy. `topk` chooses the bounded heap
/// or the full sort; nothing else differs between the two rounds.
[[nodiscard]] std::vector<KnnSample> RunKnnBatch(Database& db, const std::string& column,
                                                 const std::vector<Vector>& queries, std::size_t k,
                                                 bool topk) {
    db.SetTopKEnabled(topk);

    // Warm-up: the first query of a cold run pays for reading pages that every
    // later query finds resident, and it would distort the minimum.
    if (!queries.empty()) {
        (void)db.Execute(KnnQuery(db, column, queries.front(), k));
    }

    std::vector<KnnSample> samples;
    samples.reserve(queries.size());
    for (const Vector& query : queries) {
        const QueryResult result = db.Execute(KnnQuery(db, column, query, k));
        samples.push_back(KnnSample{result.elapsed_ms, result.DistanceCalculations(),
                                    result.pages_read, result.buffer_hits, result.buffer_misses,
                                    result.rows.size()});
    }
    return samples;
}

/// Percentile by nearest rank over an already sorted sample.
[[nodiscard]] double Percentile(const std::vector<double>& sorted, double fraction) {
    if (sorted.empty()) {
        return 0.0;
    }
    const auto index = static_cast<std::size_t>(fraction * static_cast<double>(sorted.size()));
    return sorted[std::min(index, sorted.size() - 1)];
}

struct KnnStatistics {
    double mean = 0.0;
    double minimum = 0.0;
    double maximum = 0.0;
    double deviation = 0.0;
    double p50 = 0.0;
    double p95 = 0.0;
    double p99 = 0.0;
    double queries_per_second = 0.0;
    std::uint64_t distances = 0;
    std::uint64_t pages_read = 0;
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
};

[[nodiscard]] KnnStatistics Summarise(const std::vector<KnnSample>& samples) {
    KnnStatistics stats;
    if (samples.empty()) {
        return stats;
    }

    std::vector<double> latencies;
    latencies.reserve(samples.size());
    for (const KnnSample& sample : samples) {
        latencies.push_back(sample.elapsed_ms);
        stats.distances += sample.distances;
        stats.pages_read += sample.pages_read;
        stats.hits += sample.hits;
        stats.misses += sample.misses;
    }
    std::ranges::sort(latencies);

    double total = 0.0;
    for (double latency : latencies) {
        total += latency;
    }
    const auto count = static_cast<double>(latencies.size());
    stats.mean = total / count;

    double squared = 0.0;
    for (double latency : latencies) {
        squared += (latency - stats.mean) * (latency - stats.mean);
    }
    stats.deviation = std::sqrt(squared / count);

    stats.minimum = latencies.front();
    stats.maximum = latencies.back();
    stats.p50 = Percentile(latencies, 0.50);
    stats.p95 = Percentile(latencies, 0.95);
    stats.p99 = Percentile(latencies, 0.99);
    stats.queries_per_second = stats.mean > 0.0 ? 1000.0 / stats.mean : 0.0;
    return stats;
}

/// Compares the bounded Top-k heap against the full sort on the same queries.
void RunKnnBenchmark(Database& db, std::size_t k, std::size_t query_count) {
    const auto column = FindVectorColumn(db);
    if (!column.has_value()) {
        return;
    }

    const std::vector<Vector> queries = MakeQueryVectors(query_count, column->dimension);
    const bool restore = db.TopKEnabled();

    const KnnStatistics topk = Summarise(RunKnnBatch(db, column->name, queries, k, true));
    const KnnStatistics full = Summarise(RunKnnBatch(db, column->name, queries, k, false));
    db.SetTopKEnabled(restore);

    std::cout << "Búsqueda exacta de vecinos más cercanos\n"
              << "Tabla: " << db.GetCatalog().TableName() << "   "
              << db.GetCatalog().RecordCount() << " vectores de dimensión " << column->dimension
              << "   k = " << k << "   " << query_count << " consultas\n\n";

    const std::string separator =
        "+-----------------------------+-------------+-----------+-----------+-----------+--------+";
    std::cout << separator << '\n'
              << "| estrategia                  | media       | p50       | p95       | p99       "
                 "| cons/s |\n"
              << separator << '\n';

    const auto row = [](const char* label, const KnnStatistics& stats) {
        std::cout << "| " << Pad(label, 27) << " | "
                  << Pad(std::format("{:.4f} ms", stats.mean), 11) << " | "
                  << Pad(std::format("{:.4f}", stats.p50), 9) << " | "
                  << Pad(std::format("{:.4f}", stats.p95), 9) << " | "
                  << Pad(std::format("{:.4f}", stats.p99), 9) << " | "
                  << Pad(std::format("{:.0f}", stats.queries_per_second), 6) << " |\n";
    };
    row("Top-k acotado (O(n log k))", topk);
    row("Orden completo (O(n log n))", full);
    std::cout << separator << '\n';

    std::cout << std::format("\nLatencia mínima y máxima: Top-k {:.4f}/{:.4f} ms, "
                             "orden completo {:.4f}/{:.4f} ms.\n",
                             topk.minimum, topk.maximum, full.minimum, full.maximum);
    std::cout << std::format("Desviación estándar: Top-k {:.4f} ms, orden completo {:.4f} ms.\n",
                             topk.deviation, full.deviation);
    std::cout << "Distancias calculadas: " << topk.distances << " y " << full.distances
              << ". Son iguales a propósito: las dos estrategias son exactas y examinan\n"
                 "todos los registros; lo que cambia es cuánto ordenan después.\n";
    std::cout << "Páginas leídas del disco: " << topk.pages_read << " y " << full.pages_read
              << ". Aciertos/fallos del buffer: " << topk.hits << "/" << topk.misses << " y "
              << full.hits << "/" << full.misses << ".\n";
    if (topk.mean > 0.0) {
        std::cout << std::format("Aceleración del Top-k acotado: {:.2f}x.\n", full.mean / topk.mean);
    }
}

/// Appends one CSV row per query and strategy, for the experiment scripts.
void RunKnnCsv(Database& db, const std::string& path, std::size_t k, std::size_t query_count) {
    const auto column = FindVectorColumn(db);
    if (!column.has_value()) {
        return;
    }

    const std::vector<Vector> queries = MakeQueryVectors(query_count, column->dimension);
    const bool restore = db.TopKEnabled();
    const std::vector<KnnSample> topk = RunKnnBatch(db, column->name, queries, k, true);
    const std::vector<KnnSample> full = RunKnnBatch(db, column->name, queries, k, false);
    db.SetTopKEnabled(restore);

    const bool write_header = !std::filesystem::exists(path);
    std::ofstream csv(path, std::ios::app);
    if (!csv) {
        std::cout << "No se pudo abrir '" << path << "' para escritura.\n";
        return;
    }
    if (write_header) {
        csv << "estrategia,vectores,dimension,k,consulta,latencia_ms,distancias,"
               "registros_examinados,page_reads,buffer_hits,buffer_misses,filas\n";
    }

    const auto write = [&](const char* label, const std::vector<KnnSample>& samples) {
        for (std::size_t i = 0; i < samples.size(); ++i) {
            const KnnSample& sample = samples[i];
            csv << label << ',' << db.GetCatalog().RecordCount() << ',' << column->dimension << ','
                << k << ',' << i << ',' << std::format("{:.6f}", sample.elapsed_ms) << ','
                << sample.distances << ',' << sample.distances << ',' << sample.pages_read << ','
                << sample.hits << ',' << sample.misses << ',' << sample.rows << '\n';
        }
    };
    write("topk", topk);
    write("fullsort", full);

    std::cout << "Escritas " << (topk.size() + full.size()) << " filas en " << path << ".\n";
}

void RunTopKCommand(Database& db, const std::string& argument) {
    if (!db.HasTable()) {
        std::cout << "No hay ninguna tabla definida.\n";
        return;
    }

    if (argument == "on") {
        db.SetTopKEnabled(true);
    } else if (argument == "off") {
        db.SetTopKEnabled(false);
    } else if (!argument.empty()) {
        std::cout << "Uso: .topk [on|off]\n";
        return;
    }

    if (db.TopKEnabled()) {
        std::cout << "Selección Top-k acotada: ACTIVADA (KnnScanOperator, O(n log k))\n";
    } else {
        std::cout << "Selección Top-k acotada: desactivada\n"
                     "Las consultas NEAREST ordenarán las n distancias completas\n"
                     "(KnnFullSortOperator, O(n log n)). Es la línea base de la evaluación.\n";
    }
}

void RunIndexCommand(Database& db, const std::string& argument) {
    if (!db.HasTable()) {
        std::cout << "No hay ninguna tabla definida.\n";
        return;
    }

    if (argument == "on") {
        db.SetIndexEnabled(true);
    } else if (argument == "off") {
        db.SetIndexEnabled(false);
    } else if (!argument.empty()) {
        std::cout << "Uso: .indice [on|off]\n";
        return;
    }

    std::cout << "Uso del índice: " << (db.IndexEnabled() ? "activado" : "DESACTIVADO") << '\n';
    if (!db.IndexEnabled()) {
        std::cout << "Las búsquedas por clave primaria usarán un escaneo secuencial.\n"
                     "Sirve para medir el coste de no tener índice; use .indice on al terminar.\n";
    }
}

/// Runs a dot command. Returns false when the session should end.
bool RunInternalCommand(const std::string& full_command, Database& db,
                        const std::filesystem::path& config_path) {
    const auto [command, argument] = SplitCommand(full_command);

    if (command == ".exit" || command == ".quit") {
        return false;
    }
    if (command == ".help") {
        PrintHelp();
    } else if (command == ".clear") {
        // ANSI clear-screen and cursor-home; avoids spawning an external shell.
        std::cout << "\x1B[2J\x1B[H" << std::flush;
    } else if (command == ".schema") {
        PrintSchema(db);
    } else if (command == ".pages") {
        PrintPages(db);
    } else if (command == ".buffer") {
        PrintBuffer(db);
    } else if (command == ".files") {
        PrintFiles(db, config_path);
    } else if (command == ".flush") {
        db.Flush();
        std::cout << "Páginas sincronizadas con el disco.\n";
    } else if (command == ".indice") {
        RunIndexCommand(db, argument);
    } else if (command == ".vectorizado") {
        RunVectorizedCommand(db, argument);
    } else if (command == ".topk") {
        RunTopKCommand(db, argument);
    } else if (command == ".knnbench" || command == ".knncsv") {
        // .knnbench [k] [consultas]      .knncsv <ruta> [k] [consultas]
        std::istringstream arguments(argument);
        std::string path;
        if (command == ".knncsv" && !(arguments >> path)) {
            std::cout << "Uso: .knncsv <ruta> [k] [consultas]\n";
            return true;
        }

        std::size_t k = 10;
        std::size_t queries = 20;
        arguments >> k;
        arguments >> queries;
        if (k == 0 || queries == 0) {
            std::cout << "k y el número de consultas deben ser mayores que cero.\n";
            return true;
        }

        if (command == ".knnbench") {
            RunKnnBenchmark(db, k, queries);
        } else {
            RunKnnCsv(db, path, k, queries);
        }
    } else if (command == ".bench" || command == ".benchvec") {
        std::size_t repetitions = command == ".bench" ? 100 : 20;
        if (!argument.empty()) {
            const auto* first = argument.data();
            const auto* last = first + argument.size();
            if (std::from_chars(first, last, repetitions).ec != std::errc{} || repetitions == 0) {
                std::cout << "Uso: " << command << " [número de repeticiones]\n";
                return true;
            }
        }
        if (command == ".bench") {
            RunBenchmark(db, repetitions);
        } else {
            RunVectorizedBenchmark(db, repetitions);
        }
    } else {
        std::cout << "Comando desconocido: " << command << ". Use .help\n";
    }
    return true;
}

std::string Trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

/// Reads statements from `input` and runs them.
///
/// A statement ends at a semicolon, so it may span several lines. Errors the
/// user can fix are reported and the session continues; anything else
/// propagates and ends the process with a non-zero status.
int RunSession(std::istream& input, Database& db, const std::filesystem::path& config_path,
               bool interactive) {
    std::string pending;
    std::string line;

    while (true) {
        if (interactive) {
            std::cout << (pending.empty() ? "minidb> " : "   ...> ") << std::flush;
        }
        if (!std::getline(input, line)) {
            break;
        }

        const std::string trimmed = Trim(line);
        if (pending.empty() && (trimmed.empty() || trimmed.starts_with("--"))) {
            continue;
        }
        if (pending.empty() && trimmed.starts_with(".")) {
            if (!RunInternalCommand(trimmed, db, config_path)) {
                break;
            }
            continue;
        }

        pending += (pending.empty() ? "" : " ") + trimmed;
        if (!pending.ends_with(";")) {
            continue;  // keep reading: the statement is not finished
        }

        const std::string sql = pending;
        pending.clear();

        if (!interactive) {
            std::cout << "minidb> " << sql << '\n';
        }
        try {
            const QueryResult result = db.Execute(sql);
            PrintPlan(result);
            PrintRows(result);
            std::cout << result.message << '\n';
            PrintCost(result);
            std::cout << '\n';
        } catch (const QueryError& error) {
            std::cout << "Error: " << error.what() << "\n\n";
        }
    }

    if (!Trim(pending).empty()) {
        std::cout << "Aviso: se descartó una sentencia incompleta (falta ';').\n";
    }

    db.Flush();
    if (interactive) {
        std::cout << "\nBase de datos sincronizada. Hasta luego.\n";
    }
    return 0;
}

}  // namespace
}  // namespace minidb

int main(int argc, char** argv) {
    using namespace minidb;

    try {
        const std::filesystem::path config_path =
            (argc > 2) ? std::filesystem::path(argv[2]) : std::filesystem::path("minidb.conf");
        DatabaseConfig config = DatabaseConfig::Load(config_path);

        // An explicit path on the command line wins over the config file.
        if (argc > 1) {
            config.data_file = argv[1];
        }

        Database db(config.data_file, config.buffer_pool_frames, config.vectorized);

        const bool interactive = ::isatty(0) != 0;
        if (interactive) {
            std::cout << "MiniDB iniciado correctamente\n"
                      << "Archivo: " << std::filesystem::absolute(db.Path()).string() << '\n'
                      << "Tamaño de página: " << kPageSize << " bytes\n"
                      << "Frames del Buffer Pool: " << db.Pool().FrameCount() << '\n'
                      << "Ejecución: "
                      << (db.VectorizedEnabled() ? "vectorizada por lotes"
                                                 : "Volcano tupla a tupla")
                      << " (cámbiela con .vectorizado on|off)\n"
                      << (db.HasTable()
                              ? "Tabla existente: " + db.GetCatalog().TableName() + " (" +
                                    std::to_string(db.GetCatalog().RecordCount()) + " registros)\n"
                              : "Base de datos vacía: use CREATE TABLE para empezar\n")
                      << "Escriba .help para ver la ayuda\n\n";
        }

        return RunSession(std::cin, db, config_path, interactive);

    } catch (const StorageError& error) {
        std::cerr << "Error de almacenamiento: " << error.what() << '\n';
        return 2;
    } catch (const QueryError& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "Error inesperado: " << error.what() << '\n';
        return 3;
    }
}
