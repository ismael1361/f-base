#ifndef HE_SERVICES_H
#define HE_SERVICES_H

/*
 * he_services.h — Declarações da camada de USE-CASES (src/he_services.c).
 *
 * Orquestra repo/mapper/query/csv para implementar as operações do
 * motor: set, update, extract, query, export/import CSV.
 *
 * Convenção de retorno (válida para todos):
 *   - não-NULL       → texto alocado com sqlite3_malloc (caller libera
 *                      com sqlite3_free — inclusive no destrutor do
 *                      sqlite3_result_text)
 *   - NULL, *err NULL → resultado SQL NULL (dados não existem)
 *   - NULL, *err !NULL → erro (mensagem alocada, caller libera)
 *
 * Todos recebem HeStmtCache* (statements preparados uma única vez) —
 * zero prepares no hot path.
 */

#include <stddef.h>
#include "he_types.h"
#include "he_stmt_cache.h"

/* ===========================================================================
 * OPERAÇÕES
 * ===========================================================================
 */
/* set_json(doc_id, json_text, max_inline_size) → revision (replace completo) */
char *he_set_json(HeStmtCache *cache, const char *doc_id, const char *json_str,
                  size_t max_inline_size, char **err);

/* update_json(doc_id, json_text, max_inline_size) → revision (deep merge) */
char *he_update_json(HeStmtCache *cache, const char *doc_id, const char *json_str,
                     size_t max_inline_size, char **err);

/* extract_json(prefix) → JSON reconstruído (ou NULL se não existir) */
char *he_extract_json(HeStmtCache *cache, const char *input, char **err);

/* query_json(prefix, query_json) → array JSON de resultados */
char *he_query_json(HeStmtCache *cache, const char *prefix, const char *query_str,
                    char **err);

/* export_csv(prefix) → CSV (RFC 4180) */
char *he_export_csv(HeStmtCache *cache, const char *prefix, char **err);

/* import_csv(prefix, csv_text, max_inline_size) → revision */
char *he_import_csv(HeStmtCache *cache, const char *prefix, const char *csv_text,
                    size_t max_inline_size, char **err);

#endif /* HE_SERVICES_H */
