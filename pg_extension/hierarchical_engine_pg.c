/*
 * =============================================================================
 * hierarchical_engine_pg.c — Controllers da extensão PostgreSQL
 * =============================================================================
 *
 * Camada de CONTROLLERS: entry points SQL (PG_FUNCTION_INFO_V1) que fazem o
 * parsing dos argumentos (text → char*) e delegam para os SERVICES
 * (src/pg_services.c), que por sua vez usam os UTILS (src/pg_utils.c).
 *
 * Arquitetura em camadas:
 *   src/pg_types.h                 — constantes e structs compartilhadas
 *   src/pg_utils.c / pg_utils.h    — utils: uuid, paths, wildcards, strings, valores
 *   src/pg_services.c / .h         — services: SPI, flatten/insert, merge, extract, query, CSV
 *   hierarchical_engine_pg.c       — controllers: funções SQL registradas
 *
 * Funções SQL registradas:
 *   set_json(doc_id, json_text)          → revision UUID (replace completo)
 *   update_json(doc_id, json_text)       → revision UUID (deep merge)
 *   extract_json(prefix)                 → JSON reconstruído (ou NULL)
 *   query_json(prefix, query_json)       → array JSON com filtros/ordenação
 *   export_csv(prefix)                   → CSV (RFC 4180)
 *   import_csv(prefix, csv_text)         → revision UUID
 *   ingest_json(doc_id, json_text)       → alias de set_json (compat)
 *
 * Build (Windows/MinGW-w64 — w64devkit):
 *   gcc -shared -O3 -I"pg_extension/pgsql/pgsql/include" \
 *       -I"pg_extension/pgsql/pgsql/include/server" \
 *       -I"pg_extension/pgsql/pgsql/include/internal" \
 *       -I"pg_extension" \
 *       pg_extension/hierarchical_engine_pg.c \
 *       pg_extension/src/pg_utils.c pg_extension/src/pg_services.c \
 *       pg_extension/yyjson.c \
 *       "pg_extension/pgsql/pgsql/lib/postgres.lib" \
 *       -o pg_extension/hierarchical_engine.dll
 *
 * =============================================================================
 */

#include "postgres.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "catalog/pg_type.h"
#include "src/pg_services.h"

PG_MODULE_MAGIC;

/* ===========================================================================
 * HELPERS DE CONTROLLER
 * ===========================================================================
 */

/* Converte o argumento argno (text) em cstring; erro se NULL. */
static char *arg_text(FunctionCallInfo fcinfo, int argno)
{
  if (PG_ARGISNULL(argno))
    ereport(ERROR,
            (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
             errmsg("Argument must not be NULL")));
  return text_to_cstring(PG_GETARG_TEXT_PP(argno));
}

/* ===========================================================================
 * FUNÇÃO: SET_JSON (JSON -> Nodes, replace completo)
 * ===========================================================================
 */
PG_FUNCTION_INFO_V1(set_json);
Datum set_json(PG_FUNCTION_ARGS)
{
  char *doc_id = arg_text(fcinfo, 0);
  char *json_str = arg_text(fcinfo, 1);

  char *result = service_set_json(doc_id, json_str, fcinfo->flinfo->fn_mcxt);
  if (!result)
    PG_RETURN_NULL();
  PG_RETURN_TEXT_P(cstring_to_text(result));
}

/* ===========================================================================
 * FUNÇÃO: UPDATE_JSON (JSON -> Nodes, deep merge)
 * ===========================================================================
 */
PG_FUNCTION_INFO_V1(update_json);
Datum update_json(PG_FUNCTION_ARGS)
{
  char *doc_id = arg_text(fcinfo, 0);
  char *json_str = arg_text(fcinfo, 1);

  char *result = service_update_json(doc_id, json_str, fcinfo->flinfo->fn_mcxt);
  if (!result)
    PG_RETURN_NULL();
  PG_RETURN_TEXT_P(cstring_to_text(result));
}

/* ===========================================================================
 * FUNÇÃO: EXTRACT_JSON (Nodes -> JSON)
 * ===========================================================================
 */
PG_FUNCTION_INFO_V1(extract_json);
Datum extract_json(PG_FUNCTION_ARGS)
{
  if (PG_ARGISNULL(0))
    PG_RETURN_NULL();
  char *input = text_to_cstring(PG_GETARG_TEXT_PP(0));
  if (!input || input[0] == '\0')
    PG_RETURN_NULL();

  char *result = service_extract_json(input, fcinfo->flinfo->fn_mcxt);
  if (!result)
    PG_RETURN_NULL();
  PG_RETURN_TEXT_P(cstring_to_text(result));
}

/* ===========================================================================
 * FUNÇÃO: QUERY_JSON
 * ===========================================================================
 */
PG_FUNCTION_INFO_V1(query_json);
Datum query_json(PG_FUNCTION_ARGS)
{
  if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
    PG_RETURN_NULL();
  char *prefix_input = text_to_cstring(PG_GETARG_TEXT_PP(0));
  char *query_str = text_to_cstring(PG_GETARG_TEXT_PP(1));

  char *result = service_query_json(prefix_input, query_str, fcinfo->flinfo->fn_mcxt);
  if (!result)
    PG_RETURN_NULL();
  PG_RETURN_TEXT_P(cstring_to_text(result));
}

/* ===========================================================================
 * FUNÇÃO: EXPORT_CSV (Nodes -> CSV)
 * ===========================================================================
 */
PG_FUNCTION_INFO_V1(export_csv);
Datum export_csv(PG_FUNCTION_ARGS)
{
  if (PG_ARGISNULL(0))
    PG_RETURN_TEXT_P(cstring_to_text(CSV_HEADER));
  char *input = text_to_cstring(PG_GETARG_TEXT_PP(0));
  if (!input || input[0] == '\0')
    PG_RETURN_TEXT_P(cstring_to_text(CSV_HEADER));

  char *result = service_export_csv(input, fcinfo->flinfo->fn_mcxt);
  if (!result)
    PG_RETURN_TEXT_P(cstring_to_text(CSV_HEADER));
  PG_RETURN_TEXT_P(cstring_to_text(result));
}

/* ===========================================================================
 * FUNÇÃO: IMPORT_CSV (CSV -> Nodes)
 * ===========================================================================
 */
PG_FUNCTION_INFO_V1(import_csv);
Datum import_csv(PG_FUNCTION_ARGS)
{
  char *prefix_input = arg_text(fcinfo, 0);
  char *csv_text = arg_text(fcinfo, 1);

  char *result = service_import_csv(prefix_input, csv_text, fcinfo->flinfo->fn_mcxt);
  if (!result)
    PG_RETURN_NULL();
  PG_RETURN_TEXT_P(cstring_to_text(result));
}

/* ===========================================================================
 * ALIAS: ingest_json → set_json (compatibilidade)
 * ===========================================================================
 */
PG_FUNCTION_INFO_V1(ingest_json);
Datum ingest_json(PG_FUNCTION_ARGS)
{
  return set_json(fcinfo);
}
