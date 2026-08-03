/*
 * =============================================================================
 * pg_utils.c — Camada de UTILS da extensão PostgreSQL
 * =============================================================================
 *
 * Funções puras, sem dependência de SPI ou de tabelas:
 *   - UUID v4 (via pg_strong_random)
 *   - Normalização de paths (convenção de trailing '/')
 *   - Wildcard pattern matching (* e $var) para query/update multi-nível
 *   - Escape/unescape de strings JSON
 *   - Conversão de valores yyjson ↔ storage (inline optimization)
 *
 * Dependências:
 *   - src/pg_types.h (constantes de tipo e structs)
 *   - yyjson.h
 */

#include "postgres.h"
#include "utils/builtins.h"
#include "port.h" /* pg_strong_random */
#include "yyjson.h"
#include "pg_types.h"
#include "pg_utils.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

/* ===========================================================================
 * UUID GENERATION (v4) — via pg_strong_random (seguro e sem dependências)
 * ===========================================================================
 */
void generate_uuid_v4(char *buf, size_t buf_size)
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
void normalize_path(const char *input, char *out_path, size_t out_size)
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

void path_without_trailing_slash(const char *normalized, char *out, size_t out_size)
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
bool path_has_wildcard(const char *path)
{
  if (!path)
    return false;
  return strchr(path, '*') != NULL || strchr(path, '$') != NULL;
}

void wildcard_parse(const char *path, WildcardPattern *pattern)
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

bool wildcard_matches(const char *path, const WildcardPattern *pattern)
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

void wildcard_fixed_prefix(const char *pattern_path, char *out, size_t out_size)
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

void make_upper_bound(const char *prefix, char *upper, size_t up_size)
{
  strncpy(upper, prefix, up_size - 1);
  upper[up_size - 1] = '\0';
  size_t len = strlen(upper);
  if (len > 0)
    upper[len - 1]++;
}

/* Extrai o segmento seg_idx (0-based) de um path normalizado. */
bool extract_path_segment(const char *path, int seg_idx,
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

/* ===========================================================================
 * HELPERS DE STRING JSON
 * ===========================================================================
 */
size_t json_escape_string(char *buf, size_t buf_size, const char *raw)
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

char *json_escape_string_alloc(const char *raw)
{
  size_t need = json_escape_string(NULL, 0, raw);
  char *buf = palloc(need);
  json_escape_string(buf, need, raw);
  return buf;
}

char *json_unescape_string_alloc(const char *quoted)
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
 * VALUE FITS INLINE
 * ===========================================================================
 */
bool value_fits_inline(yyjson_val *val, size_t max_inline_size)
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

/* Serializa um valor primitivo para text_value. Retorna string palloc'd
 * (nunca trunca). Caller deve pfree(). */
char *serialize_primitive_value(yyjson_val *val, int *out_type)
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

/* ===========================================================================
 * MAKE VALUE FROM STORAGE (type + text_value → yyjson mut_val)
 * ===========================================================================
 */
yyjson_mut_val *make_value_from_storage(yyjson_mut_doc *doc,
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
