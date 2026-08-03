/*
 * =============================================================================
 * hierarchical_engine_pg.c — Extensão C para PostgreSQL (embedded-postgres)
 * =============================================================================
 *
 * Porta o motor hierárquico JSON↔tabela do SQLite (hierarchical_engine.c)
 * para PostgreSQL usando a SPI (Server Programming Interface).
 *
 * A extensão roda DENTRO do processo do servidor postgres (como no SQLite
 * que roda no processo da aplicação), eliminando IPC e round-trips.
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
 *       pg_extension/hierarchical_engine_pg.c pg_extension/yyjson.c \
 *       "pg_extension/pgsql/pgsql/lib/postgres.lib" \
 *       -o pg_extension/hierarchical_engine.dll
 *
 * =============================================================================
 */

#include "postgres.h"
#include "fmgr.h"
#include "executor/spi.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "catalog/pg_type.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "port.h" /* pg_strong_random */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>
#include "yyjson.h"

PG_MODULE_MAGIC;

/* Forward declaration (definida na seção CSV — usada antes em extract_json). */
static text *fn_text_copy(const char *str, MemoryContext fn_mcxt);

/* ===========================================================================
 * TYPE SYSTEM (alinhado com o MDE JavaScript em ivipbase)
 * ===========================================================================
 * TYPE_EMPTY=0, TYPE_OBJECT=1, TYPE_ARRAY=2, TYPE_NUMBER=3,
 * TYPE_BOOLEAN=4, TYPE_STRING=5, TYPE_DATETIME=6, TYPE_BIGINT=7,
 * TYPE_BINARY=8, TYPE_REFERENCE=9
 *
 * Convenção de paths:
 *   - Containers (OBJECT, ARRAY) terminam com '/'  → ex: "/users/100/"
 *   - Primitivos NÃO terminam com '/'               → ex: "/users/100/name"
 *
 * text_value:
 *   - Para containers: JSON com os filhos inline (ex: '{"name":"John","age":30}')
 *   - Para STRING: valor JSON-escapeado (ex: '"John"')
 *   - Para NUMBER: representação textual (ex: "30" ou "3.14")
 *   - Para BOOLEAN: "true" ou "false"
 */
#define TYPE_EMPTY 0
#define TYPE_OBJECT 1
#define TYPE_ARRAY 2
#define TYPE_NUMBER 3
#define TYPE_BOOLEAN 4
#define TYPE_STRING 5
#define TYPE_DATETIME 6
#define TYPE_BIGINT 7
#define TYPE_BINARY 8
#define TYPE_REFERENCE 9

#define DEFAULT_MAX_INLINE_SIZE 0
#define UUID_STR_LEN 37

/* ===========================================================================
 * UUID GENERATION (v4) — via pg_strong_random (seguro e sem dependências)
 * ===========================================================================
 */
static void generate_uuid_v4(char *buf, size_t buf_size)
{
  if (buf_size < UUID_STR_LEN)
  {
    if (buf_size > 0)
      buf[0] = '\0';
    return;
  }
  unsigned char bytes[16];
  if (!pg_strong_random((char *)bytes, sizeof(bytes)))
  {
    /* Fallback não criptográfico: clock + contador (não usa gettimeofday) */
    static uint64_t counter = 0;
    uint64_t t = (uint64_t)time(NULL) * 1000000 + (counter++ & 0xFFFFF);
    memcpy(bytes, &t, sizeof(t));
    uintptr_t addr = (uintptr_t)&buf;
    memcpy(bytes + 8, &addr, sizeof(addr));
  }
  bytes[6] = (bytes[6] & 0x0f) | 0x40; /* version 4 */
  bytes[8] = (bytes[8] & 0x3f) | 0x80; /* variant 10xx */
  snprintf(buf, buf_size,
           "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
           bytes[0], bytes[1], bytes[2], bytes[3],
           bytes[4], bytes[5], bytes[6], bytes[7],
           bytes[8], bytes[9], bytes[10], bytes[11],
           bytes[12], bytes[13], bytes[14], bytes[15]);
}

/* ===========================================================================
 * PATH NORMALIZATION
 * ===========================================================================
 */
static void normalize_path(const char *input, char *out_path, size_t out_size)
{
  if (!input || !*input)
  {
    snprintf(out_path, out_size, "/");
    return;
  }

  size_t len = strlen(input);
  if (len >= out_size)
    len = out_size - 1;

  while (len > 1 && input[len - 1] == '/')
    len--;

  if (len == 0)
  {
    snprintf(out_path, out_size, "/");
    return;
  }

  if (input[0] == '/')
  {
    if (input[len - 1] == '/')
      snprintf(out_path, out_size, "%.*s", (int)len, input);
    else
      snprintf(out_path, out_size, "%.*s/", (int)len, input);
  }
  else
  {
    snprintf(out_path, out_size, "/%.*s/", (int)len, input);
  }
}

static void path_without_trailing_slash(const char *normalized, char *out, size_t out_size)
{
  size_t len = strlen(normalized);
  if (len > 1 && normalized[len - 1] == '/')
    len--;
  if (len >= out_size)
    len = out_size - 1;
  memcpy(out, normalized, len);
  out[len] = '\0';
}

/* ===========================================================================
 * WILDCARD PATTERN MATCHING (para query/update multi-nível)
 * ===========================================================================
 */
#define MAX_WC_SEGMENTS 64

typedef struct
{
  char segments[MAX_WC_SEGMENTS][256];
  bool is_wildcard[MAX_WC_SEGMENTS];
  int count;
} WildcardPattern;

static bool path_has_wildcard(const char *path)
{
  if (!path)
    return false;
  return strchr(path, '*') != NULL || strchr(path, '$') != NULL;
}

static void wildcard_parse(const char *path, WildcardPattern *pattern)
{
  memset(pattern, 0, sizeof(WildcardPattern));
  if (!path || !*path)
    return;

  const char *p = path;
  if (*p == '/')
    p++;

  while (*p && pattern->count < MAX_WC_SEGMENTS)
  {
    const char *end = strchr(p, '/');
    size_t len = end ? (size_t)(end - p) : strlen(p);

    if (len > 0)
    {
      size_t cp = len < 255 ? len : 255;
      memcpy(pattern->segments[pattern->count], p, cp);
      pattern->segments[pattern->count][cp] = '\0';

      pattern->is_wildcard[pattern->count] =
          (cp == 1 && p[0] == '*') || (cp > 0 && p[0] == '$');

      pattern->count++;
    }

    if (!end)
      break;
    p = end + 1;
  }
}

static bool wildcard_matches(const char *path, const WildcardPattern *pattern)
{
  if (!path || pattern->count == 0)
    return false;

  const char *p = path;
  if (*p == '/')
    p++;

  int seg_idx = 0;
  while (*p && seg_idx < pattern->count)
  {
    const char *end = strchr(p, '/');
    size_t len = end ? (size_t)(end - p) : strlen(p);

    if (len == 0)
    {
      p = end ? end + 1 : p + strlen(p);
      continue;
    }

    if (!pattern->is_wildcard[seg_idx])
    {
      if (strlen(pattern->segments[seg_idx]) != len ||
          strncmp(pattern->segments[seg_idx], p, len) != 0)
      {
        return false;
      }
    }

    seg_idx++;
    p = end ? end + 1 : p + strlen(p);
  }

  while (*p == '/')
    p++;

  return seg_idx == pattern->count && *p == '\0';
}

static void wildcard_fixed_prefix(const char *pattern_path, char *out, size_t out_size)
{
  if (!pattern_path || !*pattern_path)
  {
    snprintf(out, out_size, "/");
    return;
  }

  const char *p = pattern_path;
  if (*p == '/')
    p++;

  const char *first_wc = NULL;

  while (*p)
  {
    const char *end = strchr(p, '/');
    size_t len = end ? (size_t)(end - p) : strlen(p);

    if (len > 0)
    {
      if ((len == 1 && *p == '*') || *p == '$')
      {
        first_wc = (p > pattern_path) ? p - 1 : pattern_path;
        break;
      }
    }

    if (!end)
      break;
    p = end + 1;
  }

  if (first_wc)
  {
    size_t n = (size_t)(first_wc - pattern_path);
    char tmp[1024];
    size_t tmp_len = 0;

    if (n == 0 || pattern_path[0] != '/')
    {
      tmp[0] = '/';
      tmp_len = 1;
    }

    if (n > 0)
    {
      if (n >= sizeof(tmp) - tmp_len - 1)
        n = sizeof(tmp) - tmp_len - 2;
      memcpy(tmp + tmp_len, pattern_path, n);
      tmp_len += n;
    }

    if (tmp_len == 0 || tmp[tmp_len - 1] != '/')
    {
      if (tmp_len < sizeof(tmp) - 1)
        tmp[tmp_len++] = '/';
    }

    tmp[tmp_len] = '\0';
    strncpy(out, tmp, out_size - 1);
    out[out_size - 1] = '\0';
  }
  else
  {
    normalize_path(pattern_path, out, out_size);
  }
}

static void make_upper_bound(const char *prefix, char *upper, size_t up_size)
{
  strncpy(upper, prefix, up_size - 1);
  upper[up_size - 1] = '\0';
  size_t len = strlen(upper);
  if (len > 0)
    upper[len - 1]++;
}

/* ===========================================================================
 * HELPERS DE STRING JSON
 * ===========================================================================
 */
/* Escapa uma string raw para JSON com aspas. Retorna o tamanho necessário
 * (incluindo as 2 aspas e o NUL). Se buf == NULL ou buf_size < need, apenas
 * calcula o tamanho — permite alocação exata sem truncamento. */
static size_t json_escape_string(char *buf, size_t buf_size, const char *raw)
{
  size_t need = 3; /* 2 aspas + NUL */
  for (const char *p = raw; *p; p++)
  {
    switch (*p)
    {
    case '"':
    case '\\':
    case '\n':
    case '\r':
    case '\t':
      need += 2;
      break;
    default:
      need += 1;
      break;
    }
  }
  if (!buf || buf_size < need)
    return need;

  size_t j = 1;
  buf[0] = '"';
  for (const char *p = raw; *p; p++)
  {
    switch (*p)
    {
    case '"':
      buf[j++] = '\\';
      buf[j++] = '"';
      break;
    case '\\':
      buf[j++] = '\\';
      buf[j++] = '\\';
      break;
    case '\n':
      buf[j++] = '\\';
      buf[j++] = 'n';
      break;
    case '\r':
      buf[j++] = '\\';
      buf[j++] = 'r';
      break;
    case '\t':
      buf[j++] = '\\';
      buf[j++] = 't';
      break;
    default:
      buf[j++] = *p;
      break;
    }
  }
  buf[j++] = '"';
  buf[j] = '\0';
  return need;
}

/* Aloca e escapa uma string raw para JSON (palloc'd, nunca trunca). */
static char *json_escape_string_alloc(const char *raw)
{
  size_t need = json_escape_string(NULL, 0, raw);
  char *buf = palloc(need);
  json_escape_string(buf, need, raw);
  return buf;
}

/* Desescapa uma string JSON com aspas em buffer palloc'd.
 * O resultado nunca é maior que a entrada (escaping só cresce), então
 * palloc(strlen) é sempre suficiente — sem truncamento. */
static char *json_unescape_string_alloc(const char *quoted)
{
  size_t len = strlen(quoted);
  if (len < 2 || quoted[0] != '"' || quoted[len - 1] != '"')
    return pstrdup(quoted);

  char *buf = palloc(len);
  size_t i, j;
  for (i = 1, j = 0; i < len - 1; i++)
  {
    if (quoted[i] == '\\' && i + 1 < len - 1)
    {
      i++;
      switch (quoted[i])
      {
      case '"':
        buf[j++] = '"';
        break;
      case '\\':
        buf[j++] = '\\';
        break;
      case 'n':
        buf[j++] = '\n';
        break;
      case 'r':
        buf[j++] = '\r';
        break;
      case 't':
        buf[j++] = '\t';
        break;
      default:
        buf[j++] = '\\';
        buf[j++] = quoted[i];
        break;
      }
    }
    else
    {
      buf[j++] = quoted[i];
    }
  }
  buf[j] = '\0';
  return buf;
}

/* ===========================================================================
 * HELPERS DE BANCO (SPI — roda no processo do servidor)
 * ===========================================================================
 */

/* INSERT OR REPLACE no PostgreSQL: ON CONFLICT (path) DO UPDATE */
#define SQL_INSERT_NODE                                                                   \
  "INSERT INTO nodes (path, type, text_value, created, modified, revision_nr, revision) " \
  "VALUES ($1, $2, $3, "                                                                  \
  "  COALESCE((SELECT created FROM nodes WHERE path = $1), "                              \
  "           EXTRACT(EPOCH FROM now())::bigint), "                                       \
  "  EXTRACT(EPOCH FROM now())::bigint, $5, $4) "                                         \
  "ON CONFLICT (path) DO UPDATE SET "                                                     \
  "  type = EXCLUDED.type, "                                                              \
  "  text_value = EXCLUDED.text_value, "                                                  \
  "  modified = EXCLUDED.modified, "                                                      \
  "  revision_nr = EXCLUDED.revision_nr, "                                                \
  "  revision = EXCLUDED.revision"

#define SQL_CHECK_EXISTS "SELECT 1 FROM nodes WHERE path = $1"
#define SQL_UPDATE_TEXT "UPDATE nodes SET text_value = $1 WHERE path = $2"
#define SQL_UPDATE_EMPTY "UPDATE nodes SET text_value = '{}' WHERE path = $1 AND text_value IS NULL"
#define SQL_DELETE_RANGE "DELETE FROM nodes WHERE path >= $1 AND path < $2"
#define SQL_SELECT_NR "SELECT revision_nr FROM nodes WHERE path = $1"
#define SQL_SELECT_RANGE \
  "SELECT path, type, text_value FROM nodes WHERE path >= $1 AND path < $2 ORDER BY path"
#define SQL_SCAN_PATHS \
  "SELECT path FROM nodes WHERE path >= $1 AND path < $2 ORDER BY path"
#define SQL_EXTRACT_JSON "SELECT COALESCE(extract_json($1), 'null')"

static SPIPlanPtr spi_prepare_checked(const char *sql, int nargs, Oid *argtypes,
                                      const char *what)
{
  SPIPlanPtr plan = SPI_prepare(sql, nargs, argtypes);
  if (plan == NULL)
    ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("SPI_prepare falhou em %s: %s", what,
                    SPI_result_code_string(SPI_result))));
  return plan;
}

/* Insere um nó usando o plano preparado (com revision) */
static void insert_node_rev(SPIPlanPtr stmt, const char *path, int type,
                            const char *text_value, const char *revision,
                            int revision_nr)
{
  Datum values[5];
  char nulls[5] = "    ";
  values[0] = CStringGetTextDatum(path);
  values[1] = Int32GetDatum(type);
  if (text_value)
    values[2] = CStringGetTextDatum(text_value);
  else
    nulls[2] = 'n';
  values[3] = CStringGetTextDatum(revision);
  values[4] = Int32GetDatum(revision_nr);

  if (SPI_execute_plan(stmt, values, nulls, false, 0) != SPI_OK_INSERT)
    ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("falha no INSERT do nó %s: %s", path,
                    SPI_result_code_string(SPI_result))));
}

static void insert_node(SPIPlanPtr stmt, const char *path, int type, const char *text_value)
{
  insert_node_rev(stmt, path, type, text_value, "rev_1", 1);
}

/* Obtém o revision_nr atual para um path (0 se não existir) */
static int get_current_revision_nr(const char *path)
{
  int nr = 0;
  Oid argtypes[1] = {TEXTOID};
  Datum values[1];
  char nulls[1] = " ";
  values[0] = CStringGetTextDatum(path);

  if (SPI_execute_with_args(SQL_SELECT_NR, 1, argtypes, values, nulls, true, 1) ==
      SPI_OK_SELECT)
  {
    if (SPI_processed > 0 && SPI_tuptable != NULL)
    {
      char *val = SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1);
      if (val)
        nr = atoi(val);
    }
  }
  return nr;
}

/* Deleta todos os nós cujo path esteja no range [prefix, prefix+1) */
static void delete_subtree(const char *prefix)
{
  if (!prefix || prefix[0] == '\0')
    return;
  char upper[1024];
  strncpy(upper, prefix, sizeof(upper) - 1);
  upper[sizeof(upper) - 1] = '\0';
  size_t len = strlen(upper);
  if (len > 0)
    upper[len - 1]++;

  Oid argtypes[2] = {TEXTOID, TEXTOID};
  Datum values[2];
  char nulls[2] = "  ";
  values[0] = CStringGetTextDatum(prefix);
  values[1] = CStringGetTextDatum(upper);

  if (SPI_execute_with_args(SQL_DELETE_RANGE, 2, argtypes, values, nulls, false, 0) !=
      SPI_OK_DELETE)
    ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("falha no DELETE do range %s: %s", prefix,
                    SPI_result_code_string(SPI_result))));
}

/* Garante que todos os paths intermediários entre "/" e o documento existam */
static void ensure_intermediate_paths(SPIPlanPtr check, SPIPlanPtr insert,
                                      const char *doc_id)
{
  /* Garante que "/" existe */
  {
    Datum values[1];
    char nulls[1] = " ";
    values[0] = CStringGetTextDatum("/");
    if (SPI_execute_plan(check, values, nulls, true, 1) != SPI_OK_SELECT)
      ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                      errmsg("falha no SELECT check")));
    if (SPI_processed == 0)
      insert_node(insert, "/", 1, "{}");
  }

  char full_path[1024] = "/";
  char *copy = pstrdup(doc_id);
  if (!copy)
    return;

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

  for (int i = 0; i < token_count - 1; i++)
  {
    size_t cur = strlen(full_path);
    snprintf(full_path + cur, sizeof(full_path) - cur, "%s/", tokens[i]);

    Datum values[1];
    char nulls[1] = " ";
    values[0] = CStringGetTextDatum(full_path);
    if (SPI_execute_plan(check, values, nulls, true, 1) != SPI_OK_SELECT)
      ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                      errmsg("falha no SELECT check intermediário")));
    if (SPI_processed == 0)
      insert_node(insert, full_path, 1, "{}");
  }
}

/* ===========================================================================
 * VALUE FITS INLINE
 * ===========================================================================
 */
static bool value_fits_inline(yyjson_val *val, size_t max_inline_size)
{
  if (yyjson_is_num(val) || yyjson_is_bool(val))
    return max_inline_size > 0;

  if (yyjson_is_str(val))
  {
    const char *s = yyjson_get_str(val);
    size_t len = s ? strlen(s) : 0;
    return len <= max_inline_size;
  }

  if (yyjson_is_obj(val))
    return yyjson_obj_size(val) == 0;

  if (yyjson_is_arr(val))
    return yyjson_arr_size(val) == 0;

  if (yyjson_is_null(val))
    return true;

  return false;
}

/* ===========================================================================
 * FLATTEN_AND_INSERT (recursivo) — inline optimization + arrays
 * ===========================================================================
 */
static void flatten_value(SPIPlanPtr stmt, SPIPlanPtr upd_text, SPIPlanPtr upd_empty,
                          yyjson_val *node, const char *parent_path,
                          const char *revision, int revision_nr,
                          size_t max_inline_size);
static void flatten_array_as_object(SPIPlanPtr stmt, SPIPlanPtr upd_text,
                                    SPIPlanPtr upd_empty,
                                    yyjson_val *arr, const char *parent_path,
                                    const char *revision, int revision_nr,
                                    size_t max_inline_size);

/* Serializa um valor primitivo para text_value. Retorna string palloc'd
 * (nunca trunca). Caller deve pfree(). */
static char *serialize_primitive_value(yyjson_val *val, int *out_type)
{
  if (yyjson_is_str(val))
  {
    *out_type = TYPE_STRING;
    return json_escape_string_alloc(yyjson_get_str(val));
  }
  else if (yyjson_is_int(val))
  {
    *out_type = TYPE_NUMBER;
    char buf[64];
    snprintf(buf, sizeof(buf), "%lld", yyjson_get_sint(val));
    return pstrdup(buf);
  }
  else if (yyjson_is_real(val))
  {
    *out_type = TYPE_NUMBER;
    char buf[64];
    snprintf(buf, sizeof(buf), "%.17g", yyjson_get_real(val));
    return pstrdup(buf);
  }
  else if (yyjson_is_bool(val))
  {
    *out_type = TYPE_BOOLEAN;
    return pstrdup(yyjson_get_bool(val) ? "true" : "false");
  }
  else
  {
    *out_type = TYPE_EMPTY;
    return pstrdup("");
  }
}

static void update_node_text(SPIPlanPtr upd_plan, const char *path,
                             const char *text_value)
{
  Datum values[2];
  char nulls[2] = "  ";
  values[0] = CStringGetTextDatum(text_value);
  values[1] = CStringGetTextDatum(path);
  if (SPI_execute_plan(upd_plan, values, nulls, false, 0) != SPI_OK_UPDATE)
    ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                    errmsg("falha no UPDATE text_value")));
}

static void update_node_empty(SPIPlanPtr upd_plan, const char *path)
{
  Datum values[1];
  char nulls[1] = " ";
  values[0] = CStringGetTextDatum(path);
  if (SPI_execute_plan(upd_plan, values, nulls, false, 0) != SPI_OK_UPDATE)
    ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                    errmsg("falha no UPDATE empty")));
}

static void flatten_value(SPIPlanPtr stmt, SPIPlanPtr upd_text, SPIPlanPtr upd_empty,
                          yyjson_val *node, const char *parent_path,
                          const char *revision, int revision_nr,
                          size_t max_inline_size)
{
  char current_path[2048];

  if (yyjson_is_obj(node))
  {
    yyjson_mut_doc *inline_doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *inline_obj = yyjson_mut_obj(inline_doc);
    yyjson_mut_doc_set_root(inline_doc, inline_obj);
    bool has_inline = false;

    yyjson_obj_iter iter;
    yyjson_obj_iter_init(node, &iter);
    yyjson_val *key, *val;

    while ((key = yyjson_obj_iter_next(&iter)))
    {
      val = yyjson_obj_iter_get_val(key);
      const char *k = yyjson_get_str(key);

      snprintf(current_path, sizeof(current_path), "%s%s", parent_path, k);

      if (yyjson_is_null(val))
      {
        char del_path[2048];
        snprintf(del_path, sizeof(del_path), "%s/", current_path);
        delete_subtree(del_path);
        continue;
      }

      if (value_fits_inline(val, max_inline_size))
      {
        if (yyjson_is_str(val))
          yyjson_mut_obj_add_str(inline_doc, inline_obj, k, yyjson_get_str(val));
        else if (yyjson_is_int(val))
          yyjson_mut_obj_add_int(inline_doc, inline_obj, k, yyjson_get_sint(val));
        else if (yyjson_is_real(val))
          yyjson_mut_obj_add_real(inline_doc, inline_obj, k, yyjson_get_real(val));
        else if (yyjson_is_bool(val))
          yyjson_mut_obj_add_bool(inline_doc, inline_obj, k, yyjson_get_bool(val));
        else if (yyjson_is_null(val))
          yyjson_mut_obj_add_null(inline_doc, inline_obj, k);
        else if (yyjson_is_obj(val))
          yyjson_mut_obj_add_obj(inline_doc, inline_obj, k);
        else if (yyjson_is_arr(val))
          yyjson_mut_obj_add_arr(inline_doc, inline_obj, k);
        has_inline = true;
      }
      else
      {
        if (yyjson_is_obj(val))
        {
          strncat(current_path, "/", sizeof(current_path) - strlen(current_path) - 1);
          insert_node_rev(stmt, current_path, TYPE_OBJECT, NULL, revision, revision_nr);
          flatten_value(stmt, upd_text, upd_empty, val, current_path,
                        revision, revision_nr, max_inline_size);
        }
        else if (yyjson_is_arr(val))
        {
          strncat(current_path, "/", sizeof(current_path) - strlen(current_path) - 1);
          insert_node_rev(stmt, current_path, TYPE_ARRAY, NULL, revision, revision_nr);
          flatten_array_as_object(stmt, upd_text, upd_empty, val, current_path,
                                  revision, revision_nr, max_inline_size);
        }
        else
        {
          int prim_type;
          char *text_buf = serialize_primitive_value(val, &prim_type);
          insert_node_rev(stmt, current_path, prim_type, text_buf,
                          revision, revision_nr);
          pfree(text_buf);
        }
      }
    }

    const char *container_text = has_inline ? NULL : "{}";
    char *inline_json = NULL;

    if (has_inline)
    {
      inline_json = yyjson_mut_write(inline_doc, 0, NULL);
      if (inline_json)
        container_text = inline_json;
      else
        container_text = "{}";
    }

    update_node_text(upd_text, parent_path, container_text);

    if (inline_json)
      free(inline_json);

    yyjson_mut_doc_free(inline_doc);
  }
  else if (yyjson_is_arr(node))
  {
    flatten_array_as_object(stmt, upd_text, upd_empty, node, parent_path,
                            revision, revision_nr, max_inline_size);
  }
}

static void flatten_array_as_object(SPIPlanPtr stmt, SPIPlanPtr upd_text,
                                    SPIPlanPtr upd_empty,
                                    yyjson_val *arr, const char *parent_path,
                                    const char *revision, int revision_nr,
                                    size_t max_inline_size)
{
  yyjson_arr_iter iter;
  yyjson_arr_iter_init(arr, &iter);
  yyjson_val *val;
  char current_path[2048];
  char idx_key[64];
  int idx = 0;

  yyjson_mut_doc *inline_doc = NULL;
  yyjson_mut_val *inline_obj = NULL;

  while ((val = yyjson_arr_iter_next(&iter)))
  {
    snprintf(idx_key, sizeof(idx_key), "%d", idx);

    if (yyjson_is_null(val))
    {
      idx++;
      continue;
    }

    if (value_fits_inline(val, max_inline_size))
    {
      if (!inline_doc)
      {
        inline_doc = yyjson_mut_doc_new(NULL);
        inline_obj = yyjson_mut_obj(inline_doc);
        yyjson_mut_doc_set_root(inline_doc, inline_obj);
      }

      if (yyjson_is_str(val))
        yyjson_mut_obj_add_str(inline_doc, inline_obj, idx_key, yyjson_get_str(val));
      else if (yyjson_is_int(val))
        yyjson_mut_obj_add_int(inline_doc, inline_obj, idx_key, yyjson_get_sint(val));
      else if (yyjson_is_real(val))
        yyjson_mut_obj_add_real(inline_doc, inline_obj, idx_key, yyjson_get_real(val));
      else if (yyjson_is_bool(val))
        yyjson_mut_obj_add_bool(inline_doc, inline_obj, idx_key, yyjson_get_bool(val));
      else if (yyjson_is_obj(val))
        yyjson_mut_obj_add_obj(inline_doc, inline_obj, idx_key);
      else if (yyjson_is_arr(val))
        yyjson_mut_obj_add_arr(inline_doc, inline_obj, idx_key);
      idx++;
    }
    else
    {
      snprintf(current_path, sizeof(current_path), "%s%s", parent_path, idx_key);

      if (yyjson_is_obj(val))
      {
        strncat(current_path, "/", sizeof(current_path) - strlen(current_path) - 1);
        insert_node_rev(stmt, current_path, TYPE_OBJECT, NULL, revision, revision_nr);
        flatten_value(stmt, upd_text, upd_empty, val, current_path,
                      revision, revision_nr, max_inline_size);
      }
      else if (yyjson_is_arr(val))
      {
        strncat(current_path, "/", sizeof(current_path) - strlen(current_path) - 1);
        insert_node_rev(stmt, current_path, TYPE_ARRAY, NULL, revision, revision_nr);
        flatten_array_as_object(stmt, upd_text, upd_empty, val, current_path,
                                revision, revision_nr, max_inline_size);
      }
      else
      {
        int prim_type;
        char *text_buf = serialize_primitive_value(val, &prim_type);
        insert_node_rev(stmt, current_path, prim_type, text_buf,
                        revision, revision_nr);
        pfree(text_buf);
      }
      idx++;
    }
  }

  if (inline_doc)
  {
    char *inline_json = yyjson_mut_write(inline_doc, 0, NULL);
    if (inline_json)
    {
      update_node_text(upd_text, parent_path, inline_json);
      free(inline_json);
    }
    yyjson_mut_doc_free(inline_doc);
  }
  else
  {
    update_node_empty(upd_empty, parent_path);
  }
}

/* ===========================================================================
 * MAKE VALUE FROM STORAGE (type + text_value → yyjson mut_val)
 * ===========================================================================
 */
static yyjson_mut_val *make_value_from_storage(yyjson_mut_doc *doc,
                                               int type, const char *text_val)
{
  switch (type)
  {
  case TYPE_OBJECT:
  {
    yyjson_mut_val *obj = yyjson_mut_obj(doc);
    if (text_val && text_val[0] != '\0')
    {
      yyjson_doc *inline_doc = yyjson_read(text_val, strlen(text_val), 0);
      if (inline_doc)
      {
        yyjson_val *inline_root = yyjson_doc_get_root(inline_doc);
        if (inline_root && yyjson_is_obj(inline_root))
        {
          yyjson_obj_iter iter;
          yyjson_obj_iter_init(inline_root, &iter);
          yyjson_val *k, *v;
          while ((k = yyjson_obj_iter_next(&iter)))
          {
            v = yyjson_obj_iter_get_val(k);
            yyjson_mut_obj_add(obj,
                               yyjson_mut_strcpy(doc, yyjson_get_str(k)),
                               yyjson_val_mut_copy(doc, v));
          }
        }
        yyjson_doc_free(inline_doc);
      }
    }
    return obj;
  }
  case TYPE_ARRAY:
  {
    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    if (text_val && text_val[0] != '\0')
    {
      yyjson_doc *inline_doc = yyjson_read(text_val, strlen(text_val), 0);
      if (inline_doc)
      {
        yyjson_val *inline_root = yyjson_doc_get_root(inline_doc);
        if (inline_root && yyjson_is_obj(inline_root))
        {
          yyjson_obj_iter iter;
          yyjson_obj_iter_init(inline_root, &iter);
          yyjson_val *k, *v;
          while ((k = yyjson_obj_iter_next(&iter)))
          {
            v = yyjson_obj_iter_get_val(k);
            yyjson_mut_arr_append(arr, yyjson_val_mut_copy(doc, v));
          }
        }
        yyjson_doc_free(inline_doc);
      }
    }
    return arr;
  }
  case TYPE_NUMBER:
    if (text_val)
    {
      if (strchr(text_val, '.') || strchr(text_val, 'e') || strchr(text_val, 'E'))
        return yyjson_mut_real(doc, strtod(text_val, NULL));
      else
        return yyjson_mut_int(doc, atoll(text_val));
    }
    return yyjson_mut_int(doc, 0);
  case TYPE_BOOLEAN:
    return yyjson_mut_bool(doc, text_val && strcmp(text_val, "true") == 0);
  case TYPE_STRING:
  {
    if (text_val)
    {
      char *unesc = json_unescape_string_alloc(text_val);
      yyjson_mut_val *v = yyjson_mut_strcpy(doc, unesc);
      pfree(unesc);
      return v;
    }
    return yyjson_mut_strcpy(doc, "");
  }
  case TYPE_BIGINT:
  case TYPE_DATETIME:
  case TYPE_BINARY:
  case TYPE_REFERENCE:
  default:
    return yyjson_mut_strcpy(doc, text_val ? text_val : "");
  }
}

/* ===========================================================================
 * FUNÇÃO: SET_JSON (JSON -> Nodes, replace completo)
 * ===========================================================================
 */
PG_FUNCTION_INFO_V1(set_json);
Datum set_json(PG_FUNCTION_ARGS)
{
  if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
    ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                    errmsg("Arguments must not be NULL")));
  char *doc_id = text_to_cstring(PG_GETARG_TEXT_PP(0));
  char *json_str = text_to_cstring(PG_GETARG_TEXT_PP(1));
  size_t max_inline_size = DEFAULT_MAX_INLINE_SIZE;

  if (SPI_connect() != SPI_OK_CONNECT)
    ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                    errmsg("SPI_connect falhou")));

  Oid argtypes_insert[5] = {TEXTOID, INT4OID, TEXTOID, TEXTOID, INT4OID};
  Oid argtypes_check[1] = {TEXTOID};
  Oid argtypes_text[2] = {TEXTOID, TEXTOID};

  SPIPlanPtr insert_stmt = spi_prepare_checked(SQL_INSERT_NODE, 5, argtypes_insert, "insert");
  SPIPlanPtr check_stmt = spi_prepare_checked(SQL_CHECK_EXISTS, 1, argtypes_check, "check");
  SPIPlanPtr upd_text = spi_prepare_checked(SQL_UPDATE_TEXT, 2, argtypes_text, "update_text");
  SPIPlanPtr upd_empty = spi_prepare_checked(SQL_UPDATE_EMPTY, 1, argtypes_check, "update_empty");

  yyjson_doc *doc = yyjson_read(json_str, strlen(json_str), 0);
  if (!doc)
  {
    SPI_finish();
    ereport(ERROR, (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                    errmsg("Invalid JSON")));
  }
  yyjson_val *root = yyjson_doc_get_root(doc);

  char root_path[1024];
  normalize_path(doc_id, root_path, sizeof(root_path));

  char revision[UUID_STR_LEN];
  generate_uuid_v4(revision, sizeof(revision));

  delete_subtree(root_path);

  if (yyjson_is_null(root))
  {
    yyjson_doc_free(doc);
    SPI_freeplan(insert_stmt);
    SPI_freeplan(check_stmt);
    SPI_freeplan(upd_text);
    SPI_freeplan(upd_empty);
    SPI_finish();
    PG_RETURN_TEXT_P(cstring_to_text(revision));
  }

  ensure_intermediate_paths(check_stmt, insert_stmt, doc_id);

  int rev_nr = get_current_revision_nr(root_path) + 1;

  insert_node_rev(insert_stmt, root_path, TYPE_OBJECT, NULL, revision, rev_nr);

  flatten_value(insert_stmt, upd_text, upd_empty, root, root_path,
                revision, rev_nr, max_inline_size);

  yyjson_doc_free(doc);
  SPI_freeplan(insert_stmt);
  SPI_freeplan(check_stmt);
  SPI_freeplan(upd_text);
  SPI_freeplan(upd_empty);
  SPI_finish();

  PG_RETURN_TEXT_P(cstring_to_text(revision));
}

/* ===========================================================================
 * FUNÇÃO: UPDATE_JSON (JSON -> Nodes, deep merge)
 * ===========================================================================
 */
static yyjson_mut_val *deep_merge_values(yyjson_mut_doc *mut_doc,
                                         yyjson_val *current,
                                         yyjson_val *update)
{
  if (yyjson_is_null(update))
    return NULL;

  if (!yyjson_is_obj(current) || !yyjson_is_obj(update))
  {
    return yyjson_val_mut_copy(mut_doc, update);
  }

  yyjson_mut_val *result = yyjson_mut_obj(mut_doc);

  yyjson_obj_iter cur_iter;
  yyjson_obj_iter_init(current, &cur_iter);
  yyjson_val *cur_key, *cur_val;
  while ((cur_key = yyjson_obj_iter_next(&cur_iter)))
  {
    cur_val = yyjson_obj_iter_get_val(cur_key);
    const char *k = yyjson_get_str(cur_key);

    yyjson_val *upd_val = yyjson_obj_get(update, k);
    if (upd_val)
    {
      if (yyjson_is_null(upd_val))
      {
        continue;
      }
      if (yyjson_is_obj(cur_val) && yyjson_is_obj(upd_val))
      {
        yyjson_mut_val *merged = deep_merge_values(mut_doc, cur_val, upd_val);
        if (merged)
          yyjson_mut_obj_add(result, yyjson_mut_strcpy(mut_doc, k), merged);
      }
      else
      {
        yyjson_mut_obj_add(result, yyjson_mut_strcpy(mut_doc, k),
                           yyjson_val_mut_copy(mut_doc, upd_val));
      }
    }
    else
    {
      yyjson_mut_obj_add(result, yyjson_mut_strcpy(mut_doc, k),
                         yyjson_val_mut_copy(mut_doc, cur_val));
    }
  }

  yyjson_obj_iter upd_iter;
  yyjson_obj_iter_init(update, &upd_iter);
  yyjson_val *upd_key;
  while ((upd_key = yyjson_obj_iter_next(&upd_iter)))
  {
    const char *k = yyjson_get_str(upd_key);
    yyjson_val *upd_val = yyjson_obj_iter_get_val(upd_key);

    if (!yyjson_obj_get(current, k) && !yyjson_is_null(upd_val))
    {
      yyjson_mut_obj_add(result, yyjson_mut_strcpy(mut_doc, k),
                         yyjson_val_mut_copy(mut_doc, upd_val));
    }
  }

  return result;
}

PG_FUNCTION_INFO_V1(update_json);
Datum update_json(PG_FUNCTION_ARGS)
{
  if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
    ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                    errmsg("Arguments must not be NULL")));
  char *doc_id = text_to_cstring(PG_GETARG_TEXT_PP(0));
  char *json_str = text_to_cstring(PG_GETARG_TEXT_PP(1));
  size_t max_inline_size = DEFAULT_MAX_INLINE_SIZE;

  if (SPI_connect() != SPI_OK_CONNECT)
    ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                    errmsg("SPI_connect falhou")));

  Oid argtypes_insert[5] = {TEXTOID, INT4OID, TEXTOID, TEXTOID, INT4OID};
  Oid argtypes_check[1] = {TEXTOID};
  Oid argtypes_text[2] = {TEXTOID, TEXTOID};

  SPIPlanPtr insert_stmt = spi_prepare_checked(SQL_INSERT_NODE, 5, argtypes_insert, "insert");
  SPIPlanPtr check_stmt = spi_prepare_checked(SQL_CHECK_EXISTS, 1, argtypes_check, "check");
  SPIPlanPtr upd_text = spi_prepare_checked(SQL_UPDATE_TEXT, 2, argtypes_text, "update_text");
  SPIPlanPtr upd_empty = spi_prepare_checked(SQL_UPDATE_EMPTY, 1, argtypes_check, "update_empty");

  bool has_wildcard = path_has_wildcard(doc_id);
  char root_path[1024];
  if (has_wildcard)
    wildcard_fixed_prefix(doc_id, root_path, sizeof(root_path));
  else
    normalize_path(doc_id, root_path, sizeof(root_path));

  char extract_path[1024];
  path_without_trailing_slash(root_path, extract_path, sizeof(extract_path));

  /* Carrega o JSON atual via extract_json (função SQL) */
  yyjson_doc *current_doc = NULL;
  {
    Oid argtypes1[1] = {TEXTOID};
    Datum values[1];
    char nulls[1] = " ";
    values[0] = CStringGetTextDatum(extract_path);
    if (SPI_execute_with_args(SQL_EXTRACT_JSON, 1, argtypes1, values, nulls, true, 1) ==
        SPI_OK_SELECT)
    {
      if (SPI_processed > 0 && SPI_tuptable != NULL)
      {
        char *json_str_out = SPI_getvalue(SPI_tuptable->vals[0],
                                          SPI_tuptable->tupdesc, 1);
        if (json_str_out && strcmp(json_str_out, "null") != 0)
          current_doc = yyjson_read(json_str_out, strlen(json_str_out), 0);
      }
    }
  }

  yyjson_doc *update_doc = yyjson_read(json_str, strlen(json_str), 0);
  if (!update_doc)
  {
    if (current_doc)
      yyjson_doc_free(current_doc);
    SPI_finish();
    ereport(ERROR, (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                    errmsg("Invalid JSON in update")));
  }
  yyjson_val *update_root = yyjson_doc_get_root(update_doc);

  if (!yyjson_is_obj(update_root) && !yyjson_is_null(update_root))
  {
    yyjson_doc_free(update_doc);
    if (current_doc)
      yyjson_doc_free(current_doc);
    SPI_finish();
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("UPDATE only supports JSON objects or null")));
  }

  /* Se update é null, remove */
  if (yyjson_is_null(update_root))
  {
    yyjson_doc_free(update_doc);
    if (current_doc)
      yyjson_doc_free(current_doc);
    delete_subtree(root_path);
    char rev[UUID_STR_LEN];
    generate_uuid_v4(rev, sizeof(rev));
    SPI_freeplan(insert_stmt);
    SPI_freeplan(check_stmt);
    SPI_freeplan(upd_text);
    SPI_freeplan(upd_empty);
    SPI_finish();
    PG_RETURN_TEXT_P(cstring_to_text(rev));
  }

  yyjson_mut_doc *merge_doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *merged = NULL;

  if (has_wildcard)
  {
    if (current_doc)
    {
      yyjson_val *parent_root = yyjson_doc_get_root(current_doc);
      if (yyjson_is_obj(parent_root))
      {
        merged = yyjson_mut_obj(merge_doc);
        yyjson_obj_iter iter;
        yyjson_obj_iter_init(parent_root, &iter);
        yyjson_val *k, *v;
        while ((k = yyjson_obj_iter_next(&iter)))
        {
          v = yyjson_obj_iter_get_val(k);
          const char *key = yyjson_get_str(k);
          yyjson_mut_val *child_merged = deep_merge_values(merge_doc, v, update_root);
          if (child_merged)
            yyjson_mut_obj_add(merged, yyjson_mut_strcpy(merge_doc, key),
                               child_merged);
        }
      }
      yyjson_mut_doc_set_root(merge_doc, merged ? merged : yyjson_mut_obj(merge_doc));
    }
    else
    {
      yyjson_mut_doc_free(merge_doc);
      yyjson_doc_free(update_doc);
      SPI_freeplan(insert_stmt);
      SPI_freeplan(check_stmt);
      SPI_freeplan(upd_text);
      SPI_freeplan(upd_empty);
      SPI_finish();
      PG_RETURN_NULL();
    }
  }
  else
  {
    if (current_doc)
    {
      yyjson_val *current_root = yyjson_doc_get_root(current_doc);
      merged = deep_merge_values(merge_doc, current_root, update_root);
    }
    else
    {
      merged = yyjson_val_mut_copy(merge_doc, update_root);
    }
    yyjson_mut_doc_set_root(merge_doc, merged ? merged : yyjson_mut_obj(merge_doc));
  }

  char *merged_json = yyjson_mut_write(merge_doc, 0, NULL);
  yyjson_mut_doc_free(merge_doc);
  if (current_doc)
    yyjson_doc_free(current_doc);
  yyjson_doc_free(update_doc);

  if (!merged_json)
  {
    SPI_freeplan(insert_stmt);
    SPI_freeplan(check_stmt);
    SPI_freeplan(upd_text);
    SPI_freeplan(upd_empty);
    SPI_finish();
    ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                    errmsg("Failed to serialize merged JSON")));
  }

  yyjson_doc *final_doc = yyjson_read(merged_json, strlen(merged_json), 0);
  free(merged_json);
  if (!final_doc)
  {
    SPI_freeplan(insert_stmt);
    SPI_freeplan(check_stmt);
    SPI_freeplan(upd_text);
    SPI_freeplan(upd_empty);
    SPI_finish();
    ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                    errmsg("Failed to parse merged JSON")));
  }
  yyjson_val *final_root = yyjson_doc_get_root(final_doc);

  char revision[UUID_STR_LEN];
  generate_uuid_v4(revision, sizeof(revision));

  delete_subtree(root_path);

  char clean_doc_id[1024];
  path_without_trailing_slash(root_path, clean_doc_id, sizeof(clean_doc_id));
  const char *id_for_ensure = clean_doc_id;
  if (id_for_ensure[0] == '/')
    id_for_ensure++;
  ensure_intermediate_paths(check_stmt, insert_stmt, id_for_ensure);

  int rev_nr = get_current_revision_nr(root_path) + 1;
  insert_node_rev(insert_stmt, root_path, TYPE_OBJECT, NULL, revision, rev_nr);
  flatten_value(insert_stmt, upd_text, upd_empty, final_root, root_path,
                revision, rev_nr, max_inline_size);

  yyjson_doc_free(final_doc);
  SPI_freeplan(insert_stmt);
  SPI_freeplan(check_stmt);
  SPI_freeplan(upd_text);
  SPI_freeplan(upd_empty);
  SPI_finish();

  PG_RETURN_TEXT_P(cstring_to_text(revision));
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

  if (SPI_connect() != SPI_OK_CONNECT)
    ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                    errmsg("SPI_connect falhou")));

  char prefix[1024];
  normalize_path(input, prefix, sizeof(prefix));

  char upper[1024];
  strncpy(upper, prefix, sizeof(upper) - 1);
  upper[sizeof(upper) - 1] = '\0';
  size_t plen = strlen(upper);
  if (plen > 0)
    upper[plen - 1]++;

  Oid argtypes[2] = {TEXTOID, TEXTOID};
  SPIPlanPtr stmt = spi_prepare_checked(SQL_SELECT_RANGE, 2, argtypes, "extract");

  Datum values[2];
  char nulls[2] = "  ";
  values[0] = CStringGetTextDatum(prefix);
  values[1] = CStringGetTextDatum(upper);

  if (SPI_execute_plan(stmt, values, nulls, true, 0) != SPI_OK_SELECT)
  {
    SPI_freeplan(stmt);
    SPI_finish();
    ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                    errmsg("Falha no SELECT em extract_json")));
  }

  yyjson_mut_doc *mut_doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *root = NULL;

  typedef struct
  {
    char *path;
    yyjson_mut_val *val;
  } StackNode;
  StackNode stack[2048];
  int stack_top = 0;

  int rows_count = 0;
  int nrows = (int)SPI_processed;
  TupleDesc tupdesc = SPI_tuptable ? SPI_tuptable->tupdesc : NULL;

  for (int r = 0; r < nrows && SPI_tuptable; r++)
  {
    HeapTuple tuple = SPI_tuptable->vals[r];
    char *path = SPI_getvalue(tuple, tupdesc, 1);
    char *type_s = SPI_getvalue(tuple, tupdesc, 2);
    char *text_val = SPI_getvalue(tuple, tupdesc, 3);
    int type = type_s ? atoi(type_s) : 0;

    rows_count++;

    if (rows_count == 1)
    {
      root = make_value_from_storage(mut_doc, type, text_val);
      if (!root)
        root = yyjson_mut_obj(mut_doc);
      yyjson_mut_doc_set_root(mut_doc, root);
      if (stack_top < 2048)
        stack[stack_top++] = (StackNode){pstrdup(path ? path : ""), root};
    }
    else
    {
      yyjson_mut_val *parent = NULL;
      for (int i = stack_top - 1; i >= 0; i--)
      {
        size_t sp_len = strlen(stack[i].path);
        size_t slen = sp_len;
        while (slen > 0 && stack[i].path[slen - 1] == '/')
          slen--;
        if (strncmp(path, stack[i].path, slen) == 0 &&
            (path[slen] == '/' || path[slen] == '\0'))
        {
          parent = stack[i].val;
          break;
        }
      }
      if (!parent)
        parent = root;

      char key_buf[256];
      const char *key = key_buf;
      int path_end = (int)strlen(path);
      if (path_end > 0 && path[path_end - 1] == '/')
        path_end--;

      const char *last_slash = NULL;
      for (int si = path_end - 1; si >= 0; si--)
      {
        if (path[si] == '/')
        {
          last_slash = &path[si];
          break;
        }
      }
      if (last_slash)
      {
        int key_len = path_end - (int)(last_slash - path) - 1;
        if (key_len > 0 && key_len < (int)sizeof(key_buf) - 1)
        {
          memcpy(key_buf, last_slash + 1, key_len);
          key_buf[key_len] = '\0';
        }
        else
        {
          key = path;
        }
      }
      else
      {
        key = path;
      }

      yyjson_mut_val *child = make_value_from_storage(mut_doc, type, text_val);
      bool is_container = (type == TYPE_OBJECT || type == TYPE_ARRAY);

      if (yyjson_mut_is_arr(parent))
        yyjson_mut_arr_append(parent, child);
      else
        yyjson_mut_obj_add(parent, yyjson_mut_strcpy(mut_doc, key), child);

      if (is_container && stack_top < 2048)
        stack[stack_top++] = (StackNode){pstrdup(path ? path : ""), child};
    }
  }

  SPI_freeplan(stmt);

  for (int i = 0; i < stack_top; i++)
    pfree(stack[i].path);

  if (rows_count == 0)
  {
    yyjson_mut_doc_free(mut_doc);
    SPI_finish();
    PG_RETURN_NULL();
  }

  char *json_out = yyjson_mut_write(mut_doc, 0, NULL);
  yyjson_mut_doc_free(mut_doc);

  text *result = NULL;
  if (json_out)
  {
    result = fn_text_copy(json_out, fcinfo->flinfo->fn_mcxt);
    free(json_out);
  }
  SPI_finish();

  if (result)
    PG_RETURN_TEXT_P(result);
  else
    PG_RETURN_NULL();
}

/* ===========================================================================
 * QUERY: filtros, ordenação, paginação (compartilhado entre wildcard e não)
 * ===========================================================================
 */
typedef struct
{
  char key[256];
  char op[16];
  char compare[512];
  yyjson_val *compare_val;
  bool valid;
} QueryFilter;

typedef struct
{
  char key[256];
  bool ascending;
} QueryOrder;

static bool evaluate_filter(yyjson_val *obj, QueryFilter *filter)
{
  yyjson_val *val = obj;
  char key_copy[256];
  strncpy(key_copy, filter->key, sizeof(key_copy) - 1);
  key_copy[sizeof(key_copy) - 1] = '\0';

  char *part = strtok(key_copy, ".");
  while (part && val)
  {
    if (yyjson_is_obj(val))
      val = yyjson_obj_get(val, part);
    else
      val = NULL;
    part = strtok(NULL, ".");
  }

  if (!val)
  {
    if (strcmp(filter->op, "!exists") == 0)
      return true;
    if (strcmp(filter->op, "exists") == 0)
      return false;
    return false;
  }

  double num_val = 0;
  bool is_num = false;
  const char *str_val = NULL;

  if (yyjson_is_num(val))
  {
    num_val = yyjson_get_num(val);
    is_num = true;
  }
  else if (yyjson_is_bool(val))
  {
    num_val = yyjson_get_bool(val) ? 1 : 0;
    is_num = true;
  }
  else if (yyjson_is_str(val))
  {
    str_val = yyjson_get_str(val);
  }
  else if (yyjson_is_null(val))
  {
    if (strcmp(filter->op, "==") == 0 && strcmp(filter->compare, "null") == 0)
      return true;
    if (strcmp(filter->op, "!=") == 0 && strcmp(filter->compare, "null") != 0)
      return true;
    return false;
  }
  else
  {
    return false;
  }

  const char *op = filter->op;

  if (is_num && filter->compare_val && yyjson_is_num(filter->compare_val))
  {
    double cmp = yyjson_get_num(filter->compare_val);

    if (strcmp(op, "<") == 0)
      return num_val < cmp;
    if (strcmp(op, "<=") == 0)
      return num_val <= cmp;
    if (strcmp(op, "==") == 0)
      return num_val == cmp;
    if (strcmp(op, "!=") == 0)
      return num_val != cmp;
    if (strcmp(op, ">=") == 0)
      return num_val >= cmp;
    if (strcmp(op, ">") == 0)
      return num_val > cmp;
    if (strcmp(op, "between") == 0 && yyjson_is_arr(filter->compare_val))
    {
      yyjson_val *lo = yyjson_arr_get(filter->compare_val, 0);
      yyjson_val *hi = yyjson_arr_get(filter->compare_val, 1);
      if (lo && hi && yyjson_is_num(lo) && yyjson_is_num(hi))
        return num_val >= yyjson_get_num(lo) && num_val <= yyjson_get_num(hi);
    }
  }

  if (str_val != NULL)
  {
    if (strcmp(op, "==") == 0)
      return strcmp(str_val, filter->compare) == 0;
    if (strcmp(op, "!=") == 0)
      return strcmp(str_val, filter->compare) != 0;
    if (strcmp(op, "like") == 0)
    {
      size_t clen = strlen(filter->compare);
      size_t slen = strlen(str_val);
      if (clen == 0)
        return (slen == 0);
      if (filter->compare[0] == '%' && filter->compare[clen - 1] == '%')
      {
        size_t sub_len = clen - 2;
        char sub[256];
        if (sub_len < sizeof(sub))
        {
          memcpy(sub, filter->compare + 1, sub_len);
          sub[sub_len] = '\0';
          return strstr(str_val, sub) != NULL;
        }
        return false;
      }
      if (filter->compare[0] == '%')
      {
        const char *suffix = filter->compare + 1;
        size_t sl = strlen(str_val);
        size_t sul = strlen(suffix);
        return sl >= sul && strcmp(str_val + sl - sul, suffix) == 0;
      }
      if (filter->compare[clen - 1] == '%')
      {
        size_t prefix_len = clen - 1;
        if (prefix_len < 256)
        {
          char prefix[256];
          memcpy(prefix, filter->compare, prefix_len);
          prefix[prefix_len] = '\0';
          return strncmp(str_val, prefix, prefix_len) == 0;
        }
        return false;
      }
      return strcmp(str_val, filter->compare) == 0;
    }
    if (strcmp(op, "matches") == 0)
    {
      return strstr(str_val, filter->compare) != NULL;
    }
    if (strcmp(op, "in") == 0 && filter->compare_val && yyjson_is_arr(filter->compare_val))
    {
      size_t n = yyjson_arr_size(filter->compare_val);
      for (size_t i = 0; i < n; i++)
      {
        yyjson_val *item = yyjson_arr_get(filter->compare_val, i);
        if (yyjson_is_str(item) && strcmp(str_val, yyjson_get_str(item)) == 0)
          return true;
        if (yyjson_is_num(item) && is_num && yyjson_get_num(item) == num_val)
          return true;
      }
      return false;
    }
  }

  if (strcmp(op, "exists") == 0)
    return true;
  if (strcmp(op, "!exists") == 0)
    return false;

  return false;
}

typedef struct
{
  yyjson_val *val;
  size_t index;
} SortEntry;

static int compare_entries(const void *a, const void *b, void *arg)
{
  const SortEntry *ea = (const SortEntry *)a;
  const SortEntry *eb = (const SortEntry *)b;
  QueryOrder *orders = (QueryOrder *)arg;

  if (!orders)
    return 0;

  for (int i = 0; orders[i].key[0] != '\0'; i++)
  {
    yyjson_val *va = ea->val;
    yyjson_val *vb = eb->val;
    char key_copy[256];
    strncpy(key_copy, orders[i].key, sizeof(key_copy) - 1);
    key_copy[sizeof(key_copy) - 1] = '\0';

    char *part = strtok(key_copy, ".");
    while (part && va && vb)
    {
      if (yyjson_is_obj(va))
        va = yyjson_obj_get(va, part);
      else
        va = NULL;
      if (yyjson_is_obj(vb))
        vb = yyjson_obj_get(vb, part);
      else
        vb = NULL;
      part = strtok(NULL, ".");
    }

    double na = 0, nb = 0;
    bool a_num = false, b_num = false;
    const char *sa = NULL, *sb = NULL;

    if (va && yyjson_is_num(va))
    {
      na = yyjson_get_num(va);
      a_num = true;
    }
    else if (va && yyjson_is_str(va))
      sa = yyjson_get_str(va);
    if (vb && yyjson_is_num(vb))
    {
      nb = yyjson_get_num(vb);
      b_num = true;
    }
    else if (vb && yyjson_is_str(vb))
      sb = yyjson_get_str(vb);

    int cmp = 0;
    if (a_num && b_num)
      cmp = (na < nb) ? -1 : (na > nb) ? 1
                                       : 0;
    else if (sa && sb)
      cmp = strcmp(sa, sb);
    else if (a_num && !b_num)
      cmp = -1;
    else if (!a_num && b_num)
      cmp = 1;
    else
      cmp = 0;

    if (cmp != 0)
      return orders[i].ascending ? cmp : -cmp;
  }

  return (ea->index < eb->index) ? -1 : (ea->index > eb->index) ? 1
                                                                : 0;
}

/* ===========================================================================
 * HELPERS PARA WILDCARD COM CAPTURA DE VARIÁVEIS
 * ===========================================================================
 */
static bool extract_path_segment(const char *path, int seg_idx,
                                 char *out, size_t out_size)
{
  if (!path || !*path || out_size == 0)
    return false;
  out[0] = '\0';
  const char *p = path;
  if (*p == '/')
    p++;
  int current = 0;
  while (*p && current < seg_idx)
  {
    const char *end = strchr(p, '/');
    if (!end)
      return false;
    p = end + 1;
    current++;
  }
  if (!*p || current != seg_idx)
    return false;
  const char *end = strchr(p, '/');
  size_t len = end ? (size_t)(end - p) : strlen(p);
  if (len >= out_size)
    len = out_size - 1;
  memcpy(out, p, len);
  out[len] = '\0';
  return true;
}

static bool evaluate_var_filter(const char *captured_val, const QueryFilter *filter)
{
  if (!captured_val)
    return false;

  if (strcmp(filter->op, "==") == 0)
    return strcmp(captured_val, filter->compare) == 0;
  if (strcmp(filter->op, "!=") == 0)
    return strcmp(captured_val, filter->compare) != 0;
  if (strcmp(filter->op, "like") == 0)
    return strstr(captured_val, filter->compare) != NULL;
  return false;
}

/* ===========================================================================
 * DO_QUERY_WILDCARD
 * ===========================================================================
 */
static void do_query_wildcard(const char *pattern, const char *query_str,
                              char **out_result, MemoryContext fn_mcxt)
{
  yyjson_doc *query_doc = yyjson_read(query_str, strlen(query_str), 0);
  if (!query_doc)
    ereport(ERROR, (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                    errmsg("Invalid query JSON in wildcard query")));
  yyjson_val *query_root = yyjson_doc_get_root(query_doc);
  if (!query_root)
  {
    yyjson_doc_free(query_doc);
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("Empty query JSON")));
  }

  QueryFilter filters[64];
  int filter_count = 0;
  memset(filters, 0, sizeof(filters));

  yyjson_val *filters_val = yyjson_obj_get(query_root, "filters");
  if (filters_val && yyjson_is_arr(filters_val))
  {
    size_t n = yyjson_arr_size(filters_val);
    for (size_t i = 0; i < n && filter_count < 64; i++)
    {
      yyjson_val *f = yyjson_arr_get(filters_val, i);
      if (!yyjson_is_obj(f))
        continue;
      yyjson_val *key_v = yyjson_obj_get(f, "key");
      yyjson_val *op_v = yyjson_obj_get(f, "op");
      yyjson_val *cmp_v = yyjson_obj_get(f, "compare");

      if (key_v && yyjson_is_str(key_v) && op_v && yyjson_is_str(op_v))
      {
        strncpy(filters[filter_count].key, yyjson_get_str(key_v),
                sizeof(filters[filter_count].key) - 1);
        strncpy(filters[filter_count].op, yyjson_get_str(op_v),
                sizeof(filters[filter_count].op) - 1);
        if (cmp_v)
        {
          filters[filter_count].compare_val = cmp_v;
          if (yyjson_is_str(cmp_v))
            strncpy(filters[filter_count].compare, yyjson_get_str(cmp_v),
                    sizeof(filters[filter_count].compare) - 1);
          else if (yyjson_is_num(cmp_v))
            snprintf(filters[filter_count].compare,
                     sizeof(filters[filter_count].compare),
                     "%.17g", yyjson_get_num(cmp_v));
          else if (yyjson_is_bool(cmp_v))
            strcpy(filters[filter_count].compare,
                   yyjson_get_bool(cmp_v) ? "true" : "false");
        }
        filters[filter_count].valid = true;
        filter_count++;
      }
    }
  }

  QueryOrder orders[16];
  int order_count = 0;
  memset(orders, 0, sizeof(orders));

  yyjson_val *orders_val = yyjson_obj_get(query_root, "order");
  if (orders_val && yyjson_is_arr(orders_val))
  {
    size_t n = yyjson_arr_size(orders_val);
    for (size_t i = 0; i < n && order_count < 16; i++)
    {
      yyjson_val *o = yyjson_arr_get(orders_val, i);
      if (!yyjson_is_obj(o))
        continue;
      yyjson_val *k = yyjson_obj_get(o, "key");
      yyjson_val *a = yyjson_obj_get(o, "ascending");
      if (k && yyjson_is_str(k))
      {
        strncpy(orders[order_count].key, yyjson_get_str(k),
                sizeof(orders[order_count].key) - 1);
        orders[order_count].ascending = !a ||
                                        (yyjson_is_bool(a) && yyjson_get_bool(a)) ||
                                        (yyjson_is_int(a) && yyjson_get_int(a) == 1);
        order_count++;
      }
    }
  }

  yyjson_val *skip_val = yyjson_obj_get(query_root, "skip");
  yyjson_val *take_val = yyjson_obj_get(query_root, "take");
  size_t skip = skip_val && yyjson_is_int(skip_val) ? (size_t)yyjson_get_int(skip_val) : 0;
  size_t take = take_val && yyjson_is_int(take_val) ? (size_t)yyjson_get_int(take_val) : 0;
  if (!take_val) /* não especificado → retorna tudo; take:0 explícito → vazio */
    take = (size_t)-1;

  char fixed[1024];
  wildcard_fixed_prefix(pattern, fixed, sizeof(fixed));

  char upper[1024];
  make_upper_bound(fixed, upper, sizeof(upper));

  WildcardPattern wc_pat;
  wildcard_parse(pattern, &wc_pat);

  int var_capture_seg[64];
  char var_capture_name[64][256];
  int var_capture_count = 0;
  int last_wildcard_seg = -1;

  for (int i = 0; i < wc_pat.count; i++)
  {
    if (wc_pat.is_wildcard[i])
    {
      if (wc_pat.segments[i][0] == '$' && wc_pat.segments[i][1] != '\0')
      {
        if (var_capture_count < 64)
        {
          strncpy(var_capture_name[var_capture_count],
                  wc_pat.segments[i] + 1, 255);
          var_capture_name[var_capture_count][255] = '\0';
          var_capture_seg[var_capture_count] = i;
          var_capture_count++;
        }
      }
      last_wildcard_seg = i;
    }
  }

  QueryFilter field_filters[64];
  int field_filter_count = 0;
  QueryFilter var_filters[16];
  int var_filter_count = 0;

  for (int f = 0; f < filter_count; f++)
  {
    const char *k = filters[f].key;
    if (k[0] == '$' || strcmp(k, "{key}") == 0)
    {
      if (var_filter_count < 16)
        var_filters[var_filter_count++] = filters[f];
    }
    else
    {
      if (field_filter_count < 64)
        field_filters[field_filter_count++] = filters[f];
    }
  }

  /* Scan da tabela por paths que casam o padrão */
  Oid argtypes[2] = {TEXTOID, TEXTOID};
  SPIPlanPtr scan_stmt = spi_prepare_checked(SQL_SCAN_PATHS, 2, argtypes, "wildcard scan");

  Datum values[2];
  char nulls[2] = "  ";
  values[0] = CStringGetTextDatum(fixed);
  values[1] = CStringGetTextDatum(upper);

  if (SPI_execute_plan(scan_stmt, values, nulls, true, 0) != SPI_OK_SELECT)
  {
    SPI_freeplan(scan_stmt);
    yyjson_doc_free(query_doc);
    ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                    errmsg("Failed to scan wildcard paths")));
  }

  char **paths = NULL;
  int path_count = 0;
  int path_cap = 128;
  paths = (char **)palloc(path_cap * sizeof(char *));

  int nrows = (int)SPI_processed;
  TupleDesc tupdesc = SPI_tuptable ? SPI_tuptable->tupdesc : NULL;

  for (int r = 0; r < nrows && SPI_tuptable; r++)
  {
    HeapTuple tuple = SPI_tuptable->vals[r];
    char *p = SPI_getvalue(tuple, tupdesc, 1);
    if (!p)
      continue;
    if (!wildcard_matches(p, &wc_pat))
    {
      pfree(p);
      continue;
    }

    if (var_filter_count > 0)
    {
      bool passes = true;
      for (int vf = 0; vf < var_filter_count && passes; vf++)
      {
        const char *vkey = var_filters[vf].key;
        const char *captured = NULL;
        char cap_buf[256];

        if (strcmp(vkey, "{key}") == 0)
        {
          if (last_wildcard_seg >= 0)
          {
            if (extract_path_segment(p, last_wildcard_seg, cap_buf,
                                     sizeof(cap_buf)))
              captured = cap_buf;
          }
        }
        else if (vkey[0] == '$')
        {
          const char *vname = vkey + 1;
          for (int vd = 0; vd < var_capture_count; vd++)
          {
            if (strcmp(var_capture_name[vd], vname) == 0)
            {
              if (extract_path_segment(p, var_capture_seg[vd], cap_buf,
                                       sizeof(cap_buf)))
                captured = cap_buf;
              break;
            }
          }
        }

        if (!captured)
        {
          passes = false;
          break;
        }
        passes = evaluate_var_filter(captured, &var_filters[vf]);
      }
      if (!passes)
      {
        pfree(p);
        continue;
      }
    }

    if (path_count >= path_cap)
    {
      path_cap *= 2;
      paths = (char **)repalloc(paths, path_cap * sizeof(char *));
    }
    paths[path_count++] = pstrdup(p); /* copia — p pertence ao contexto SPI */
  }

  SPI_freeplan(scan_stmt);

  /* Para cada path, obtém o valor JSON via extract_json */
  yyjson_mut_doc *result_doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *result_arr = yyjson_mut_arr(result_doc);
  yyjson_mut_doc_set_root(result_doc, result_arr);

  SortEntry *entries = (SortEntry *)palloc((path_count > 0 ? path_count : 1) * sizeof(SortEntry));
  yyjson_doc **item_docs = (yyjson_doc **)palloc((path_count > 0 ? path_count : 1) * sizeof(yyjson_doc *));
  size_t valid = 0;

  Oid argtypes1[1] = {TEXTOID};
  SPIPlanPtr extract_stmt = spi_prepare_checked(SQL_EXTRACT_JSON, 1, argtypes1,
                                                "extract in wildcard");

  for (int i = 0; i < path_count; i++)
  {
    Datum v[1];
    char nul[1] = " ";
    v[0] = CStringGetTextDatum(paths[i]);
    if (SPI_execute_plan(extract_stmt, v, nul, true, 1) == SPI_OK_SELECT)
    {
      if (SPI_processed > 0 && SPI_tuptable)
      {
        char *json_str_out = SPI_getvalue(SPI_tuptable->vals[0],
                                          SPI_tuptable->tupdesc, 1);
        if (json_str_out && strcmp(json_str_out, "null") != 0)
        {
          yyjson_doc *item_doc = yyjson_read(json_str_out, strlen(json_str_out), 0);
          if (item_doc)
          {
            yyjson_val *rootv = yyjson_doc_get_root(item_doc);
            if (rootv)
            {
              entries[valid].val = rootv;
              entries[valid].index = valid;
              item_docs[valid] = item_doc;
              valid++;
            }
            else
            {
              yyjson_doc_free(item_doc);
            }
          }
        }
      }
    }
  }

  SPI_freeplan(extract_stmt);

  for (int i = 0; i < path_count; i++)
    pfree(paths[i]);
  pfree(paths);

  /* Aplica filtros de campo */
  SortEntry *filtered = NULL;
  size_t filtered_count = 0;

  if (valid == 0)
  {
    yyjson_mut_doc_free(result_doc);
    pfree(entries);
    pfree(item_docs);
    yyjson_doc_free(query_doc);
    *out_result = MemoryContextStrdup(fn_mcxt, "[]");
    return;
  }

  if (field_filter_count > 0)
  {
    filtered = (SortEntry *)palloc(valid * sizeof(SortEntry));
    for (size_t i = 0; i < valid; i++)
    {
      bool match = true;
      for (int f = 0; f < field_filter_count && match; f++)
      {
        if (!field_filters[f].valid)
          continue;
        match = evaluate_filter(entries[i].val, &field_filters[f]);
      }
      if (match)
      {
        filtered[filtered_count].val = entries[i].val;
        filtered[filtered_count].index = entries[i].index;
        filtered_count++;
      }
    }
  }
  else
  {
    filtered = entries;
    filtered_count = valid;
  }

  if (order_count > 0 && filtered_count > 1)
  {
    qsort_arg(filtered, filtered_count, sizeof(SortEntry), compare_entries, orders);
  }

  size_t start = skip < filtered_count ? skip : filtered_count;
  size_t end = (take < filtered_count - start) ? (start + take) : filtered_count;

  for (size_t i = start; i < end; i++)
  {
    if (filtered[i].val)
    {
      yyjson_mut_val *copy = yyjson_val_mut_copy(result_doc, filtered[i].val);
      if (copy)
        yyjson_mut_arr_append(result_arr, copy);
    }
  }

  char *json_out = yyjson_mut_write(result_doc, 0, NULL);
  if (json_out)
  {
    *out_result = MemoryContextStrdup(fn_mcxt, json_out);
    free(json_out);
  }
  else
  {
    *out_result = MemoryContextStrdup(fn_mcxt, "[]");
  }

  yyjson_mut_doc_free(result_doc);
  for (size_t i = 0; i < valid; i++)
    if (item_docs[i])
      yyjson_doc_free(item_docs[i]);
  pfree(item_docs);
  if (filtered != entries)
    pfree(filtered);
  pfree(entries);
  yyjson_doc_free(query_doc);
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

  if (SPI_connect() != SPI_OK_CONNECT)
    ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                    errmsg("SPI_connect falhou")));

  bool has_wildcard = path_has_wildcard(prefix_input);

  if (has_wildcard)
  {
    char *out = NULL;
    do_query_wildcard(prefix_input, query_str, &out, fcinfo->flinfo->fn_mcxt);
    SPI_finish();
    if (out)
    {
      text *result = fn_text_copy(out, fcinfo->flinfo->fn_mcxt);
      pfree(out);
      PG_RETURN_TEXT_P(result);
    }
    PG_RETURN_NULL();
  }

  char query_prefix[1024];
  normalize_path(prefix_input, query_prefix, sizeof(query_prefix));

  Oid argtypes1[1] = {TEXTOID};
  SPIPlanPtr extract_stmt = spi_prepare_checked(SQL_EXTRACT_JSON, 1, argtypes1, "extract");

  char extract_arg[1024];
  path_without_trailing_slash(query_prefix, extract_arg, sizeof(extract_arg));

  yyjson_doc *data_doc = NULL;
  {
    Datum values[1];
    char nulls[1] = " ";
    values[0] = CStringGetTextDatum(extract_arg);
    if (SPI_execute_plan(extract_stmt, values, nulls, true, 1) == SPI_OK_SELECT)
    {
      if (SPI_processed > 0 && SPI_tuptable)
      {
        char *json_str_out = SPI_getvalue(SPI_tuptable->vals[0],
                                          SPI_tuptable->tupdesc, 1);
        if (json_str_out && strcmp(json_str_out, "null") != 0)
          data_doc = yyjson_read(json_str_out, strlen(json_str_out), 0);
      }
    }
  }
  SPI_freeplan(extract_stmt);

  if (!data_doc)
  {
    SPI_finish();
    PG_RETURN_NULL();
  }

  yyjson_val *data_root = yyjson_doc_get_root(data_doc);

  yyjson_doc *query_doc = yyjson_read(query_str, strlen(query_str), 0);
  if (!query_doc)
  {
    yyjson_doc_free(data_doc);
    SPI_finish();
    ereport(ERROR, (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                    errmsg("Invalid query JSON")));
  }
  yyjson_val *query_root = yyjson_doc_get_root(query_doc);

  QueryFilter filters[64];
  int filter_count = 0;
  memset(filters, 0, sizeof(filters));

  yyjson_val *filters_val = yyjson_obj_get(query_root, "filters");
  if (filters_val && yyjson_is_arr(filters_val))
  {
    size_t n = yyjson_arr_size(filters_val);
    for (size_t i = 0; i < n && filter_count < 64; i++)
    {
      yyjson_val *f = yyjson_arr_get(filters_val, i);
      if (!yyjson_is_obj(f))
        continue;
      yyjson_val *key_v = yyjson_obj_get(f, "key");
      yyjson_val *op_v = yyjson_obj_get(f, "op");
      yyjson_val *cmp_v = yyjson_obj_get(f, "compare");

      if (key_v && yyjson_is_str(key_v) && op_v && yyjson_is_str(op_v))
      {
        strncpy(filters[filter_count].key, yyjson_get_str(key_v),
                sizeof(filters[filter_count].key) - 1);
        strncpy(filters[filter_count].op, yyjson_get_str(op_v),
                sizeof(filters[filter_count].op) - 1);
        if (cmp_v)
        {
          filters[filter_count].compare_val = cmp_v;
          if (yyjson_is_str(cmp_v))
            strncpy(filters[filter_count].compare, yyjson_get_str(cmp_v),
                    sizeof(filters[filter_count].compare) - 1);
          else if (yyjson_is_num(cmp_v))
            snprintf(filters[filter_count].compare,
                     sizeof(filters[filter_count].compare),
                     "%.17g", yyjson_get_num(cmp_v));
          else if (yyjson_is_bool(cmp_v))
            strcpy(filters[filter_count].compare,
                   yyjson_get_bool(cmp_v) ? "true" : "false");
        }
        filters[filter_count].valid = true;
        filter_count++;
      }
    }
  }

  QueryOrder orders[16];
  int order_count = 0;
  memset(orders, 0, sizeof(orders));

  yyjson_val *orders_val = yyjson_obj_get(query_root, "order");
  if (orders_val && yyjson_is_arr(orders_val))
  {
    size_t n = yyjson_arr_size(orders_val);
    for (size_t i = 0; i < n && order_count < 16; i++)
    {
      yyjson_val *o = yyjson_arr_get(orders_val, i);
      if (!yyjson_is_obj(o))
        continue;
      yyjson_val *k = yyjson_obj_get(o, "key");
      yyjson_val *a = yyjson_obj_get(o, "ascending");
      if (k && yyjson_is_str(k))
      {
        strncpy(orders[order_count].key, yyjson_get_str(k),
                sizeof(orders[order_count].key) - 1);
        orders[order_count].ascending = !a ||
                                        (yyjson_is_bool(a) && yyjson_get_bool(a)) ||
                                        (yyjson_is_int(a) && yyjson_get_int(a) == 1);
        order_count++;
      }
    }
  }

  yyjson_val *skip_val = yyjson_obj_get(query_root, "skip");
  yyjson_val *take_val = yyjson_obj_get(query_root, "take");
  size_t skip = skip_val && yyjson_is_int(skip_val) ? (size_t)yyjson_get_int(skip_val) : 0;
  size_t take = take_val && yyjson_is_int(take_val) ? (size_t)yyjson_get_int(take_val) : 0;
  if (!take_val) /* não especificado → retorna tudo; take:0 explícito → vazio */
    take = (size_t)-1;

  SortEntry *entries = NULL;
  size_t total = 0;

  if (yyjson_is_arr(data_root))
  {
    total = yyjson_arr_size(data_root);
    entries = (SortEntry *)palloc(total * sizeof(SortEntry));
    for (size_t i = 0; i < total; i++)
    {
      entries[i].val = yyjson_arr_get(data_root, i);
      entries[i].index = i;
    }
  }
  else if (yyjson_is_obj(data_root))
  {
    total = yyjson_obj_size(data_root);
    entries = (SortEntry *)palloc(total * sizeof(SortEntry));
    size_t i = 0;
    yyjson_obj_iter iter;
    yyjson_obj_iter_init(data_root, &iter);
    yyjson_val *k, *v;
    while ((k = yyjson_obj_iter_next(&iter)))
    {
      v = yyjson_obj_iter_get_val(k);
      if (i < total)
      {
        entries[i].val = v;
        entries[i].index = i;
        i++;
      }
    }
    total = i;
  }
  else
  {
    total = 1;
    entries = (SortEntry *)palloc(sizeof(SortEntry));
    entries[0].val = data_root;
    entries[0].index = 0;
  }

  SortEntry *filtered = NULL;
  size_t filtered_count = 0;

  if (filter_count > 0)
  {
    filtered = (SortEntry *)palloc(total * sizeof(SortEntry));
    for (size_t i = 0; i < total; i++)
    {
      bool match = true;
      for (int f = 0; f < filter_count && match; f++)
      {
        if (!filters[f].valid)
          continue;
        match = evaluate_filter(entries[i].val, &filters[f]);
      }
      if (match)
      {
        filtered[filtered_count].val = entries[i].val;
        filtered[filtered_count].index = entries[i].index;
        filtered_count++;
      }
    }
  }
  else
  {
    filtered = entries;
    filtered_count = total;
  }

  if (order_count > 0 && filtered_count > 1)
  {
    qsort_arg(filtered, filtered_count, sizeof(SortEntry), compare_entries, orders);
  }

  size_t start = skip < filtered_count ? skip : filtered_count;
  size_t end = (take < filtered_count - start) ? (start + take) : filtered_count;

  yyjson_mut_doc *result_doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *result_arr = yyjson_mut_arr(result_doc);
  yyjson_mut_doc_set_root(result_doc, result_arr);

  for (size_t i = start; i < end; i++)
  {
    if (filtered[i].val)
    {
      yyjson_mut_val *copy = yyjson_val_mut_copy(result_doc, filtered[i].val);
      if (copy)
        yyjson_mut_arr_append(result_arr, copy);
    }
  }

  char *json_out = yyjson_mut_write(result_doc, 0, NULL);
  text *result = NULL;
  if (json_out)
  {
    result = cstring_to_text(json_out);
    free(json_out);
  }

  yyjson_mut_doc_free(result_doc);
  if (filtered != entries)
    pfree(filtered);
  pfree(entries);
  yyjson_doc_free(data_doc);
  yyjson_doc_free(query_doc);
  SPI_finish();

  if (result)
    PG_RETURN_TEXT_P(result);
  PG_RETURN_NULL();
}

/* ===========================================================================
 * CSV MODULE: EXPORT_CSV + IMPORT_CSV
 * ===========================================================================
 */
#define CSV_HEADER "path,type,text_value\n"

/* Cria um text no contexto da função (sobrevive ao SPI_finish e à query). */
static text *fn_text_copy(const char *str, MemoryContext fn_mcxt)
{
  MemoryContext old = MemoryContextSwitchTo(fn_mcxt);
  text *result = cstring_to_text(str);
  MemoryContextSwitchTo(old);
  return result;
}

static StringInfo csv_escape(const char *value)
{
  StringInfo s = makeStringInfo();
  if (!value)
    return s;

  if (!strpbrk(value, ",\"\n\r"))
  {
    appendStringInfoString(s, value);
    return s;
  }

  appendStringInfoChar(s, '"');
  while (*value)
  {
    if (*value == '"')
      appendStringInfoString(s, "\"\"");
    else
      appendStringInfoChar(s, *value);
    value++;
  }
  appendStringInfoChar(s, '"');
  return s;
}

typedef struct
{
  char *path;
  int type;
  char *text_value;
} CsvRow;

static int compare_csv_rows(const void *a, const void *b)
{
  const CsvRow *ra = (const CsvRow *)a;
  const CsvRow *rb = (const CsvRow *)b;
  return strcmp(ra->path, rb->path);
}

static int parse_csv_line(const char *line, char ***out_fields)
{
  int cap = 8, count = 0;
  *out_fields = (char **)palloc(cap * sizeof(char *));

  const char *p = line;
  StringInfo buf = makeStringInfo();
  int in_quotes = 0;

  while (*p)
  {
    if (*p == '"')
    {
      if (in_quotes && *(p + 1) == '"')
      {
        appendStringInfoChar(buf, '"');
        p += 2;
        continue;
      }
      in_quotes = !in_quotes;
      p++;
    }
    else if (*p == ',' && !in_quotes)
    {
      if (count >= cap)
      {
        cap *= 2;
        *out_fields = (char **)repalloc(*out_fields, cap * sizeof(char *));
      }
      (*out_fields)[count++] = pstrdup(buf->data);
      resetStringInfo(buf);
      p++;
    }
    else
    {
      appendStringInfoChar(buf, *p);
      p++;
    }
  }

  if (count >= cap)
  {
    cap *= 2;
    *out_fields = (char **)repalloc(*out_fields, cap * sizeof(char *));
  }
  (*out_fields)[count++] = pstrdup(buf->data);

  return count;
}

static int split_csv_lines(const char *csv_text, const char ***out_lines, size_t **out_lengths)
{
  int cap = 128, count = 0;
  *out_lines = (const char **)palloc(cap * sizeof(const char *));
  *out_lengths = (size_t *)palloc(cap * sizeof(size_t));

  const char *start = csv_text;
  int in_quotes = 0;

  for (const char *p = csv_text;; p++)
  {
    if (*p == '"')
      in_quotes = !in_quotes;

    if ((*p == '\n' && !in_quotes) || *p == '\0')
    {
      if (count >= cap)
      {
        cap *= 2;
        *out_lines = (const char **)repalloc(*out_lines, cap * sizeof(const char *));
        *out_lengths = (size_t *)repalloc(*out_lengths, cap * sizeof(size_t));
      }
      (*out_lines)[count] = start;
      (*out_lengths)[count] = (size_t)(p - start);
      count++;
      start = p + 1;

      if (*p == '\0')
        break;
    }
  }

  return count;
}

static int parse_csv(const char *csv_text, CsvRow **out_rows)
{
  *out_rows = NULL;

  const char **lines = NULL;
  size_t *line_lengths = NULL;
  int line_count = split_csv_lines(csv_text, &lines, &line_lengths);
  if (line_count < 0)
    return -1;
  if (line_count == 0)
  {
    pfree(lines);
    pfree(line_lengths);
    return 0;
  }

  int cap = line_count > 1 ? line_count - 1 : 64;
  int count = 0;
  *out_rows = (CsvRow *)palloc(cap * sizeof(CsvRow));
  if (!*out_rows)
  {
    pfree(lines);
    pfree(line_lengths);
    return -1;
  }

  for (int i = 1; i < line_count; i++)
  {
    const char *line = lines[i];
    size_t llen = line_lengths[i];
    while (llen > 0 && line[llen - 1] == '\r')
      llen--;

    if (llen == 0)
      continue;

    char *line_copy = (char *)palloc(llen + 1);
    memcpy(line_copy, line, llen);
    line_copy[llen] = '\0';

    char **fields = NULL;
    int nf = parse_csv_line(line_copy, &fields);
    pfree(line_copy);

    if (nf < 2)
    {
      if (fields)
      {
        for (int j = 0; j < nf; j++)
          pfree(fields[j]);
        pfree(fields);
      }
      continue;
    }

    if (count >= cap)
    {
      cap = cap * 2 + 1;
      *out_rows = (CsvRow *)repalloc(*out_rows, cap * sizeof(CsvRow));
    }

    CsvRow *row = &(*out_rows)[count];
    memset(row, 0, sizeof(CsvRow));
    row->path = pstrdup(fields[0]);
    row->type = atoi(fields[1]);
    if (nf > 2 && fields[2] && fields[2][0] != '\0')
      row->text_value = pstrdup(fields[2]);

    for (int j = 0; j < nf; j++)
      pfree(fields[j]);
    pfree(fields);

    count++;
  }

  pfree(lines);
  pfree(line_lengths);
  return count;
}

static void free_csv_rows(CsvRow *rows, int count)
{
  if (!rows)
    return;
  for (int i = 0; i < count; i++)
  {
    if (rows[i].path)
      pfree(rows[i].path);
    if (rows[i].text_value)
      pfree(rows[i].text_value);
  }
  pfree(rows);
}

/* ===========================================================================
 * EXPORT_CSV (Nodes -> CSV)
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

  if (SPI_connect() != SPI_OK_CONNECT)
    ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                    errmsg("SPI_connect falhou")));

  char prefix[1024];
  normalize_path(input, prefix, sizeof(prefix));

  char upper[1024];
  snprintf(upper, sizeof(upper), "%s", prefix);
  size_t plen = strlen(upper);
  if (plen > 0)
    upper[plen - 1]++;

  Oid argtypes[2] = {TEXTOID, TEXTOID};
  SPIPlanPtr stmt = spi_prepare_checked(SQL_SELECT_RANGE, 2, argtypes, "export_csv");

  Datum values[2];
  char nulls[2] = "  ";
  values[0] = CStringGetTextDatum(prefix);
  values[1] = CStringGetTextDatum(upper);

  if (SPI_execute_plan(stmt, values, nulls, true, 0) != SPI_OK_SELECT)
  {
    SPI_freeplan(stmt);
    SPI_finish();
    ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                    errmsg("Falha no SELECT em export_csv")));
  }

  StringInfo out = makeStringInfo();
  appendStringInfoString(out, CSV_HEADER);

  int nrows = (int)SPI_processed;
  TupleDesc tupdesc = SPI_tuptable ? SPI_tuptable->tupdesc : NULL;

  for (int r = 0; r < nrows && SPI_tuptable; r++)
  {
    HeapTuple tuple = SPI_tuptable->vals[r];
    char *path = SPI_getvalue(tuple, tupdesc, 1);
    char *type_s = SPI_getvalue(tuple, tupdesc, 2);
    char *text_val = SPI_getvalue(tuple, tupdesc, 3);
    int type = type_s ? atoi(type_s) : 0;

    StringInfo epath = csv_escape(path);
    StringInfo etext = csv_escape(text_val);

    appendStringInfoString(out, epath->data);
    appendStringInfo(out, ",%d,", type);
    appendStringInfoString(out, etext->data);
    appendStringInfoChar(out, '\n');
  }

  text *result = fn_text_copy(out->data, fcinfo->flinfo->fn_mcxt);
  SPI_freeplan(stmt);
  SPI_finish();

  PG_RETURN_TEXT_P(result);
}

/* ===========================================================================
 * IMPORT_CSV (CSV -> Nodes)
 * ===========================================================================
 */
static yyjson_mut_val *csv_rows_to_tree(yyjson_mut_doc *doc,
                                        CsvRow *rows, int count)
{
  if (count == 0)
    return yyjson_mut_obj(doc);

  qsort(rows, (size_t)count, sizeof(CsvRow), compare_csv_rows);

  CsvRow *root_row = &rows[0];
  yyjson_mut_val *root = make_value_from_storage(doc, root_row->type,
                                                 root_row->text_value);
  if (!root)
    root = yyjson_mut_obj(doc);
  yyjson_mut_doc_set_root(doc, root);

  typedef struct
  {
    const char *path;
    yyjson_mut_val *val;
  } StackNode;

  StackNode stack[2048];
  int stack_top = 0;
  stack[stack_top++] = (StackNode){root_row->path, root};

  for (int i = 1; i < count; i++)
  {
    CsvRow *row = &rows[i];
    const char *path = row->path;
    int type = row->type;
    const char *text_val = row->text_value;

    yyjson_mut_val *parent = NULL;
    for (int si = stack_top - 1; si >= 0; si--)
    {
      size_t sp_len = strlen(stack[si].path);
      size_t slen = sp_len;
      while (slen > 0 && stack[si].path[slen - 1] == '/')
        slen--;
      if (strncmp(path, stack[si].path, slen) == 0 &&
          (path[slen] == '/' || path[slen] == '\0'))
      {
        parent = stack[si].val;
        break;
      }
    }
    if (!parent)
      parent = root;

    char key_buf[256];
    const char *key = key_buf;
    int path_end = (int)strlen(path);
    if (path_end > 0 && path[path_end - 1] == '/')
      path_end--;

    const char *last_slash = NULL;
    for (int si = path_end - 1; si >= 0; si--)
    {
      if (path[si] == '/')
      {
        last_slash = &path[si];
        break;
      }
    }
    if (last_slash)
    {
      int key_len = path_end - (int)(last_slash - path) - 1;
      if (key_len > 0 && key_len < (int)sizeof(key_buf) - 1)
      {
        memcpy(key_buf, last_slash + 1, (size_t)key_len);
        key_buf[key_len] = '\0';
      }
      else
      {
        key = path;
      }
    }
    else
    {
      key = path;
    }

    bool is_container = (type == TYPE_OBJECT || type == TYPE_ARRAY);
    yyjson_mut_val *child = make_value_from_storage(doc, type, text_val);

    if (yyjson_mut_is_arr(parent))
      yyjson_mut_arr_append(parent, child);
    else
      yyjson_mut_obj_add(parent, yyjson_mut_strcpy(doc, key), child);

    if (is_container && stack_top < 2048)
      stack[stack_top++] = (StackNode){path, child};
  }

  return root;
}

PG_FUNCTION_INFO_V1(import_csv);
Datum import_csv(PG_FUNCTION_ARGS)
{
  if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
    ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                    errmsg("Arguments must not be NULL")));
  char *prefix_input = text_to_cstring(PG_GETARG_TEXT_PP(0));
  char *csv_text = text_to_cstring(PG_GETARG_TEXT_PP(1));
  size_t max_inline_size = DEFAULT_MAX_INLINE_SIZE;

  if (SPI_connect() != SPI_OK_CONNECT)
    ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                    errmsg("SPI_connect falhou")));

  CsvRow *rows = NULL;
  int row_count = parse_csv(csv_text, &rows);

  if (row_count < 0)
  {
    SPI_finish();
    ereport(ERROR, (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                    errmsg("Failed to parse CSV")));
  }

  if (row_count == 0)
  {
    free_csv_rows(rows, 0);
    char rev[UUID_STR_LEN];
    generate_uuid_v4(rev, sizeof(rev));
    SPI_finish();
    PG_RETURN_TEXT_P(cstring_to_text(rev));
  }

  yyjson_mut_doc *tree_doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *tree_root = csv_rows_to_tree(tree_doc, rows, row_count);

  if (!tree_root)
  {
    free_csv_rows(rows, row_count);
    yyjson_mut_doc_free(tree_doc);
    SPI_finish();
    ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                    errmsg("Failed to build tree from CSV")));
  }

  char *json_str = yyjson_mut_write(tree_doc, 0, NULL);
  yyjson_mut_doc_free(tree_doc);
  free_csv_rows(rows, row_count);

  if (!json_str)
  {
    SPI_finish();
    ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                    errmsg("Failed to serialize tree")));
  }

  yyjson_doc *final_doc = yyjson_read(json_str, strlen(json_str), 0);
  free(json_str);
  if (!final_doc)
  {
    SPI_finish();
    ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                    errmsg("Failed to re-parse tree JSON")));
  }
  yyjson_val *final_root = yyjson_doc_get_root(final_doc);

  char root_path[1024];
  normalize_path(prefix_input, root_path, sizeof(root_path));

  char revision[UUID_STR_LEN];
  generate_uuid_v4(revision, sizeof(revision));

  Oid argtypes_insert[5] = {TEXTOID, INT4OID, TEXTOID, TEXTOID, INT4OID};
  Oid argtypes_check[1] = {TEXTOID};
  Oid argtypes_text[2] = {TEXTOID, TEXTOID};

  SPIPlanPtr insert_stmt = spi_prepare_checked(SQL_INSERT_NODE, 5, argtypes_insert, "insert");
  SPIPlanPtr check_stmt = spi_prepare_checked(SQL_CHECK_EXISTS, 1, argtypes_check, "check");
  SPIPlanPtr upd_text = spi_prepare_checked(SQL_UPDATE_TEXT, 2, argtypes_text, "update_text");
  SPIPlanPtr upd_empty = spi_prepare_checked(SQL_UPDATE_EMPTY, 1, argtypes_check, "update_empty");

  delete_subtree(root_path);

  char clean_id[1024];
  path_without_trailing_slash(root_path, clean_id, sizeof(clean_id));
  const char *id_for_ensure = clean_id;
  if (id_for_ensure[0] == '/')
    id_for_ensure++;
  ensure_intermediate_paths(check_stmt, insert_stmt, id_for_ensure);

  int rev_nr = get_current_revision_nr(root_path) + 1;

  insert_node_rev(insert_stmt, root_path, TYPE_OBJECT, NULL, revision, rev_nr);
  flatten_value(insert_stmt, upd_text, upd_empty, final_root, root_path,
                revision, rev_nr, max_inline_size);

  yyjson_doc_free(final_doc);
  SPI_freeplan(insert_stmt);
  SPI_freeplan(check_stmt);
  SPI_freeplan(upd_text);
  SPI_freeplan(upd_empty);
  SPI_finish();

  PG_RETURN_TEXT_P(cstring_to_text(revision));
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
