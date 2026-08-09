#ifndef HE_QUERY_H
#define HE_QUERY_H

/*
 * he_query.h — Declarações da camada QUERY ENGINE (src/he_query.c).
 *
 * Executa consultas com filtros, ordenação e paginação sobre os dados
 * JSON reconstruídos (prefixo direto ou padrão wildcard multi-nível).
 * Sem estado global: a ordenação recebe o contexto via argumento.
 * Usa statements do HeStmtCache (zero prepares no hot path).
 */

#include <stddef.h>
#include "he_types.h"
#include "he_stmt_cache.h"

/* ===========================================================================
 * EXECUÇÃO DE QUERY
 * ===========================================================================
 */
/* Executa query_json(prefix, query_json) e retorna:
 *   - string alocada (sqlite3_malloc) com o array JSON de resultados
 *   - NULL com *err == NULL  → resultado SQL NULL (dados não existem)
 *   - NULL com *err != NULL  → erro (mensagem alocada, caller libera)
 * O caller deve sqlite3_free() a string de retorno. */
char *he_query_execute(HeStmtCache *cache, const char *prefix_input,
                       const char *query_str, char **err);

#endif /* HE_QUERY_H */
