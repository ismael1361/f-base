#ifndef HE_CSV_H
#define HE_CSV_H

/*
 * he_csv.h — Declarações da camada CSV (src/he_csv.c).
 *
 * Parser/serializador RFC 4180 (escape, split, parse) e reconstrução
 * da árvore JSON a partir das linhas. Sem dependência de SQL — apenas
 * alocação via sqlite3_malloc (declarada no .c via sqlite3ext.h).
 */

#include <stddef.h>
#include "he_types.h"

/* Tipo opaco sqlite3_str (definido via sqlite3ext.h apenas nos .c) */
typedef struct sqlite3_str sqlite3_str;

/* ===========================================================================
 * ESCAPE
 * ===========================================================================
 */
/* Escapa um valor para CSV (RFC 4180). Retorna string alocada com
 * sqlite3_malloc — caller deve sqlite3_free(). */
char *he_csv_escape(const char *value);

/* Append de um valor CSV diretamente em um sqlite3_str (RFC 4180), sem
 * alocação intermediária — hot path do export_csv. */
void he_csv_escape_append(sqlite3_str *out, const char *value);

/* ===========================================================================
 * PARSE
 * ===========================================================================
 */
/* Parseia um CSV completo em array de CsvRow.
 * Suporta campos com quebra de linha entre aspas (RFC 4180).
 * Retorna o número de linhas (excluindo header) ou -1 em erro. */
int he_csv_parse(const char *csv_text, CsvRow **out_rows);

/* Libera array de CsvRow (count linhas) */
void he_csv_free_rows(CsvRow *rows, int count);

/* ===========================================================================
 * ÁRVORE JSON (CSV → JSON)
 * ===========================================================================
 */
/* Reconstrói uma árvore JSON a partir das linhas CSV usando stack.
 * Retorna yyjson_mut_val* (root) alocado em doc. */
void *he_csv_rows_to_tree(void *doc, CsvRow *rows, int count);

#endif /* HE_CSV_H */
