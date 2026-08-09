#ifndef HE_MAPPER_H
#define HE_MAPPER_H

/*
 * he_mapper.h — Declarações da camada de MAPPER (src/he_mapper.c).
 *
 * Converte JSON ↔ nós da tabela: flatten (JSON → nodes), reconstrução
 * de valor a partir do storage e deep merge. Não executa SQL direto —
 * usa a camada he_repo (statements do HeStmtCache) para persistência.
 */

#include <stddef.h>
#include "he_types.h"
#include "he_stmt_cache.h"

/* ===========================================================================
 * FLATTEN (JSON → Nodes)
 * ===========================================================================
 */
/* Achata um valor JSON em nós da tabela (SET mode).
 * parent_path SEMPRE termina com '/' (ex: "/users/100/"). */
void he_mapper_flatten_value(HeStmtCache *cache, void *node,
                             const char *parent_path, const char *revision,
                             int revision_nr, size_t max_inline_size);

/* Achata um array usando índices numéricos. */
void he_mapper_flatten_array_as_object(HeStmtCache *cache, void *arr,
                                       const char *parent_path,
                                       const char *revision, int revision_nr,
                                       size_t max_inline_size);

/* ===========================================================================
 * MERGE (update parcial)
 * ===========================================================================
 */
/* Faz deep merge de dois valores yyjson. Retorna um mut_val* alocado em
 * mut_doc (NULL quando update é null → deleção da chave). */
void *he_mapper_deep_merge(void *mut_doc, void *current, void *update);

#endif /* HE_MAPPER_H */
