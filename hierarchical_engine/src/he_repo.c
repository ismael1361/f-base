/*
 * he_repo.c — Camada de PERSISTÊNCIA do motor SQLite (Clean Architecture).
 *
 * Executa SQL na tabela `nodes`: transações aninháveis, inserção,
 * deleção por prefixo, garantia de paths intermediários e leitura
 * auxiliar via extract_json. Nenhuma lógica de domínio aqui.
 *
 * Todas as funções usam statements do HeStmtCache (reutilizados entre
 * chamadas, preparados uma única vez em he_stmt_cache.c).
 */

#include <sqlite3ext.h>
/* sqlite3_api é definida em he_extension.c (SQLITE_EXTENSION_INIT1/INIT2).
 * As macros do sqlite3ext.h usam esta variável — declarar extern aqui. */
extern const sqlite3_api_routines *sqlite3_api;

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "he_types.h"
#include "he_repo.h"
#include "he_utils.h"

/* ===========================================================================
 * TRANSAÇÕES (suporte a transações aninhadas)
 * ===========================================================================
 * As funções de escrita (set_json/update_json/import_csv) abrem BEGIN
 * IMMEDIATE/COMMIT próprios. Quando chamadas DENTRO de um transaction()
 * externo (better-sqlite3, libsql), o BEGIN aninhado falharia. Estes
 * helpers só abrem/fecham a transação se a extensão for a dona dela
 * (autocommit == 1), preservando a atomicidade quando chamadas soltas.
 */

/// Inicia BEGIN IMMEDIATE somente se não houver transação externa ativa.
int he_repo_begin_write_tx(HeStmtCache *cache)
{
  if (sqlite3_get_autocommit(cache->db))
  {
    sqlite3_exec(cache->db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    return 1;
  }
  return 0;
}

/// Executa COMMIT somente se a transação foi iniciada por begin_write_tx.
void he_repo_end_write_tx(HeStmtCache *cache, int owned)
{
  if (owned)
    sqlite3_exec(cache->db, "COMMIT", NULL, NULL, NULL);
}

/* ===========================================================================
 * OPERAÇÕES DE ESCRITA
 * ===========================================================================
 */

/// Insere um nó na tabela usando o statement do cache (com revision).
/// keep_created=0 (default): insert_new SEM subquery — mais rápido.
/// keep_created=1: insert com subquery (preserva created de nó existente —
/// necessário no fast path de update de folhas).
void he_repo_insert_node_rev(HeStmtCache *cache, const char *path, int type,
                             const char *text_value, const char *revision,
                             int revision_nr, int keep_created)
{
  sqlite3_stmt *stmt = keep_created ? cache->insert : cache->insert_new;
  sqlite3_reset(stmt);
  sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);
  sqlite3_bind_int(stmt, 2, type);
  if (text_value)
  {
    sqlite3_bind_text(stmt, 3, text_value, -1, SQLITE_STATIC);
  }
  else
  {
    sqlite3_bind_null(stmt, 3);
  }
  sqlite3_bind_text(stmt, 4, revision, -1, SQLITE_STATIC);
  sqlite3_bind_int(stmt, 5, revision_nr);
  sqlite3_step(stmt);
}

/// Versão sem revision (usa rev_1 / nr=1 para compatibilidade)
void he_repo_insert_node(HeStmtCache *cache, const char *path, int type,
                         const char *text_value)
{
  // Gera revision UUID (alocado — caller/destrutor usa sqlite3_free)
  char rev_buf[UUID_STR_LEN];
  generate_uuid_v4(rev_buf, sizeof(rev_buf));
  char *revision = sqlite3_mprintf("%s", rev_buf);

  he_repo_insert_node_rev(cache, path, type, text_value, revision, 1, 0);
}

/// Obtém o revision_nr atual para um path (0 se não existir)
int he_repo_get_revision_nr(HeStmtCache *cache, const char *path)
{
  sqlite3_stmt *stmt = cache->revision;
  sqlite3_reset(stmt);
  sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);
  int nr = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW)
  {
    nr = sqlite3_column_int(stmt, 0);
  }
  return nr;
}

/// Obtém o type de um nó existente (-1 se não existir)
int he_repo_get_type(HeStmtCache *cache, const char *path)
{
  sqlite3_stmt *stmt = cache->type;
  sqlite3_reset(stmt);
  sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);
  int type = -1;
  if (sqlite3_step(stmt) == SQLITE_ROW)
  {
    type = sqlite3_column_int(stmt, 0);
  }
  return type;
}

/// Deleta todos os nós cujo path esteja no range [prefix, prefix+1)
/// Ex: delete_subtree("/users/100/") → deleta paths >= "/users/100/" e < "/users/1000"
void he_repo_delete_subtree(HeStmtCache *cache, const char *prefix)
{
  if (!prefix || prefix[0] == '\0')
    return;
  char upper[1024];
  strncpy(upper, prefix, sizeof(upper) - 1);
  upper[sizeof(upper) - 1] = '\0';
  size_t len = strlen(upper);
  if (len > 0)
    upper[len - 1]++;

  sqlite3_stmt *del = cache->delete_range;
  sqlite3_reset(del);
  sqlite3_bind_text(del, 1, prefix, -1, SQLITE_STATIC);
  sqlite3_bind_text(del, 2, upper, -1, SQLITE_STATIC);
  sqlite3_step(del);
}

/// Deleta o nó EXATO e seu subtree, com fronteira de segmento '/' —
/// diferente de delete_subtree(prefix), que varre [prefix, prefix+1) e
/// alcança chaves irmãs com prefixo de texto comum quando chamado sem o
/// '/' final (ex.: "user" → upper "uses" → apagava "username").
void he_repo_delete_exact_subtree(HeStmtCache *cache, const char *path)
{
  if (!path || path[0] == '\0')
    return;
  sqlite3_stmt *del = cache->delete_exact;
  sqlite3_reset(del);
  sqlite3_bind_text(del, 1, path, -1, SQLITE_STATIC);
  sqlite3_step(del);
}

/// Garante que todos os paths intermediários entre "/" e o documento existam.
/// Ex: doc_id = "users/100" → cria "/" (se não existir), "/users/", "/users/100/"
void he_repo_ensure_intermediate_paths(HeStmtCache *cache, const char *doc_id)
{
  sqlite3_stmt *check = cache->check;
  sqlite3_stmt *insert = cache->insert;

  // Garante que "/" existe
  sqlite3_reset(check);
  sqlite3_bind_text(check, 1, "/", -1, SQLITE_STATIC);
  if (sqlite3_step(check) != SQLITE_ROW)
  {
    he_repo_insert_node(cache, "/", 1, "{}");
  }

  char full_path[1024] = "/";
  char *copy = sqlite3_mprintf("%s", doc_id);
  if (!copy)
    return;

  // Tokeniza doc_id em um array
  char tokens[64][256];
  int token_count = 0;
  char *token = strtok(copy, "/");
  while (token && token_count < 64)
  {
    strncpy(tokens[token_count], token, 255);
    tokens[token_count][255] = '\0';
    token_count++;
    token = strtok(NULL, "/");
  }

  // Cria APENAS os paths INTERMEDIÁRIOS (exclui o último token = nome do documento)
  // Ex: doc_id "users/100" → cria "/users/" (NÃO cria "/users/100/")
  for (int i = 0; i < token_count - 1; i++)
  {
    size_t cur = strlen(full_path);
    snprintf(full_path + cur, sizeof(full_path) - cur, "%s/", tokens[i]);

    sqlite3_reset(check);
    sqlite3_bind_text(check, 1, full_path, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(check) != SQLITE_ROW)
    {
      he_repo_insert_node(cache, full_path, 1, "{}");
    }
  }

  sqlite3_free(copy);
}

/// Atualiza o text_value de um nó existente (container com inline children).
void he_repo_update_text(HeStmtCache *cache, const char *path,
                         const char *text_value, int is_dynamic)
{
  sqlite3_stmt *upd = cache->update_text;
  sqlite3_reset(upd);
  sqlite3_bind_text(upd, 1, text_value, -1,
                    is_dynamic ? free : SQLITE_STATIC);
  sqlite3_bind_text(upd, 2, path, -1, SQLITE_STATIC);
  sqlite3_step(upd);
}

/// Define text_value='{}' apenas se o nó estiver com text_value NULL
/// (usado para arrays vazios sem inline children).
void he_repo_update_empty(HeStmtCache *cache, const char *path)
{
  sqlite3_stmt *upd = cache->update_empty;
  sqlite3_reset(upd);
  sqlite3_bind_text(upd, 1, path, -1, SQLITE_STATIC);
  sqlite3_step(upd);
}

/* ===========================================================================
 * LEITURA AUXILIAR
 * ===========================================================================
 */

/// Executa `SELECT COALESCE(extract_json(?1), 'null')` e retorna a string
/// alocada (caller libera com sqlite3_free) ou NULL em falha de preparo.
char *he_repo_extract_json(HeStmtCache *cache, const char *path)
{
  sqlite3_stmt *stmt = cache->extract;
  sqlite3_reset(stmt);
  sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);

  char *out = NULL;
  if (sqlite3_step(stmt) == SQLITE_ROW)
  {
    const char *txt = (const char *)sqlite3_column_text(stmt, 0);
    out = txt ? sqlite3_mprintf("%s", txt) : NULL;
  }
  return out;
}

/* ===========================================================================
 * DEDUP DE SET (tabela doc_hashes)
 * ===========================================================================
 */

/// Lê (hash, revision) do último write do path. Só retorna 1 quando o nó
/// raiz AINDA tem a mesma revision (JOIN nodes) — se o documento foi
/// alterado/deletado fora do set (ex.: manual), o dedup é desarmado.
int he_repo_get_doc_hash(HeStmtCache *cache, const char *path,
                         char *hash_out, char *revision_out)
{
  sqlite3_stmt *stmt = cache->doc_hash_get;
  sqlite3_reset(stmt);
  sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);
  if (sqlite3_step(stmt) == SQLITE_ROW)
  {
    const char *h = (const char *)sqlite3_column_text(stmt, 0);
    const char *r = (const char *)sqlite3_column_text(stmt, 1);
    if (h)
    {
      strncpy(hash_out, h, 64);
      hash_out[64] = '\0';
    }
    else
      hash_out[0] = '\0';
    if (r)
    {
      strncpy(revision_out, r, UUID_STR_LEN - 1);
      revision_out[UUID_STR_LEN - 1] = '\0';
    }
    else
      revision_out[0] = '\0';
    return 1;
  }
  return 0;
}

/// Registra/atualiza (path, hash, revision) após um write completo.
void he_repo_set_doc_hash(HeStmtCache *cache, const char *path,
                          const char *hash, const char *revision)
{
  sqlite3_stmt *stmt = cache->doc_hash_set;
  sqlite3_reset(stmt);
  sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, hash, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 3, revision, -1, SQLITE_STATIC);
  sqlite3_step(stmt);
}

/// Remove a entrada (delete/update — invalida o dedup do path).
void he_repo_clear_doc_hash(HeStmtCache *cache, const char *path)
{
  sqlite3_stmt *stmt = cache->doc_hash_del;
  sqlite3_reset(stmt);
  sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);
  sqlite3_step(stmt);
}
