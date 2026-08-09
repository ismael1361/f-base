/*
 * he_extension.c — Controllers + Entry point da extensão SQLite
 * (Clean Architecture: camada de interface/adaptação).
 *
 * Única camada que conversa com o runtime SQLite via sqlite3_context /
 * sqlite3_value: faz o parsing dos argumentos e delega o trabalho aos
 * use-cases (he_services). Também registra as funções SQL e o entry
 * point sqlite3_extension_init.
 *
 * Funções SQL registradas:
 *   register_table(table_name)                       → cria {t}_nodes + {t}_doc_hashes
 *   set_json([table,] doc_id, json_text [, max_inline])   → revision UUID
 *   update_json([table,] doc_id, json_text [, max_inline]) → revision UUID
 *   extract_json([table,] prefix [, options])        → JSON reconstruído (ou NULL)
 *   query_json([table,] prefix, query_json)          → array JSON
 *   export_csv([table,] prefix)                      → CSV (RFC 4180)
 *   import_csv([table,] prefix, csv_text [, max_inline]) → revision UUID
 *   ingest_json([table,] doc_id, json_text)          → alias de set_json (compat)
 *
 * O 1º argumento opcional `table` é identificado por NÃO começar com '/'
 * (doc_ids sempre começam com '/') e seleciona o par {t}_nodes/
 * {t}_doc_hashes. Sem ele, opera no par default (nodes/doc_hashes).
 */

#include <sqlite3ext.h>
SQLITE_EXTENSION_INIT1

#include "he_types.h"
#include "he_services.h"
#include "he_stmt_cache.h"

/* ===========================================================================
 * HELPERS DE CONTROLLER
 * ===========================================================================
 */

/// Entrega o resultado de um service ao contexto SQL de forma uniforme:
/// erro → sqlite3_result_error; NULL → SQL NULL; texto → sqlite3_free
/// como destrutor (os services alocam com sqlite3_malloc).
static void finish_result(sqlite3_context *context, char *result, char *err)
{
  if (err)
  {
    sqlite3_result_error(context, err, -1);
    sqlite3_free(err);
    return;
  }
  if (!result)
  {
    sqlite3_result_null(context);
    return;
  }
  sqlite3_result_text(context, result, -1, sqlite3_free);
}

/// Extrai max_inline_size do argumento opcional (posição 2+off, onde off é
/// o offset do doc_id quando um nome de tabela é passado como 1º argumento).
/// O binding N-API do libsql converte números JS para SQLITE_FLOAT,
/// então aceita INTEGER e FLOAT (também TEXT numérico via numeric_type).
static size_t parse_max_inline(int argc, sqlite3_value **argv, int off)
{
  int idx = 2 + off; /* sem table: 2 (doc,json[,inline]); com table: 3 */
  if (argc <= idx)
    return DEFAULT_MAX_INLINE_SIZE;

  int t = sqlite3_value_numeric_type(argv[idx]);
  if (t == SQLITE_INTEGER)
  {
    int val = sqlite3_value_int(argv[idx]);
    return val > 0 ? (size_t)val : 0;
  }
  if (t == SQLITE_FLOAT)
  {
    double d = sqlite3_value_double(argv[idx]);
    return d > 0 ? (size_t)d : 0;
  }
  return DEFAULT_MAX_INLINE_SIZE;
}

/// Detecta se o 1º argumento é um nome de tabela.
///
/// Regra de distinção não-ambígua:
///   - Doc_ids SEMPRE contêm '/' — ou começam com '/' ("/users/100") ou
///     são paths relativos legados ("users/100"). Um identificador de
///     tabela [A-Za-z_][A-Za-z0-9_]* NUNCA contém '/'.
///   - Logo, argv[0] é tabela IFF `he_validate_table_name(argv[0])` == 1
///     E argv[0] não começa com '/'. O C revalida o nome antes de qualquer
///     interpolação no SQL (anti SQL injection).
///
/// Retorna o nome da tabela ("" para o par default) e define *off com o
/// offset do doc_id dentro de argv.
static const char *he_parse_table(sqlite3_value **argv, int *off)
{
  const char *v0 = (const char *)sqlite3_value_text(argv[0]);
  if (v0 && v0[0] != '/' && he_validate_table_name(v0))
  {
    *off = 1;
    return v0;
  }
  *off = 0;
  return "";
}

/* ===========================================================================
 * FUNÇÃO: SET_JSON (JSON -> Nodes, replace completo)
 * ===========================================================================
 */
static void set_json_func(sqlite3_context *context, int argc, sqlite3_value **argv)
{
  int off;
  const char *table = he_parse_table(argv, &off);

  // set_json([table,] doc_id, json_text [, max_inline_size])
  if (argc < off + 2 || argc > off + 3)
  {
    sqlite3_result_error(context,
                         "Usage: set_json([table,] doc_id, json_text [, max_inline_size])", -1);
    return;
  }

  const char *doc_id = (const char *)sqlite3_value_text(argv[off]);
  const char *json_str = (const char *)sqlite3_value_text(argv[off + 1]);
  if (!doc_id || !json_str)
  {
    sqlite3_result_error(context, "Arguments must not be NULL", -1);
    return;
  }

  size_t max_inline_size = parse_max_inline(argc, argv, off);

  char *err = NULL;
  HeStmtCache *cache = he_stmt_cache_get(sqlite3_context_db_handle(context),
                                         context, table, &err);
  if (!cache)
  {
    finish_result(context, NULL, err);
    return;
  }
  char *result = he_set_json(cache, doc_id, json_str, max_inline_size, &err);
  finish_result(context, result, err);
}

/* ===========================================================================
 * FUNÇÃO: UPDATE_JSON (JSON -> Nodes, merge)
 * ===========================================================================
 */
static void update_json_func(sqlite3_context *context, int argc, sqlite3_value **argv)
{
  int off;
  const char *table = he_parse_table(argv, &off);

  // update_json([table,] doc_id, json_text [, max_inline_size])
  if (argc < off + 2 || argc > off + 3)
  {
    sqlite3_result_error(context,
                         "Usage: update_json([table,] doc_id, json_text [, max_inline_size])", -1);
    return;
  }

  const char *doc_id = (const char *)sqlite3_value_text(argv[off]);
  const char *json_str = (const char *)sqlite3_value_text(argv[off + 1]);
  if (!doc_id || !json_str)
  {
    sqlite3_result_error(context, "Arguments must not be NULL", -1);
    return;
  }

  size_t max_inline_size = parse_max_inline(argc, argv, off);

  char *err = NULL;
  HeStmtCache *cache = he_stmt_cache_get(sqlite3_context_db_handle(context),
                                         context, table, &err);
  if (!cache)
  {
    finish_result(context, NULL, err);
    return;
  }
  char *result = he_update_json(cache, doc_id, json_str, max_inline_size, &err);
  finish_result(context, result, err);
}

/* ===========================================================================
 * FUNÇÃO: EXTRACT_JSON (Nodes -> JSON)
 * ===========================================================================
 */
static void extract_json_func(sqlite3_context *context, int argc, sqlite3_value **argv)
{
  int off;
  const char *table = he_parse_table(argv, &off);

  // extract_json([table,] prefix [, options_json])
  if (argc < off + 1 || argc > off + 2)
  {
    sqlite3_result_error(context,
                         "Usage: extract_json([table,] prefix [, options_json])", -1);
    return;
  }

  const char *input = (const char *)sqlite3_value_text(argv[off]);
  if (!input || input[0] == '\0')
  {
    sqlite3_result_null(context);
    return;
  }

  // O 2º argumento (include/exclude) é aceito por compatibilidade,
  // porém ignorado — a filtragem é feita em query_json (comportamento
  // histórico da extensão).

  char *err = NULL;
  HeStmtCache *cache = he_stmt_cache_get(sqlite3_context_db_handle(context),
                                         context, table, &err);
  if (!cache)
  {
    finish_result(context, NULL, err);
    return;
  }
  char *result = he_extract_json(cache, input, &err);
  finish_result(context, result, err);
}

/* ===========================================================================
 * FUNÇÃO: QUERY_JSON
 * ===========================================================================
 */
static void query_json_func(sqlite3_context *context, int argc, sqlite3_value **argv)
{
  int off;
  const char *table = he_parse_table(argv, &off);

  // query_json([table,] prefix, query_json)
  if (argc != off + 2)
  {
    sqlite3_result_error(context,
                         "Usage: query_json([table,] prefix, query_json)", -1);
    return;
  }

  const char *prefix_input = (const char *)sqlite3_value_text(argv[off]);
  const char *query_str = (const char *)sqlite3_value_text(argv[off + 1]);

  if (!prefix_input || !query_str)
  {
    sqlite3_result_null(context);
    return;
  }

  char *err = NULL;
  HeStmtCache *cache = he_stmt_cache_get(sqlite3_context_db_handle(context),
                                         context, table, &err);
  if (!cache)
  {
    finish_result(context, NULL, err);
    return;
  }
  char *result = he_query_json(cache, prefix_input, query_str, &err);
  finish_result(context, result, err);
}

/* ===========================================================================
 * FUNÇÃO: EXPORT_CSV (Nodes -> CSV)
 * ===========================================================================
 */
static void export_csv_func(sqlite3_context *context, int argc,
                            sqlite3_value **argv)
{
  int off;
  const char *table = he_parse_table(argv, &off);

  // export_csv([table,] prefix)
  if (argc != off + 1)
  {
    sqlite3_result_error(context, "Usage: export_csv([table,] prefix)", -1);
    return;
  }

  const char *input = (const char *)sqlite3_value_text(argv[off]);
  if (!input || input[0] == '\0')
  {
    sqlite3_result_text(context, CSV_HEADER, -1, SQLITE_STATIC);
    return;
  }

  char *err = NULL;
  HeStmtCache *cache = he_stmt_cache_get(sqlite3_context_db_handle(context),
                                         context, table, &err);
  if (!cache)
  {
    finish_result(context, NULL, err);
    return;
  }
  char *result = he_export_csv(cache, input, &err);
  finish_result(context, result, err);
}

/* ===========================================================================
 * FUNÇÃO: IMPORT_CSV (CSV -> Nodes)
 * ===========================================================================
 */
static void import_csv_func(sqlite3_context *context, int argc,
                            sqlite3_value **argv)
{
  int off;
  const char *table = he_parse_table(argv, &off);

  // import_csv([table,] prefix, csv_text [, max_inline_size])
  if (argc < off + 2 || argc > off + 3)
  {
    sqlite3_result_error(context,
                         "Usage: import_csv([table,] prefix, csv_text [, max_inline_size])", -1);
    return;
  }

  const char *prefix_input = (const char *)sqlite3_value_text(argv[off]);
  const char *csv_text = (const char *)sqlite3_value_text(argv[off + 1]);
  if (!prefix_input || !csv_text)
  {
    sqlite3_result_error(context, "Arguments must not be NULL", -1);
    return;
  }

  size_t max_inline_size = parse_max_inline(argc, argv, off);

  char *err = NULL;
  HeStmtCache *cache = he_stmt_cache_get(sqlite3_context_db_handle(context),
                                         context, table, &err);
  if (!cache)
  {
    finish_result(context, NULL, err);
    return;
  }
  char *result = he_import_csv(cache, prefix_input, csv_text, max_inline_size, &err);
  finish_result(context, result, err);
}

/* ===========================================================================
 * FUNÇÃO: REGISTER_TABLE (DDL por tabela)
 * ===========================================================================
 * register_table('users') → CREATE TABLE IF NOT EXISTS users_nodes (...) +
 * users_doc_hashes (...). Idempotente. O nome é validado como identificador
 * (anti SQL injection) antes de ser interpolado.
 */
static void register_table_func(sqlite3_context *context, int argc,
                                sqlite3_value **argv)
{
  if (argc != 1)
  {
    sqlite3_result_error(context, "Usage: register_table(table_name)", -1);
    return;
  }

  const char *name = (const char *)sqlite3_value_text(argv[0]);
  if (!name || !name[0] || !he_validate_table_name(name))
  {
    sqlite3_result_error(context, "Invalid table name", -1);
    return;
  }

  sqlite3 *db = sqlite3_context_db_handle(context);
  char *nodes = sqlite3_mprintf("%s_nodes", name);
  char *hashes = sqlite3_mprintf("%s_doc_hashes", name);
  char *ddl = sqlite3_mprintf(
      "CREATE TABLE IF NOT EXISTS %s ("
      "  path TEXT PRIMARY KEY,"
      "  type INTEGER NOT NULL,"
      "  text_value TEXT,"
      "  created INTEGER NOT NULL,"
      "  modified INTEGER NOT NULL,"
      "  revision_nr INTEGER NOT NULL,"
      "  revision TEXT NOT NULL"
      ") WITHOUT ROWID;"
      "CREATE TABLE IF NOT EXISTS %s ("
      "  path TEXT PRIMARY KEY,"
      "  hash TEXT NOT NULL,"
      "  revision TEXT NOT NULL"
      ") WITHOUT ROWID;",
      nodes, hashes);

  char *init_err = NULL;
  int rc = ddl ? sqlite3_exec(db, ddl, NULL, NULL, &init_err) : SQLITE_NOMEM;
  sqlite3_free(ddl);
  sqlite3_free(nodes);
  sqlite3_free(hashes);

  if (rc != SQLITE_OK)
  {
    sqlite3_result_error(context,
                         init_err ? init_err : sqlite3_errmsg(db), -1);
    sqlite3_free(init_err);
    return;
  }
  sqlite3_free(init_err);
  sqlite3_result_int(context, 1);
}

/* ===========================================================================
 * REGISTRO DA EXTENSÃO
 * ===========================================================================
 */
#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_extension_init(sqlite3 *db, char **pzErrMsg, const sqlite3_api_routines *pApi)
{
  SQLITE_EXTENSION_INIT2(pApi);

  sqlite3_create_function(db, "register_table", 1, SQLITE_UTF8, 0, register_table_func, 0, 0);
  sqlite3_create_function(db, "set_json", -1, SQLITE_UTF8, 0, set_json_func, 0, 0);
  sqlite3_create_function(db, "update_json", -1, SQLITE_UTF8, 0, update_json_func, 0, 0);
  sqlite3_create_function(db, "extract_json", -1, SQLITE_UTF8, 0, extract_json_func, 0, 0);
  sqlite3_create_function(db, "query_json", -1, SQLITE_UTF8, 0, query_json_func, 0, 0);
  sqlite3_create_function(db, "export_csv", -1, SQLITE_UTF8, 0, export_csv_func, 0, 0);
  sqlite3_create_function(db, "import_csv", -1, SQLITE_UTF8, 0, import_csv_func, 0, 0);

  // Mantém ingest_json como alias para compatibilidade reversa
  sqlite3_create_function(db, "ingest_json", -1, SQLITE_UTF8, 0, set_json_func, 0, 0);

  return SQLITE_OK;
}
