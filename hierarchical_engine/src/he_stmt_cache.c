/*
 * he_stmt_cache.c — Cache de prepared statements por conexão.
 *
 * Elimina o custo de `sqlite3_prepare_v2` repetido: os statements fixos
 * do motor (insert, check, update_text, update_empty, delete_range,
 * revision, type, extract, scan, scan_path) são preparados uma única vez
 * por statement SQL externo e reutilizados entre chamadas das funções.
 *
 * MULTI-TABELA: um HeStmtRegistry (auxdata slot 0) guarda um HeStmtCache
 * por nome de tabela — `{table}_nodes`/`{table}_doc_hashes` (ou o par
 * default `nodes`/`doc_hashes`). O nome é VALIDADO antes de ser
 * interpolado no SQL (anti SQL injection). O ciclo de vida usa
 * sqlite3_get_auxdata/set_auxdata: o destrutor do registry finaliza
 * todos os caches quando o statement externo é finalizado.
 */

#include <sqlite3ext.h>
/* sqlite3_api é definida em he_extension.c (SQLITE_EXTENSION_INIT1/INIT2).
 * As macros do sqlite3ext.h usam esta variável — declarar extern aqui. */
extern const sqlite3_api_routines *sqlite3_api;

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "he_types.h"
#include "he_stmt_cache.h"

/* ===========================================================================
 * VALIDAÇÃO DE NOME DE TABELA
 * ===========================================================================
 */

/// Valida um nome de tabela: "" (default) ou [A-Za-z_][A-Za-z0-9_]{0,30}.
/// Retorna 1 se válido/default; 0 se inválido (NUNCA interpolar inválido).
int he_validate_table_name(const char *table_name)
{
  if (!table_name || table_name[0] == '\0')
    return 1; /* default */
  size_t len = strlen(table_name);
  if (len >= 32)
    return 0;
  if (!isalpha((unsigned char)table_name[0]) && table_name[0] != '_')
    return 0;
  for (const char *p = table_name + 1; *p; p++)
  {
    if (!isalnum((unsigned char)*p) && *p != '_')
      return 0;
  }
  return 1;
}

/* ===========================================================================
 * DESTRUTORES
 * ===========================================================================
 */

/// Finaliza os statements de um cache individual e libera.
void he_stmt_cache_destroy(void *ptr)
{
  HeStmtCache *cache = (HeStmtCache *)ptr;
  if (!cache)
    return;

  if (cache->insert)
    sqlite3_finalize(cache->insert);
  if (cache->insert_new)
    sqlite3_finalize(cache->insert_new);
  if (cache->check)
    sqlite3_finalize(cache->check);
  if (cache->update_text)
    sqlite3_finalize(cache->update_text);
  if (cache->update_empty)
    sqlite3_finalize(cache->update_empty);
  if (cache->delete_range)
    sqlite3_finalize(cache->delete_range);
  if (cache->delete_exact)
    sqlite3_finalize(cache->delete_exact);
  if (cache->revision)
    sqlite3_finalize(cache->revision);
  if (cache->type)
    sqlite3_finalize(cache->type);
  if (cache->extract)
    sqlite3_finalize(cache->extract);
  if (cache->scan)
    sqlite3_finalize(cache->scan);
  if (cache->scan_path)
    sqlite3_finalize(cache->scan_path);
  if (cache->doc_hash_get)
    sqlite3_finalize(cache->doc_hash_get);
  if (cache->doc_hash_set)
    sqlite3_finalize(cache->doc_hash_set);
  if (cache->doc_hash_del)
    sqlite3_finalize(cache->doc_hash_del);

  free(cache);
}

/// Destrutor do registry (auxdata slot 0): finaliza TODOS os caches.
static void he_stmt_registry_destroy(void *ptr)
{
  HeStmtRegistry *registry = (HeStmtRegistry *)ptr;
  if (!registry)
    return;
  for (int i = 0; i < registry->count; i++)
  {
    if (registry->caches[i])
      he_stmt_cache_destroy(registry->caches[i]);
  }
  free(registry);
}

/* ===========================================================================
 * PREPARO (por tabela)
 * ===========================================================================
 */

/// Monta o nome físico da tabela: "{table}_{suffix}" ou "{suffix}" (default).
/// Retorna string alocada com sqlite3_malloc (caller libera).
static char *he_table_name(const char *table_name, const char *suffix)
{
  if (table_name && table_name[0])
    return sqlite3_mprintf("%s_%s", table_name, suffix);
  return sqlite3_mprintf("%s", suffix);
}

/// Prepara um statement; em erro finaliza o cache e define *err.
static int prep_stmt(HeStmtCache *cache, sqlite3_stmt **out,
                     const char *sql, char **err)
{
  if (sqlite3_prepare_v2(cache->db, sql, -1, out, 0) != SQLITE_OK)
  {
    if (err)
      *err = sqlite3_mprintf("%s", sqlite3_errmsg(cache->db));
    he_stmt_cache_destroy(cache);
    return 0;
  }
  return 1;
}

/// Prepara todos os statements do motor para UMA tabela.
static HeStmtCache *he_stmt_cache_create(sqlite3 *db,
                                         const char *table_name, char **err)
{
  if (!he_validate_table_name(table_name))
  {
    if (err)
      *err = sqlite3_mprintf("Invalid table name");
    return NULL;
  }

  HeStmtCache *cache = (HeStmtCache *)calloc(1, sizeof(HeStmtCache));
  if (!cache)
  {
    if (err)
      *err = sqlite3_mprintf("OOM in stmt cache");
    return NULL;
  }
  cache->db = db;

  char *nodes = he_table_name(table_name, "nodes");
  char *hashes = he_table_name(table_name, "doc_hashes");
  if (!nodes || !hashes)
  {
    sqlite3_free(nodes);
    sqlite3_free(hashes);
    if (err)
      *err = sqlite3_mprintf("OOM in stmt cache");
    free(cache);
    return NULL;
  }

  int ok = 1;
  char *sql = NULL;

  sql = sqlite3_mprintf(
      "INSERT OR REPLACE INTO %s (path, type, text_value, "
      "created, modified, revision_nr, revision) "
      "VALUES (?1, ?2, ?3, "
      "  COALESCE((SELECT created FROM %s WHERE path = ?1), unixepoch()), "
      "  unixepoch(), ?5, ?4)",
      nodes, nodes);
  if (sql && prep_stmt(cache, &cache->insert, sql, err))
    ; /* ok */
  else
    ok = 0;
  sqlite3_free(sql);
  if (!ok)
    goto fail;

  sql = sqlite3_mprintf(
      "INSERT OR REPLACE INTO %s (path, type, text_value, "
      "created, modified, revision_nr, revision) "
      "VALUES (?1, ?2, ?3, unixepoch(), unixepoch(), ?5, ?4)",
      nodes);
  if (sql && prep_stmt(cache, &cache->insert_new, sql, err))
    ;
  else
    ok = 0;
  sqlite3_free(sql);
  if (!ok)
    goto fail;

  sql = sqlite3_mprintf("SELECT 1 FROM %s WHERE path = ?1", nodes);
  if (sql && prep_stmt(cache, &cache->check, sql, err))
    ;
  else
    ok = 0;
  sqlite3_free(sql);
  if (!ok)
    goto fail;

  sql = sqlite3_mprintf("UPDATE %s SET text_value = ?1 WHERE path = ?2", nodes);
  if (sql && prep_stmt(cache, &cache->update_text, sql, err))
    ;
  else
    ok = 0;
  sqlite3_free(sql);
  if (!ok)
    goto fail;

  sql = sqlite3_mprintf(
      "UPDATE %s SET text_value = '{}' WHERE path = ?1 AND text_value IS NULL",
      nodes);
  if (sql && prep_stmt(cache, &cache->update_empty, sql, err))
    ;
  else
    ok = 0;
  sqlite3_free(sql);
  if (!ok)
    goto fail;

  sql = sqlite3_mprintf("DELETE FROM %s WHERE path >= ?1 AND path < ?2", nodes);
  if (sql && prep_stmt(cache, &cache->delete_range, sql, err))
    ;
  else
    ok = 0;
  sqlite3_free(sql);
  if (!ok)
    goto fail;

  // Deleção EXATA + subtree delimitado por '/' (NUNCA alcança irmãs por
  // prefixo de texto: "user" não remove "username"). Usado no fast path
  // de update_json para remover chaves e containers sem colaterais.
  sql = sqlite3_mprintf(
      "DELETE FROM %s WHERE path = ?1 "
      "OR (path >= (?1 || '/') AND path < (?1 || '0'))",
      nodes);
  if (sql && prep_stmt(cache, &cache->delete_exact, sql, err))
    ;
  else
    ok = 0;
  sqlite3_free(sql);
  if (!ok)
    goto fail;

  sql = sqlite3_mprintf("SELECT revision_nr FROM %s WHERE path = ?1", nodes);
  if (sql && prep_stmt(cache, &cache->revision, sql, err))
    ;
  else
    ok = 0;
  sqlite3_free(sql);
  if (!ok)
    goto fail;

  sql = sqlite3_mprintf("SELECT type FROM %s WHERE path = ?1", nodes);
  if (sql && prep_stmt(cache, &cache->type, sql, err))
    ;
  else
    ok = 0;
  sqlite3_free(sql);
  if (!ok)
    goto fail;

  // O extract interno chama a função SQL extract_json — que precisa saber a
  // tabela. Para tabelas customizadas, embute o nome como LITERAL de string
  // (%Q — o nome já foi validado como identificador, sem risco de injeção).
  if (table_name && table_name[0])
  {
    sql = sqlite3_mprintf("SELECT COALESCE(extract_json(%Q, ?1), 'null')",
                          table_name);
    if (sql && prep_stmt(cache, &cache->extract, sql, err))
      ;
    else
      ok = 0;
    sqlite3_free(sql);
  }
  else
  {
    if (!prep_stmt(cache, &cache->extract,
                   "SELECT COALESCE(extract_json(?1), 'null')", err))
      ok = 0;
  }
  if (!ok)
    goto fail;

  sql = sqlite3_mprintf(
      "SELECT path, type, text_value FROM %s "
      "WHERE path >= ?1 AND path < ?2 ORDER BY path",
      nodes);
  if (sql && prep_stmt(cache, &cache->scan, sql, err))
    ;
  else
    ok = 0;
  sqlite3_free(sql);
  if (!ok)
    goto fail;

  sql = sqlite3_mprintf(
      "SELECT path FROM %s WHERE path >= ?1 AND path < ?2 ORDER BY path",
      nodes);
  if (sql && prep_stmt(cache, &cache->scan_path, sql, err))
    ;
  else
    ok = 0;
  sqlite3_free(sql);
  if (!ok)
    goto fail;

  sql = sqlite3_mprintf(
      "SELECT h.hash, h.revision FROM %s h "
      "JOIN %s n ON n.path = h.path AND n.revision = h.revision "
      "WHERE h.path = ?1",
      hashes, nodes);
  if (sql && prep_stmt(cache, &cache->doc_hash_get, sql, err))
    ;
  else
    ok = 0;
  sqlite3_free(sql);
  if (!ok)
    goto fail;

  sql = sqlite3_mprintf(
      "INSERT OR REPLACE INTO %s (path, hash, revision) "
      "VALUES (?1, ?2, ?3)",
      hashes);
  if (sql && prep_stmt(cache, &cache->doc_hash_set, sql, err))
    ;
  else
    ok = 0;
  sqlite3_free(sql);
  if (!ok)
    goto fail;

  sql = sqlite3_mprintf("DELETE FROM %s WHERE path = ?1", hashes);
  if (sql && prep_stmt(cache, &cache->doc_hash_del, sql, err))
    ;
  else
    ok = 0;
  sqlite3_free(sql);
  if (!ok)
    goto fail;

  sqlite3_free(nodes);
  sqlite3_free(hashes);
  return cache;

fail:
  sqlite3_free(nodes);
  sqlite3_free(hashes);
  return NULL;
}

/* ===========================================================================
 * ACESSO (registry via auxdata slot 0)
 * ===========================================================================
 */

/// Obtém (criando se necessário) o registry de tabelas da conexão.
static HeStmtRegistry *he_stmt_registry_get(sqlite3 *db,
                                            sqlite3_context *ctx, char **err)
{
  HeStmtRegistry *registry =
      (HeStmtRegistry *)sqlite3_get_auxdata(ctx, 0);
  if (registry)
    return registry;

  registry = (HeStmtRegistry *)calloc(1, sizeof(HeStmtRegistry));
  if (!registry)
  {
    if (err)
      *err = sqlite3_mprintf("OOM in stmt registry");
    return NULL;
  }
  registry->db = db;

  sqlite3_set_auxdata(ctx, 0, registry, he_stmt_registry_destroy);
  return registry;
}

/// Obtém (criando se necessário) o cache de statements da tabela.
/// table_name NULL/"" usa o par default (nodes/doc_hashes).
HeStmtCache *he_stmt_cache_get(sqlite3 *db, sqlite3_context *ctx,
                               const char *table_name, char **err)
{
  if (!he_validate_table_name(table_name))
  {
    if (err)
      *err = sqlite3_mprintf("Invalid table name");
    return NULL;
  }

  HeStmtRegistry *registry = he_stmt_registry_get(db, ctx, err);
  if (!registry)
    return NULL;

  // Procura a tabela no registro
  for (int i = 0; i < registry->count; i++)
  {
    if (strcmp(registry->table_names[i], table_name ? table_name : "") == 0)
      return registry->caches[i];
  }

  // Não existe → cria
  if (registry->count >= HE_MAX_TABLES)
  {
    if (err)
      *err = sqlite3_mprintf("Too many tables in one connection (max %d)",
                             HE_MAX_TABLES);
    return NULL;
  }

  HeStmtCache *cache = he_stmt_cache_create(db, table_name, err);
  if (!cache)
    return NULL;

  int idx = registry->count++;
  strncpy(registry->table_names[idx], table_name ? table_name : "", 31);
  registry->table_names[idx][31] = '\0';
  registry->caches[idx] = cache;
  return cache;
}
