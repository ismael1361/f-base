/*
 * he_utils.c — Camada de UTILS do motor SQLite (Clean Architecture).
 *
 * Funções puras: UUID, paths, wildcards, escape/unescape JSON,
 * decisão de inline e conversão yyjson ↔ storage.
 * Nenhuma função aqui executa SQL — apenas API sqlite3 utilitária
 * (randomness, mprintf) e yyjson.
 */

#include <sqlite3ext.h>
/* sqlite3_api é definida em he_extension.c (SQLITE_EXTENSION_INIT1/INIT2).
 * As macros do sqlite3ext.h usam esta variável — declarar extern aqui. */
extern const sqlite3_api_routines *sqlite3_api;

#include <yyjson.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

#include "he_types.h"
#include "he_utils.h"

/* ===========================================================================
 * UUID GENERATION (v4)
 * ===========================================================================
 */

/// Gera um UUID v4 string: "f6df933d8d624407aaf96bacdc0ad1fc"
void generate_uuid_v4(char *buf, size_t buf_size)
{
  if (buf_size < UUID_STR_LEN)
  {
    if (buf_size > 0)
      buf[0] = '\0';
    return;
  }
  unsigned char bytes[16];
  sqlite3_randomness(16, bytes);
  // Set version 4 (0100 in bits 12-15)
  bytes[6] = (bytes[6] & 0x0f) | 0x40;
  // Set variant (10xx in bits 6-7 of clock_seq_hi_and_reserved)
  bytes[8] = (bytes[8] & 0x3f) | 0x80;
  snprintf(buf, buf_size,
           "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
           bytes[0], bytes[1], bytes[2], bytes[3],
           bytes[4], bytes[5],
           bytes[6], bytes[7],
           bytes[8], bytes[9],
           bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
}

/* ===========================================================================
 * SHA-256 (FIPS 180-4) — usado no dedup de set (escrita idempotente)
 * ===========================================================================
 */

typedef struct
{
  uint32_t state[8];
  unsigned char buffer[64];
  uint64_t bitlen;
  unsigned int buflen;
} HeSha256Ctx;

static const uint32_t HE_SHA256_K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

#define HE_SHA256_ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void he_sha256_transform(HeSha256Ctx *c, const unsigned char data[64])
{
  uint32_t w[64];
  for (int i = 0; i < 16; i++)
    w[i] = ((uint32_t)data[i * 4] << 24) | ((uint32_t)data[i * 4 + 1] << 16) |
           ((uint32_t)data[i * 4 + 2] << 8) | (uint32_t)data[i * 4 + 3];
  for (int i = 16; i < 64; i++)
  {
    uint32_t s0 = HE_SHA256_ROTR(w[i - 15], 7) ^ HE_SHA256_ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = HE_SHA256_ROTR(w[i - 2], 17) ^ HE_SHA256_ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  uint32_t a = c->state[0], b = c->state[1], cc = c->state[2], d = c->state[3];
  uint32_t e = c->state[4], f = c->state[5], g = c->state[6], h = c->state[7];
  for (int i = 0; i < 64; i++)
  {
    uint32_t S1 = HE_SHA256_ROTR(e, 6) ^ HE_SHA256_ROTR(e, 11) ^ HE_SHA256_ROTR(e, 25);
    uint32_t ch = (e & f) ^ (~e & g);
    uint32_t t1 = h + S1 + ch + HE_SHA256_K[i] + w[i];
    uint32_t S0 = HE_SHA256_ROTR(a, 2) ^ HE_SHA256_ROTR(a, 13) ^ HE_SHA256_ROTR(a, 22);
    uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
    uint32_t t2 = S0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = cc;
    cc = b;
    b = a;
    a = t1 + t2;
  }
  c->state[0] += a;
  c->state[1] += b;
  c->state[2] += cc;
  c->state[3] += d;
  c->state[4] += e;
  c->state[5] += f;
  c->state[6] += g;
  c->state[7] += h;
}

static void he_sha256_init(HeSha256Ctx *c)
{
  c->state[0] = 0x6a09e667u;
  c->state[1] = 0xbb67ae85u;
  c->state[2] = 0x3c6ef372u;
  c->state[3] = 0xa54ff53au;
  c->state[4] = 0x510e527fu;
  c->state[5] = 0x9b05688cu;
  c->state[6] = 0x1f83d9abu;
  c->state[7] = 0x5be0cd19u;
  c->bitlen = 0;
  c->buflen = 0;
}

static void he_sha256_update(HeSha256Ctx *c, const unsigned char *data, size_t len)
{
  for (size_t i = 0; i < len; i++)
  {
    c->buffer[c->buflen++] = data[i];
    c->bitlen += 8;
    if (c->buflen == 64)
    {
      he_sha256_transform(c, c->buffer);
      c->buflen = 0;
    }
  }
}

static void he_sha256_final(HeSha256Ctx *c, unsigned char out[32])
{
  c->buffer[c->buflen++] = 0x80;
  if (c->buflen > 56)
  {
    while (c->buflen < 64)
      c->buffer[c->buflen++] = 0;
    he_sha256_transform(c, c->buffer);
    c->buflen = 0;
  }
  while (c->buflen < 56)
    c->buffer[c->buflen++] = 0;
  for (int i = 0; i < 8; i++)
    c->buffer[56 + i] = (unsigned char)(c->bitlen >> (56 - 8 * i));
  he_sha256_transform(c, c->buffer);
  for (int i = 0; i < 8; i++)
  {
    out[i * 4] = (unsigned char)(c->state[i] >> 24);
    out[i * 4 + 1] = (unsigned char)(c->state[i] >> 16);
    out[i * 4 + 2] = (unsigned char)(c->state[i] >> 8);
    out[i * 4 + 3] = (unsigned char)(c->state[i]);
  }
}

void sha256_hex_segments(const char **segments, const size_t *lens,
                         int n_segments, char *out_hex)
{
  HeSha256Ctx ctx;
  he_sha256_init(&ctx);
  for (int i = 0; i < n_segments; i++)
  {
    if (segments[i] && lens[i] > 0)
      he_sha256_update(&ctx, (const unsigned char *)segments[i], lens[i]);
  }
  unsigned char digest[32];
  he_sha256_final(&ctx, digest);
  static const char hexc[] = "0123456789abcdef";
  for (int i = 0; i < 32; i++)
  {
    out_hex[i * 2] = hexc[digest[i] >> 4];
    out_hex[i * 2 + 1] = hexc[digest[i] & 0x0F];
  }
  out_hex[64] = '\0';
}

/* ===========================================================================
 * PATH NORMALIZATION (para paths concretos, sem wildcard)
 * ===========================================================================
 */

/// Normaliza um path concreto: garante '/' inicial e '/' final.
/// NÃO suporta wildcards — use path_has_wildcard() primeiro.
///
/// Exemplos:
///   "/people"     → "/people/"
///   "people"      → "/people/"
///   "/people/"    → "/people/"
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

  // Remove trailing slashes
  while (len > 1 && input[len - 1] == '/')
    len--;

  if (len == 0)
  {
    snprintf(out_path, out_size, "/");
    return;
  }

  // Monta path normalizado com '/' inicial e final
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

/// Remove o trailing '/' de um path normalizado.
/// Ex: "/people/" → "/people"
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

/// Verifica se um path contém wildcards (* ou $)
bool path_has_wildcard(const char *path)
{
  if (!path)
    return false;
  return strchr(path, '*') != NULL || strchr(path, '$') != NULL;
}

/// Parseia um padrão de path em segmentos.
/// "/users/*/history/*" → ["users", "*", "history", "*"]
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

      // É wildcard se for "*" ou começar com "$"
      pattern->is_wildcard[pattern->count] =
          (cp == 1 && p[0] == '*') || (cp > 0 && p[0] == '$');

      pattern->count++;
    }

    if (!end)
      break;
    p = end + 1;
  }
}

/// Verifica se um path concreto casa com um padrão wildcard.
/// Ex: wildcard_matches("/users/ewout/posts", ["users","*","posts"]) → true
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

    // Pula segmentos vazios (ex: trailing slash)
    if (len == 0)
    {
      p = end ? end + 1 : p + strlen(p);
      continue;
    }

    if (pattern->is_wildcard[seg_idx])
    {
      // Wildcard casa qualquer segmento
    }
    else
    {
      // Segmento fixo: precisa casar exatamente
      if (strlen(pattern->segments[seg_idx]) != len ||
          strncmp(pattern->segments[seg_idx], p, len) != 0)
      {
        return false;
      }
    }

    seg_idx++;
    p = end ? end + 1 : p + strlen(p);
  }

  // Ignora trailing slash no path
  while (*p == '/')
    p++;

  return seg_idx == pattern->count && *p == '\0';
}

/// Extrai o prefixo fixo (parte antes do primeiro wildcard).
/// Ex: "/users/*/history/*" → "/users/"
/// Sempre retorna com '/' inicial e final.
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
        // first_wc aponta para o caractere '/' antes do wildcard,
        // ou para o início se não houver '/' anterior
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
    // Constrói prefixo garantindo '/' inicial e final
    size_t n = (size_t)(first_wc - pattern_path);
    char tmp[1024];
    size_t tmp_len = 0;

    // Garante '/' inicial
    if (n == 0 || pattern_path[0] != '/')
    {
      tmp[0] = '/';
      tmp_len = 1;
    }

    // Copia o prefixo
    if (n > 0)
    {
      if (n >= sizeof(tmp) - tmp_len - 1)
        n = sizeof(tmp) - tmp_len - 2;
      memcpy(tmp + tmp_len, pattern_path, n);
      tmp_len += n;
    }

    // Garante '/' final
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
    // Sem wildcards: normaliza
    normalize_path(pattern_path, out, out_size);
  }
}

/// Gera o upper bound para prefix range query a partir do path.
void make_upper_bound(const char *prefix, char *upper, size_t up_size)
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

/// Escapa uma string raw para o formato JSON com aspas: `"raw content"`
void json_escape_string(char *buf, size_t buf_size, const char *raw)
{
  size_t i = 0, j = 1;
  buf[0] = '"';
  while (raw[i] != '\0' && j < buf_size - 2)
  {
    switch (raw[i])
    {
    case '"':
      buf[j++] = '\\';
      if (j < buf_size - 1)
        buf[j++] = '"';
      break;
    case '\\':
      buf[j++] = '\\';
      if (j < buf_size - 1)
        buf[j++] = '\\';
      break;
    case '\n':
      buf[j++] = '\\';
      if (j < buf_size - 1)
        buf[j++] = 'n';
      break;
    case '\r':
      buf[j++] = '\\';
      if (j < buf_size - 1)
        buf[j++] = 'r';
      break;
    case '\t':
      buf[j++] = '\\';
      if (j < buf_size - 1)
        buf[j++] = 't';
      break;
    default:
      buf[j++] = raw[i];
      break;
    }
    i++;
  }
  buf[j++] = '"';
  buf[j] = '\0';
}

/// Faz o unescape de uma string JSON com aspas: `"escaped"` → raw
void json_unescape_string(const char *quoted, char *buf, size_t buf_size)
{
  size_t len = strlen(quoted);
  if (len < 2 || quoted[0] != '"' || quoted[len - 1] != '"')
  {
    strncpy(buf, quoted, buf_size - 1);
    buf[buf_size - 1] = '\0';
    return;
  }
  size_t i, j;
  for (i = 1, j = 0; i < len - 1 && j < buf_size - 1; i++)
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
        if (j < buf_size - 1)
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
}

/* ===========================================================================
 * VALUE FITS INLINE (alinhado com valueFitsInline do MDE JavaScript)
 * ===========================================================================
 */

/// Verifica se um valor JSON pode ser armazenado inline no nó pai
/// em vez de virar um nó filho dedicado.
/// Retorna true para: number, boolean, string curta, objeto/array vazio
bool value_fits_inline(void *v, size_t max_inline_size)
{
  yyjson_val *val = (yyjson_val *)v;
  if (yyjson_is_num(val) || yyjson_is_bool(val))
    return max_inline_size > 0;

  if (yyjson_is_str(val))
  {
    const char *s = yyjson_get_str(val);
    size_t len = s ? strlen(s) : 0;
    return len <= max_inline_size;
  }

  if (yyjson_is_obj(val))
  {
    // Objeto vazio → inline
    return yyjson_obj_size(val) == 0;
  }

  if (yyjson_is_arr(val))
  {
    // Array vazio → inline
    return yyjson_arr_size(val) == 0;
  }

  // null → inline (will be handled as delete)
  if (yyjson_is_null(val))
    return true;

  return false;
}

/* ===========================================================================
 * VALORES (yyjson ↔ storage)
 * ===========================================================================
 */

/// Serializa um valor primitivo yyjson como texto para text_value
void serialize_primitive_value(void *v, char *buf, size_t buf_size, int *out_type)
{
  yyjson_val *val = (yyjson_val *)v;
  if (yyjson_is_str(val))
  {
    *out_type = TYPE_STRING;
    json_escape_string(buf, buf_size, yyjson_get_str(val));
  }
  else if (yyjson_is_int(val))
  {
    *out_type = TYPE_NUMBER;
    snprintf(buf, buf_size, "%lld", yyjson_get_sint(val));
  }
  else if (yyjson_is_real(val))
  {
    *out_type = TYPE_NUMBER;
    snprintf(buf, buf_size, "%.17g", yyjson_get_real(val));
  }
  else if (yyjson_is_bool(val))
  {
    *out_type = TYPE_BOOLEAN;
    strcpy(buf, yyjson_get_bool(val) ? "true" : "false");
  }
  else
  {
    *out_type = TYPE_EMPTY;
    buf[0] = '\0';
  }
}

/// Cria um mut_val a partir do type e text_value armazenados.
/// Se type for container com inline children em text_value, faz merge.
void *make_value_from_storage(void *doc_ptr, int type, const char *text_val)
{
  yyjson_mut_doc *doc = (yyjson_mut_doc *)doc_ptr;
  switch (type)
  {
  case TYPE_OBJECT:
  {
    yyjson_mut_val *obj = yyjson_mut_obj(doc);
    // Se há inline children, faz merge
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
    // Se há inline children, adiciona como elementos
    if (text_val && text_val[0] != '\0')
    {
      yyjson_doc *inline_doc = yyjson_read(text_val, strlen(text_val), 0);
      if (inline_doc)
      {
        yyjson_val *inline_root = yyjson_doc_get_root(inline_doc);
        if (inline_root && yyjson_is_obj(inline_root))
        {
          // Inline children são armazenados como objeto no text_value
          // Precisamos ordenar pelas chaves e adicionar como elementos
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
      char unesc[2048];
      json_unescape_string(text_val, unesc, sizeof(unesc));
      return yyjson_mut_strcpy(doc, unesc);
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
