#ifndef HE_REPO_H
#define HE_REPO_H

/*
 * he_repo.h — Declarações da camada de PERSISTÊNCIA (src/he_repo.c).
 *
 * Única camada (junto com os controllers) que executa SQL na tabela
 * `nodes`. As demais camadas (mapper, query, csv, services) dependem
 * desta abstração em vez de tocar SQL diretamente.
 *
 * Todas as funções recebem HeStmtCache* — os statements são preparados
 * uma única vez (ver he_stmt_cache.c) e reutilizados entre chamadas,
 * eliminando o custo de prepare/finalize no hot path.
 */

#include <stddef.h>
#include "he_types.h"
#include "he_stmt_cache.h"

/* ===========================================================================
 * TRANSAÇÕES (aninháveis via transaction() externo)
 * ===========================================================================
 */
/* Inicia BEGIN IMMEDIATE somente se não houver transação externa ativa.
 * Retorna 1 se a transação foi iniciada por nós (caller deve commitar). */
int he_repo_begin_write_tx(HeStmtCache *cache);

/* Executa COMMIT somente se a transação foi iniciada por begin_write_tx. */
void he_repo_end_write_tx(HeStmtCache *cache, int owned);

/* ===========================================================================
 * DEDUP DE SET (tabela doc_hashes)
 * ===========================================================================
 */
/* Lê (hash, revision) do último write. Retorna 1 se existir E o nó raiz
 * ainda tiver a mesma revision (JOIN nodes); 0 caso contrário. hash_out
 * precisa de 65 bytes, revision_out de UUID_STR_LEN. */
int he_repo_get_doc_hash(HeStmtCache *cache, const char *path,
                         char *hash_out, char *revision_out);

/* Registra/atualiza (path, hash, revision) após um write completo. */
void he_repo_set_doc_hash(HeStmtCache *cache, const char *path,
                          const char *hash, const char *revision);

/* Remove a entrada (usado em delete/update — invalida o dedup). */
void he_repo_clear_doc_hash(HeStmtCache *cache, const char *path);

/* ===========================================================================
 * OPERAÇÕES DE ESCRITA
 * ===========================================================================
 */
/* Insere um nó na tabela usando o statement do cache (com revision).
 * keep_created=0 usa insert_new (SEM subquery COALESCE — otimizado para
 * caminhos onde o subtree já foi deletado: set/update-full/import).
 * keep_created=1 usa insert (com subquery — preserva created de nó
 * existente, necessário no fast path de update de folhas). */
void he_repo_insert_node_rev(HeStmtCache *cache, const char *path, int type,
                             const char *text_value, const char *revision,
                             int revision_nr, int keep_created);

/* Versão sem revision (usa rev_1 / nr=1 para compatibilidade) */
void he_repo_insert_node(HeStmtCache *cache, const char *path, int type,
                         const char *text_value);

/* Obtém o revision_nr atual para um path (0 se não existir) */
int he_repo_get_revision_nr(HeStmtCache *cache, const char *path);

/* Obtém o type de um nó existente (-1 se não existir) */
int he_repo_get_type(HeStmtCache *cache, const char *path);

/* Deleta todos os nós cujo path esteja no range [prefix, prefix+1) */
void he_repo_delete_subtree(HeStmtCache *cache, const char *prefix);

/* Deleta o nó EXATO (path = ?) e os descendentes delimitados por '/'
 * (path >= ?||'/' AND path < ?||'0'). Nunca alcança chaves irmãs que
 * apenas compartilham prefixo de texto (ex.: "user" vs "username").
 * Necessário para deleções de chave no fast path de update_json. */
void he_repo_delete_exact_subtree(HeStmtCache *cache, const char *path);

/* Garante que todos os paths intermediários entre "/" e o documento existam. */
void he_repo_ensure_intermediate_paths(HeStmtCache *cache, const char *doc_id);

/* Atualiza o text_value de um nó existente (container com inline children).
 * is_dynamic=1 indica que text_value foi alocado com malloc (libera após). */
void he_repo_update_text(HeStmtCache *cache, const char *path,
                         const char *text_value, int is_dynamic);

/* Define text_value='{}' apenas se o nó estiver com text_value NULL
 * (usado para arrays vazios sem inline children). */
void he_repo_update_empty(HeStmtCache *cache, const char *path);

/* ===========================================================================
 * LEITURA AUXILIAR
 * ===========================================================================
 */
/* Executa `SELECT COALESCE(extract_json(?1), 'null')` e retorna a string
 * alocada com sqlite3_malloc (caller libera com sqlite3_free) ou NULL em
 * falha de preparo. Retorna o literal "null" quando o path não existe. */
char *he_repo_extract_json(HeStmtCache *cache, const char *path);

#endif /* HE_REPO_H */
